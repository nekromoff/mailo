// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "dkimverifier.h"

#include <QCryptographicHash>
#include <QDnsLookup>
#include <QEventLoop>
#include <QList>
#include <QRegularExpression>
#include <QTimer>

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <chrono>
#include <memory>

namespace
{

// --- RFC 6376 §3.5: one parsed DKIM-Signature header ----------------------

struct Tag {
    QByteArray name;
    QByteArray value;
};

QList<Tag> parseTagList(const QByteArray &value)
{
    QList<Tag> tags;
    for (const QByteArray &part : value.split(';')) {
        const int eq = part.indexOf('=');
        if (eq < 0)
            continue;
        Tag t;
        t.name = part.left(eq).trimmed();
        t.value = part.mid(eq + 1).trimmed();
        if (!t.name.isEmpty())
            tags.append(t);
    }
    return tags;
}

QByteArray tagValue(const QList<Tag> &tags, const char *name)
{
    for (const Tag &t : tags) {
        if (t.name == name)
            return t.value;
    }
    return {};
}

/// Whitespace, including the CRLF of a folded continuation, removed entirely.
/// Used for base64 and hash tag values, which may be folded anywhere.
QByteArray stripWsp(const QByteArray &in)
{
    QByteArray out;
    out.reserve(in.size());
    for (const char c : in) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out.append(c);
    }
    return out;
}

// --- Canonicalization (RFC 6376 §3.4) -------------------------------------

/// Collapses WSP runs to one space and drops trailing WSP on the line.
/// A *leading* run collapses to one space and is kept: §3.4.4 reduces it, it
/// does not delete it, and the RFC's own example canonicalizes "<SP>C<SP>" to
/// "<SP>C". Header canonicalization strips what remains before the colon
/// separately, because there the RFC does say to delete it.
QByteArray relaxWhitespace(const QByteArray &in)
{
    QByteArray out;
    out.reserve(in.size());
    bool pendingSpace = false;
    for (const char c : in) {
        if (c == ' ' || c == '\t') {
            pendingSpace = true;
            continue;
        }
        if (pendingSpace)
            out.append(' ');
        pendingSpace = false;
        out.append(c);
    }
    return out; // a trailing run never flushes, so trailing WSP is dropped
}

} // namespace

namespace DkimCanon
{

/// §3.4.2 relaxed: lowercased name, unfolded value, collapsed WSP, no WSP
/// around the colon, one trailing CRLF.
QByteArray headerRelaxed(const QByteArray &name, const QByteArray &value)
{
    QByteArray unfolded;
    unfolded.reserve(value.size());
    for (int i = 0; i < value.size(); ++i) {
        const char c = value.at(i);
        if (c == '\r' && i + 1 < value.size() && value.at(i + 1) == '\n') {
            ++i; // the folding CRLF disappears; its following WSP stays as WSP
            continue;
        }
        if (c == '\n')
            continue;
        unfolded.append(c);
    }
    QByteArray v = relaxWhitespace(unfolded);
    while (v.endsWith(' '))
        v.chop(1);
    while (v.startsWith(' '))
        v.remove(0, 1);
    return name.toLower() + ':' + v + "\r\n";
}

/// §3.4.1 simple: the field exactly as it appeared, one trailing CRLF.
/// Takes the field's original bytes rather than a name/value pair — rebuilding
/// it as name + ':' + value would normalize away anything unusual around the
/// colon, and "exactly as they are in the message" is the whole contract here.
QByteArray headerSimple(const QByteArray &rawField)
{
    QByteArray out = rawField;
    while (out.endsWith('\n') || out.endsWith('\r'))
        out.chop(1);
    return out + "\r\n";
}

QByteArray bodySimple(const QByteArray &body)
{
    QByteArray b = body;
    // "Ignores all empty lines at the end of the message body."
    while (b.endsWith("\r\n\r\n"))
        b.chop(2);
    if (b.isEmpty())
        return QByteArrayLiteral("\r\n"); // an empty body canonicalizes to CRLF
    if (!b.endsWith("\r\n"))
        b += "\r\n";
    return b;
}

QByteArray bodyRelaxed(const QByteArray &body)
{
    QByteArray out;
    out.reserve(body.size());
    // Per-line: collapse WSP runs, drop trailing WSP.
    int pos = 0;
    while (pos < body.size()) {
        int eol = body.indexOf("\r\n", pos);
        const bool last = eol < 0;
        const QByteArray line = last ? body.mid(pos) : body.mid(pos, eol - pos);
        QByteArray v = relaxWhitespace(line);
        while (v.endsWith(' '))
            v.chop(1);
        out += v;
        out += "\r\n";
        if (last)
            break;
        pos = eol + 2;
    }
    // "Ignores all empty lines at the end of the message body."
    while (out.endsWith("\r\n\r\n"))
        out.chop(2);
    if (out == "\r\n")
        return {}; // a body that is entirely empty canonicalizes to nothing
    return out;
}

} // namespace DkimCanon

namespace
{

// --- Header block splitting ------------------------------------------------

struct Field {
    QByteArray name;
    QByteArray value; ///< everything after the colon, folding intact
    QByteArray raw;   ///< the field exactly as it appeared, no trailing CRLF
    bool used = false;
};

/// Splits a header block into fields, keeping each field's original bytes.
QList<Field> splitFields(const QByteArray &head)
{
    QList<Field> fields;
    int pos = 0;
    while (pos < head.size()) {
        int eol = head.indexOf("\r\n", pos);
        if (eol < 0)
            eol = head.size();
        // Absorb continuation lines (a following line starting with WSP).
        int end = eol;
        while (end + 2 < head.size() && (head.at(end + 2) == ' ' || head.at(end + 2) == '\t')) {
            int next = head.indexOf("\r\n", end + 2);
            if (next < 0) {
                end = head.size();
                break;
            }
            end = next;
        }
        const QByteArray raw = head.mid(pos, end - pos);
        const int colon = raw.indexOf(':');
        if (colon > 0) {
            Field f;
            f.name = raw.left(colon).trimmed();
            f.value = raw.mid(colon + 1);
            f.raw = raw;
            fields.append(f);
        }
        pos = end + 2;
    }
    return fields;
}

// --- DNS -------------------------------------------------------------------

/// Blocking TXT lookup. Safe here and only here: this runs on the verifier's
/// own thread, never the GUI thread.
QByteArray lookupDkimKey(const QString &name, bool *tempError)
{
    *tempError = false;
    QDnsLookup lookup(QDnsLookup::TXT, name);
    QEventLoop loop;
    QObject::connect(&lookup, &QDnsLookup::finished, &loop, &QEventLoop::quit);
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(std::chrono::seconds(10));
    lookup.lookup();
    loop.exec();

    if (!guard.isActive()) { // the timer fired first
        lookup.abort();
        *tempError = true;
        return {};
    }
    if (lookup.error() != QDnsLookup::NoError) {
        // NotFound is a permanent answer; anything else may succeed later.
        *tempError = lookup.error() != QDnsLookup::NotFoundError;
        return {};
    }
    const auto records = lookup.textRecords();
    if (records.isEmpty())
        return {};
    // A TXT record is a sequence of strings that must be concatenated.
    QByteArray joined;
    for (const QByteArray &chunk : records.first().values())
        joined += chunk;
    return joined;
}

// --- Crypto ----------------------------------------------------------------

struct PkeyDeleter {
    void operator()(EVP_PKEY *p) const { EVP_PKEY_free(p); }
};
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

struct MdCtxDeleter {
    void operator()(EVP_MD_CTX *c) const { EVP_MD_CTX_free(c); }
};
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

/// \a keyType is the k= tag: "rsa" (SubjectPublicKeyInfo DER) or "ed25519"
/// (raw 32-byte key).
PkeyPtr loadPublicKey(const QByteArray &der, const QByteArray &keyType)
{
    if (keyType == "ed25519") {
        if (der.size() != 32)
            return {};
        return PkeyPtr(EVP_PKEY_new_raw_public_key(
            EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(der.constData()),
            32));
    }
    const unsigned char *p = reinterpret_cast<const unsigned char *>(der.constData());
    return PkeyPtr(d2i_PUBKEY(nullptr, &p, der.size()));
}

bool verifySignature(EVP_PKEY *key, const QByteArray &keyType, const QByteArray &signedData,
                     const QByteArray &signature)
{
    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx)
        return false;
    const auto *sig = reinterpret_cast<const unsigned char *>(signature.constData());

    if (keyType == "ed25519") {
        // RFC 8463: Ed25519 signs the SHA-256 digest of the header data, not
        // the data itself.
        const QByteArray digest =
            QCryptographicHash::hash(signedData, QCryptographicHash::Sha256);
        if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key) != 1)
            return false;
        return EVP_DigestVerify(ctx.get(), sig, signature.size(),
                                reinterpret_cast<const unsigned char *>(digest.constData()),
                                digest.size())
            == 1;
    }
    if (EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key) != 1)
        return false;
    if (EVP_DigestVerifyUpdate(ctx.get(), signedData.constData(), signedData.size()) != 1)
        return false;
    return EVP_DigestVerifyFinal(ctx.get(), sig, signature.size()) == 1;
}

// --- Alignment -------------------------------------------------------------

/// DMARC-style relaxed alignment. Without a Public Suffix List we cannot
/// compute true organizational domains, so this accepts an exact match or a
/// parent/child relationship. That is stricter than DMARC relaxed in one
/// direction (sibling subdomains of one org domain are not matched) and never
/// looser, which is the safe way to be wrong.
bool domainsAligned(const QString &signing, const QString &from)
{
    const QString s = signing.toLower();
    const QString f = from.toLower();
    if (s.isEmpty() || f.isEmpty())
        return false;
    return s == f || f.endsWith(QLatin1Char('.') + s) || s.endsWith(QLatin1Char('.') + f);
}

} // namespace

// --- DkimVerifier ----------------------------------------------------------

struct DkimVerifier::Signature {
    QList<Tag> tags;
    QByteArray rawName;  ///< the header name as it appeared
    QByteArray rawValue; ///< the header value as it appeared, folding intact
    QByteArray rawField; ///< the whole field as it appeared, for simple canon
    QByteArray headerCanon = "simple";
    QByteArray bodyCanon = "simple";
};

bool DkimVerifier::prepare(const QByteArray &head, const QByteArray &body, const Signature &sig,
                           QByteArray *signedData, DkimResult *out) const
{
    const QByteArray algorithm = tagValue(sig.tags, "a").toLower();
    if (algorithm != "rsa-sha256" && algorithm != "ed25519-sha256") {
        out->status = DkimResult::PermError;
        // rsa-sha1 is the common case here, and RFC 8301 forbids it.
        out->detail = QObject::tr("unsupported or obsolete algorithm (%1)")
                          .arg(QString::fromLatin1(algorithm));
        return false;
    }

    // --- body hash ---
    QByteArray canonBody = sig.bodyCanon == "relaxed" ? DkimCanon::bodyRelaxed(body)
                                                      : DkimCanon::bodySimple(body);
    // l= limits how much of the body is signed. It is a weakness (anything
    // past the limit is unsigned and can be appended freely), so it is honored
    // but noted.
    const QByteArray lengthTag = tagValue(sig.tags, "l");
    bool truncated = false;
    if (!lengthTag.isEmpty()) {
        bool ok = false;
        const qsizetype limit = lengthTag.toLongLong(&ok);
        if (ok && limit >= 0 && limit < canonBody.size()) {
            canonBody = canonBody.left(limit);
            truncated = true;
        }
    }
    const QByteArray bodyHash =
        QCryptographicHash::hash(canonBody, QCryptographicHash::Sha256).toBase64();
    if (bodyHash != stripWsp(tagValue(sig.tags, "bh"))) {
        out->status = DkimResult::BodyMismatch;
        out->detail = QObject::tr("body hash does not match — the message was changed in "
                                  "transit, or our cached copy is not byte-identical to it");
        return false;
    }

    // --- the signed header set ---
    // h= is order-significant, and for a repeated field name each entry takes
    // the last not-yet-used instance, scanning upward (RFC 6376 §5.4.2).
    QList<Field> fields = splitFields(head);
    QByteArray data;
    const QByteArrayList wanted = tagValue(sig.tags, "h").split(':');
    for (const QByteArray &rawName : wanted) {
        const QByteArray name = rawName.trimmed().toLower();
        if (name.isEmpty())
            continue;
        for (int i = fields.size() - 1; i >= 0; --i) {
            Field &f = fields[i];
            if (f.used || f.name.toLower() != name)
                continue;
            data += sig.headerCanon == "relaxed" ? DkimCanon::headerRelaxed(f.name, f.value)
                                                 : DkimCanon::headerSimple(f.raw);
            f.used = true;
            break;
        }
        // A name in h= with no matching field contributes nothing, which is
        // how a signer commits to a header being absent.
    }

    // The DKIM-Signature field itself goes in last, with b= emptied and no
    // trailing CRLF (RFC 6376 §3.7).
    static const QRegularExpression bTagRe(QStringLiteral("(;?\\s*\\bb\\s*=)[^;]*"),
                                           QRegularExpression::CaseInsensitiveOption);
    auto stripB = [](const QByteArray &in) {
        QString s = QString::fromLatin1(in);
        s.replace(bTagRe, QStringLiteral("\\1"));
        return s.toLatin1();
    };
    QByteArray sigCanon = sig.headerCanon == "relaxed"
        ? DkimCanon::headerRelaxed(sig.rawName, stripB(sig.rawValue))
        : DkimCanon::headerSimple(stripB(sig.rawField));
    if (sigCanon.endsWith("\r\n"))
        sigCanon.chop(2);
    data += sigCanon;

    *signedData = data;
    if (truncated)
        out->detail = QObject::tr("only the first %1 bytes of the body are signed")
                          .arg(QString::fromLatin1(lengthTag));
    return true;
}

void DkimVerifier::verify(quint64 requestId, const QByteArray &rawMessageCrlf,
                          const QString &fromDomain)
{
    DkimResult result;

    const int split = rawMessageCrlf.indexOf("\r\n\r\n");
    if (split < 0) {
        result.status = DkimResult::PermError;
        result.detail = tr("message has no header/body separator");
        Q_EMIT finished(requestId, result);
        return;
    }
    const QByteArray head = rawMessageCrlf.left(split + 2); // keep the final CRLF
    const QByteArray body = rawMessageCrlf.mid(split + 4);

    // Collect every DKIM-Signature header; a message may carry several and
    // only one needs to verify.
    QList<Signature> signatures;
    for (const Field &f : splitFields(head)) {
        if (f.name.toLower() != "dkim-signature")
            continue;
        Signature s;
        s.rawName = f.name;
        s.rawValue = f.value;
        s.rawField = f.raw;
        s.tags = parseTagList(f.value);
        const QByteArray canon = tagValue(s.tags, "c").toLower();
        if (!canon.isEmpty()) {
            const int slash = canon.indexOf('/');
            s.headerCanon = slash < 0 ? canon : canon.left(slash);
            s.bodyCanon = slash < 0 ? QByteArrayLiteral("simple") : canon.mid(slash + 1);
        }
        signatures.append(s);
    }
    if (signatures.isEmpty()) {
        result.status = DkimResult::None;
        Q_EMIT finished(requestId, result);
        return;
    }

    // Try each signature; the first Pass that is also aligned wins outright.
    DkimResult best;
    best.status = DkimResult::PermError;
    for (const Signature &sig : signatures) {
        DkimResult r;
        r.domain = QString::fromLatin1(tagValue(sig.tags, "d")).toLower();
        r.selector = QString::fromLatin1(tagValue(sig.tags, "s"));
        r.aligned = domainsAligned(r.domain, fromDomain);
        if (r.domain.isEmpty() || r.selector.isEmpty()) {
            r.status = DkimResult::PermError;
            r.detail = tr("signature is missing its domain or selector");
            best = r;
            continue;
        }

        QByteArray signedData;
        if (!prepare(head, body, sig, &signedData, &r)) {
            best = r;
            continue;
        }

        bool tempError = false;
        const QByteArray record = m_testKeyRecord.isEmpty()
            ? lookupDkimKey(r.selector + QStringLiteral("._domainkey.") + r.domain, &tempError)
            : m_testKeyRecord;
        if (record.isEmpty()) {
            r.status = tempError ? DkimResult::TempError : DkimResult::PermError;
            r.detail = tempError ? tr("could not reach DNS to fetch the signing key")
                                 : tr("no public key published for %1").arg(r.selector);
            best = r;
            continue;
        }

        const QList<Tag> keyTags = parseTagList(record);
        const QByteArray keyType =
            tagValue(keyTags, "k").isEmpty() ? QByteArrayLiteral("rsa")
                                             : tagValue(keyTags, "k").toLower();
        const QByteArray keyData = stripWsp(tagValue(keyTags, "p"));
        if (keyData.isEmpty()) {
            r.status = DkimResult::PermError;
            r.detail = tr("the signing key has been revoked");
            best = r;
            continue;
        }
        PkeyPtr key = loadPublicKey(QByteArray::fromBase64(keyData), keyType);
        if (!key) {
            r.status = DkimResult::PermError;
            r.detail = tr("published key is unusable");
            best = r;
            continue;
        }

        const QByteArray signature = QByteArray::fromBase64(stripWsp(tagValue(sig.tags, "b")));
        if (verifySignature(key.get(), keyType, signedData, signature)) {
            r.status = DkimResult::Pass;
            if (r.detail.isEmpty()) {
                r.detail = r.aligned
                    ? tr("signed by %1").arg(r.domain)
                    // Valid but unaligned: the signer is not the From: domain,
                    // which is exactly what a forger's own valid signature
                    // looks like. Say so rather than showing a bare "pass".
                    : tr("signed by %1, which does not match the sender address")
                          .arg(r.domain);
            }
            if (r.aligned) {
                Q_EMIT finished(requestId, r);
                return;
            }
            best = r;
            continue;
        }
        r.status = DkimResult::Fail;
        r.detail = tr("signature does not match the published key");
        best = r;
    }

    Q_EMIT finished(requestId, best);
}
