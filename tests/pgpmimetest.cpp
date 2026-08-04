// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

/// Checks PgpMime against fixed RFC 3156 structures. Self-contained: no
/// keyring, no network, no gpg — the point is that the classification is
/// decided by MIME structure alone, so it can be pinned down by vectors.
///
/// Exit 0 = every case matched.

#include "pgpmime.h"

#include <KMime/Message>

#include <QByteArray>
#include <QCoreApplication>
#include <QTextStream>

#include <memory>

namespace
{
QTextStream out(stdout);
int failures = 0;

std::shared_ptr<KMime::Message> parse(const QByteArray &wire)
{
    auto msg = std::make_shared<KMime::Message>();
    msg->setContent(KMime::CRLFtoLF(wire));
    msg->parse();
    return msg;
}

const char *kindName(PgpMime::Kind k)
{
    switch (k) {
    case PgpMime::Kind::None:
        return "None";
    case PgpMime::Kind::Encrypted:
        return "Encrypted";
    case PgpMime::Kind::Signed:
        return "Signed";
    case PgpMime::Kind::InlineEncrypted:
        return "InlineEncrypted";
    case PgpMime::Kind::Partial:
        return "Partial";
    }
    return "?";
}

void check(const char *name, const QByteArray &wire, PgpMime::Kind expected,
           const QByteArray &expectedCipher = {})
{
    const auto msg = parse(wire);
    const PgpMime::Structure s = PgpMime::classify(msg.get());
    bool ok = s.kind == expected;
    QByteArray gotCipher;
    if (ok && !expectedCipher.isEmpty()) {
        gotCipher = PgpMime::ciphertext(s).trimmed();
        ok = gotCipher == expectedCipher.trimmed();
    }
    out << (ok ? "ok   " : "FAIL ") << name << " — expected " << kindName(expected)
        << ", got " << kindName(s.kind) << '\n';
    if (!ok) {
        ++failures;
        if (!expectedCipher.isEmpty())
            out << "     ciphertext: got [" << gotCipher << "]\n";
    }
    out.flush();
}

// --- Vectors --------------------------------------------------------------

// RFC 3156 §4. The ciphertext part is the second one; the first only carries
// the version.
const QByteArray kEncrypted =
    "From: sender@example.com\r\n"
    "To: rcpt@example.com\r\n"
    "Subject: encrypted\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/encrypted; boundary=\"bnd\";\r\n"
    " protocol=\"application/pgp-encrypted\"\r\n"
    "\r\n"
    "--bnd\r\n"
    "Content-Type: application/pgp-encrypted\r\n"
    "\r\n"
    "Version: 1\r\n"
    "\r\n"
    "--bnd\r\n"
    "Content-Type: application/octet-stream\r\n"
    "\r\n"
    "-----BEGIN PGP MESSAGE-----\r\n"
    "\r\n"
    "hQEMAwAAAAAAAAAAAQ\r\n"
    "-----END PGP MESSAGE-----\r\n"
    "\r\n"
    "--bnd--\r\n";

// RFC 3156 §5.
const QByteArray kSigned =
    "From: sender@example.com\r\n"
    "Subject: signed\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/signed; boundary=\"bnd\"; micalg=pgp-sha256;\r\n"
    " protocol=\"application/pgp-signature\"\r\n"
    "\r\n"
    "--bnd\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "signed text\r\n"
    "\r\n"
    "--bnd\r\n"
    "Content-Type: application/pgp-signature\r\n"
    "\r\n"
    "-----BEGIN PGP SIGNATURE-----\r\n"
    "\r\n"
    "iHUEARYIAB0\r\n"
    "-----END PGP SIGNATURE-----\r\n"
    "\r\n"
    "--bnd--\r\n";

// multipart/encrypted without the protocol parameter. Not something we will
// decrypt, and not something we will call encrypted either.
const QByteArray kNoProtocol =
    "From: sender@example.com\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/encrypted; boundary=\"bnd\"\r\n"
    "\r\n"
    "--bnd\r\n"
    "Content-Type: application/pgp-encrypted\r\n"
    "\r\n"
    "Version: 1\r\n"
    "\r\n"
    "--bnd\r\n"
    "Content-Type: application/octet-stream\r\n"
    "\r\n"
    "-----BEGIN PGP MESSAGE-----\r\n"
    "-----END PGP MESSAGE-----\r\n"
    "\r\n"
    "--bnd--\r\n";

// The MIME-mixing shape: an encrypted part inside an ordinary multipart/mixed,
// with sender-supplied plaintext beside it. Decrypting the one part and
// showing it under a single badge is the spoof this refuses to enable.
const QByteArray kMixed =
    "From: sender@example.com\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/mixed; boundary=\"outer\"\r\n"
    "\r\n"
    "--outer\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "Your bank details are below, they are encrypted.\r\n"
    "\r\n"
    "--outer\r\n"
    "Content-Type: multipart/encrypted; boundary=\"bnd\";\r\n"
    " protocol=\"application/pgp-encrypted\"\r\n"
    "\r\n"
    "--bnd\r\n"
    "Content-Type: application/pgp-encrypted\r\n"
    "\r\n"
    "Version: 1\r\n"
    "\r\n"
    "--bnd\r\n"
    "Content-Type: application/octet-stream\r\n"
    "\r\n"
    "-----BEGIN PGP MESSAGE-----\r\n"
    "-----END PGP MESSAGE-----\r\n"
    "\r\n"
    "--bnd--\r\n"
    "\r\n"
    "--outer--\r\n";

// The shape plenty of clients actually send: the RFC 3156 structure wrapped in
// a multipart/mixed, with an empty text/plain beside it. Nothing is visible
// except the encrypted part, so there is nothing a decryption could be confused
// with — this must decrypt, not be refused as "partly encrypted".
const QByteArray kWrappedEncrypted =
    "From: sender@example.com\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/mixed; boundary=\"outer\"\r\n"
    "\r\n"
    "--outer\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "\r\n"
    "--outer\r\n"
    "Content-Type: multipart/encrypted; boundary=\"bnd\";\r\n"
    " protocol=\"application/pgp-encrypted\"\r\n"
    "\r\n"
    "--bnd\r\n"
    "Content-Type: application/pgp-encrypted\r\n"
    "\r\n"
    "Version: 1\r\n"
    "\r\n"
    "--bnd\r\n"
    "Content-Type: application/octet-stream; name=\"encrypted.asc\"\r\n"
    "\r\n"
    "-----BEGIN PGP MESSAGE-----\r\n"
    "\r\n"
    "hQEMAwAAAAAAAAAAAQ\r\n"
    "-----END PGP MESSAGE-----\r\n"
    "\r\n"
    "--bnd--\r\n"
    "\r\n"
    "--outer--\r\n";

// The same wrapper, but with an unencrypted attachment beside the encrypted
// part. Now there *is* something to confuse it with, so it stays refused.
const QByteArray kWrappedWithAttachment =
    "From: sender@example.com\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/mixed; boundary=\"outer\"\r\n"
    "\r\n"
    "--outer\r\n"
    "Content-Type: multipart/encrypted; boundary=\"bnd\";\r\n"
    " protocol=\"application/pgp-encrypted\"\r\n"
    "\r\n"
    "--bnd\r\n"
    "Content-Type: application/pgp-encrypted\r\n"
    "\r\n"
    "Version: 1\r\n"
    "\r\n"
    "--bnd\r\n"
    "Content-Type: application/octet-stream\r\n"
    "\r\n"
    "-----BEGIN PGP MESSAGE-----\r\n"
    "-----END PGP MESSAGE-----\r\n"
    "\r\n"
    "--bnd--\r\n"
    "\r\n"
    "--outer\r\n"
    "Content-Type: application/pdf; name=\"invoice.pdf\"\r\n"
    "Content-Disposition: attachment; filename=\"invoice.pdf\"\r\n"
    "\r\n"
    "%PDF-1.4 not encrypted at all\r\n"
    "\r\n"
    "--outer--\r\n";

// The shape that actually arrives from Gmail with a browser extension, and the
// one that exposed this: no multipart/encrypted anywhere. The armor is attached
// as a base64 text/plain file called encrypted.asc, beside an empty
// multipart/alternative whose HTML half is "<div dir="ltr"></div>". Nothing is
// visible except the armor, so it must decrypt.
const QByteArray kArmoredAttachment =
    "From: Michal Maly <mmmaly@gmail.com>\r\n"
    "To: Daniel Duris <dusoft@staznosti.sk>\r\n"
    "Subject: Re: test\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/mixed; boundary=\"outer\"\r\n"
    "\r\n"
    "--outer\r\n"
    "Content-Type: multipart/alternative; boundary=\"alt\"\r\n"
    "\r\n"
    "--alt\r\n"
    "Content-Type: text/plain; charset=\"UTF-8\"\r\n"
    "\r\n"
    "\r\n"
    "--alt\r\n"
    "Content-Type: text/html; charset=\"UTF-8\"\r\n"
    "\r\n"
    "<div dir=\"ltr\"></div>\r\n"
    "\r\n"
    "--alt--\r\n"
    "\r\n"
    "--outer\r\n"
    "Content-Type: text/plain; charset=\"US-ASCII\"; name=\"encrypted.asc\"\r\n"
    "Content-Disposition: attachment; filename=\"encrypted.asc\"\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "\r\n"
    // "-----BEGIN PGP MESSAGE-----\n\nhQEMAwAAAA\n-----END PGP MESSAGE-----\n"
    "LS0tLS1CRUdJTiBQR1AgTUVTU0FHRS0tLS0tCgpoUUVNQXdBQUFBCi0tLS0tRU5EIFBHUCBN\r\n"
    "RVNTQUdFLS0tLS0K\r\n"
    "\r\n"
    "--outer--\r\n";

// The same, but with a real message typed beside the encrypted attachment.
// Now there is something to confuse the decryption with, so it is refused.
const QByteArray kArmoredAttachmentWithText =
    "From: sender@example.com\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/mixed; boundary=\"outer\"\r\n"
    "\r\n"
    "--outer\r\n"
    "Content-Type: text/plain; charset=\"UTF-8\"\r\n"
    "\r\n"
    "Your bank details are in the attachment, they are encrypted.\r\n"
    "\r\n"
    "--outer\r\n"
    "Content-Type: text/plain; name=\"encrypted.asc\"\r\n"
    "Content-Disposition: attachment; filename=\"encrypted.asc\"\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "\r\n"
    "LS0tLS1CRUdJTiBQR1AgTUVTU0FHRS0tLS0tCgpoUUVNQXdBQUFBCi0tLS0tRU5EIFBHUCBN\r\n"
    "RVNTQUdFLS0tLS0K\r\n"
    "\r\n"
    "--outer--\r\n";

// Legacy inline armor that is the whole body.
const QByteArray kInline =
    "From: sender@example.com\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "-----BEGIN PGP MESSAGE-----\r\n"
    "\r\n"
    "hQEMAwAAAAAAAAAAAQ\r\n"
    "-----END PGP MESSAGE-----\r\n";

// The same armor with a covering note above it. Same refusal as kMixed, for
// the same reason: half the body would be decrypted and half not.
const QByteArray kInlineWithText =
    "From: sender@example.com\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "Hi! The secret part follows.\r\n"
    "\r\n"
    "-----BEGIN PGP MESSAGE-----\r\n"
    "\r\n"
    "hQEMAwAAAAAAAAAAAQ\r\n"
    "-----END PGP MESSAGE-----\r\n";

const QByteArray kPlain =
    "From: sender@example.com\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "Nothing to see here. Not even -----BEGIN PGP SIGNED MESSAGE-----\r\n";

const QByteArray kOrdinaryMultipart =
    "From: sender@example.com\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/alternative; boundary=\"bnd\"\r\n"
    "\r\n"
    "--bnd\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "hello\r\n"
    "\r\n"
    "--bnd\r\n"
    "Content-Type: text/html\r\n"
    "\r\n"
    "<p>hello</p>\r\n"
    "\r\n"
    "--bnd--\r\n";
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    check("RFC 3156 multipart/encrypted", kEncrypted, PgpMime::Kind::Encrypted,
          "-----BEGIN PGP MESSAGE-----\n\nhQEMAwAAAAAAAAAAAQ\n-----END PGP MESSAGE-----");
    check("RFC 3156 multipart/signed", kSigned, PgpMime::Kind::Signed);
    check("multipart/encrypted without protocol", kNoProtocol, PgpMime::Kind::Partial);
    check("encrypted part inside multipart/mixed, beside sender text", kMixed,
          PgpMime::Kind::Partial);
    check("encrypted part wrapped in multipart/mixed, nothing beside it",
          kWrappedEncrypted, PgpMime::Kind::Encrypted,
          "-----BEGIN PGP MESSAGE-----\n\nhQEMAwAAAAAAAAAAAQ\n-----END PGP MESSAGE-----");
    check("encrypted part beside an unencrypted attachment", kWrappedWithAttachment,
          PgpMime::Kind::Partial);
    check("armor attached as encrypted.asc, empty body beside it", kArmoredAttachment,
          PgpMime::Kind::InlineEncrypted,
          "-----BEGIN PGP MESSAGE-----\n\nhQEMAwAAAA\n-----END PGP MESSAGE-----");
    check("armor attached as encrypted.asc, sender text beside it",
          kArmoredAttachmentWithText, PgpMime::Kind::Partial);
    check("inline armor as the whole body", kInline, PgpMime::Kind::InlineEncrypted,
          "-----BEGIN PGP MESSAGE-----\n\nhQEMAwAAAAAAAAAAAQ\n-----END PGP MESSAGE-----");
    check("inline armor with plaintext around it", kInlineWithText,
          PgpMime::Kind::Partial);
    check("ordinary text message", kPlain, PgpMime::Kind::None);
    check("ordinary multipart/alternative", kOrdinaryMultipart, PgpMime::Kind::None);

    // The signed vector's parts must be the ones a verifier would need.
    {
        const auto msg = parse(kSigned);
        const PgpMime::Structure s = PgpMime::classify(msg.get());
        const bool ok = PgpMime::signature(s).contains("BEGIN PGP SIGNATURE")
            && PgpMime::signedOctets(s).contains("signed text")
            // The signature part must never be inside the signed octets: that
            // would be a signature over itself.
            && !PgpMime::signedOctets(s).contains("BEGIN PGP SIGNATURE");
        out << (ok ? "ok   " : "FAIL ") << "signed part and signature extracted\n";
        if (!ok)
            ++failures;
    }

    // The signed octets must come out of the message's own bytes, byte for
    // byte. This is the difference between a mismatch that means "this message
    // was altered" and one that means "we could not reproduce what was signed"
    // — the whole reason the OpenPGP badge has no "invalid" state until the
    // octets are known-original (doc/roadmap.md).
    {
        const auto msg = parse(kSigned);
        const PgpMime::Structure s = PgpMime::classify(msg.get());
        bool exact = false;
        const QByteArray sliced = PgpMime::signedOctets(kSigned, s, &exact);

        // What RFC 3156 says the signature covers: everything after the first
        // delimiter line, up to but not including the CRLF before the next one.
        const QByteArray expected =
            "Content-Type: text/plain\r\n"
            "\r\n"
            "signed text\r\n";

        const bool ok = exact && sliced == expected;
        out << (ok ? "ok   " : "FAIL ") << "signed octets sliced from the raw message\n";
        if (!ok) {
            ++failures;
            out << "     exact=" << exact << " got [" << sliced << "]\n"
                << "     want [" << expected << "]\n";
        }
    }

    // A message whose bytes are not available (a decrypted tree, or a cached
    // body reassembled from externalised attachments) must say so rather than
    // quietly hand back rebuilt octets that look just as authoritative.
    {
        const auto msg = parse(kSigned);
        const PgpMime::Structure s = PgpMime::classify(msg.get());
        bool exact = true;
        // Raw bytes from a different message: the boundary is not in them.
        PgpMime::signedOctets(kPlain, s, &exact);
        out << (!exact ? "ok   " : "FAIL ")
            << "octets reported inexact when the raw message does not match\n";
        if (exact)
            ++failures;
    }

    // --- Outgoing side (§6): what we build must be what we signed ---
    //
    // The whole point of buildSigned() is that the content part is copied in
    // verbatim, so the octets a verifier slices back out are the octets that
    // were signed. This closes the loop without a keyring: build a
    // multipart/signed, classify it, slice its signed octets, and require them
    // to equal what went in.
    {
        const QByteArray assembled =
            "From: me@example.com\r\n"
            "To: you@example.com\r\n"
            "Subject: outgoing\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=\"utf-8\"\r\n"
            "Content-Transfer-Encoding: quoted-printable\r\n"
            "\r\n"
            "hello, this is the body\r\n";
        const PgpMime::OutgoingParts parts = PgpMime::splitForCrypto(assembled);

        // Identity headers stay outside the wrapper; content headers go in.
        const bool split = parts.valid
            && parts.identityHeaders.contains("Subject: outgoing")
            && !parts.identityHeaders.contains("Content-Type")
            && parts.contentPart.startsWith("Content-Type: text/plain")
            && parts.contentPart.contains("Content-Transfer-Encoding: quoted-printable")
            && parts.contentPart.endsWith("hello, this is the body\r\n");
        out << (split ? "ok   " : "FAIL ") << "outgoing split: identity vs content headers\n";
        if (!split)
            ++failures;

        const QByteArray fakeSig = "-----BEGIN PGP SIGNATURE-----\r\n"
                                   "\r\nAAAA\r\n"
                                   "-----END PGP SIGNATURE-----\r\n";
        const QByteArray built =
            PgpMime::buildSigned(parts, fakeSig, QStringLiteral("pgp-sha256"));

        const auto reparsed = parse(built);
        const PgpMime::Structure s2 = PgpMime::classify(reparsed.get());
        bool exact = false;
        const QByteArray sliced = PgpMime::signedOctets(built, s2, &exact);

        const bool loop = s2.kind == PgpMime::Kind::Signed && exact
            && sliced == parts.contentPart
            // The Subject must still be readable without any key: RFC 3156
            // signs the body, never the identity headers.
            && built.contains("Subject: outgoing")
            && built.contains("micalg=pgp-sha256");
        out << (loop ? "ok   " : "FAIL ")
            << "outgoing signed message round-trips to the exact signed octets\n";
        if (!loop) {
            ++failures;
            out << "     kind=" << kindName(s2.kind) << " exact=" << exact << '\n'
                << "     sliced [" << sliced << "]\n     signed [" << parts.contentPart
                << "]\n";
        }

        // And the encrypted wrapper: classifiable, with the ciphertext intact
        // and the subject still outside it.
        const QByteArray cipher = "-----BEGIN PGP MESSAGE-----\r\n"
                                  "\r\nZZZZ\r\n"
                                  "-----END PGP MESSAGE-----\r\n";
        const QByteArray enc = PgpMime::buildEncrypted(parts, cipher);
        const auto encParsed = parse(enc);
        const PgpMime::Structure s3 = PgpMime::classify(encParsed.get());
        const bool encOk = s3.kind == PgpMime::Kind::Encrypted
            // Compared LF-normalised: KMime holds bodies in LF form, and the
            // wire form is restored on the way out, not on the way in.
            && KMime::CRLFtoLF(PgpMime::ciphertext(s3)).trimmed()
                == KMime::CRLFtoLF(cipher).trimmed()
            && enc.contains("Subject: outgoing");
        out << (encOk ? "ok   " : "FAIL ")
            << "outgoing encrypted message round-trips to the exact ciphertext\n";
        if (!encOk)
            ++failures;
    }

    // isEncrypted() is what the cache and index guards key off (§4): every
    // shape it returns true for must be one whose body never reaches the
    // plaintext SQLite index, and every shape it returns false for is indexed
    // as usual. Pinned here because a wrong answer either way is silent — an
    // unsearchable inbox, or an encryption that quietly stopped meaning
    // anything on this disk.
    {
        struct {
            const char *name;
            const QByteArray &wire;
            bool encrypted;
        } cases[] = {
            {"multipart/encrypted", kEncrypted, true},
            {"inline armor", kInline, true},
            {"multipart/signed", kSigned, false},
            {"partly encrypted", kMixed, false},
            {"plain text", kPlain, false},
        };
        for (const auto &c : cases) {
            const auto msg = parse(c.wire);
            const bool got = PgpMime::classify(msg.get()).isEncrypted();
            const bool ok = got == c.encrypted;
            out << (ok ? "ok   " : "FAIL ") << "index guard: " << c.name << " → "
                << (got ? "not indexed" : "indexed") << '\n';
            if (!ok)
                ++failures;
        }
    }

    // Head-only detection, used on the fetch path before a body exists — it is
    // what puts the lock in the message list without a second query.
    {
        const bool ok =
            PgpMime::kindFromHead("Content-Type: multipart/encrypted; x=y")
                == PgpMime::Kind::Encrypted
            && PgpMime::kindFromHead("content-type: MULTIPART/SIGNED")
                == PgpMime::Kind::Signed
            && PgpMime::kindFromHead("Content-Type: text/plain") == PgpMime::Kind::None;
        out << (ok ? "ok   " : "FAIL ") << "head-only detection\n";
        if (!ok)
            ++failures;
    }

    // Telling a decrypted MIME entity from decrypted *text*. Inline PGP
    // produces the latter, and parsing it as the former is how a successful
    // decryption ends up showing "no displayable text part".
    {
        struct {
            const char *name;
            QByteArray data;
            bool entity;
        } cases[] = {
            {"PGP/MIME plaintext is an entity",
             "Content-Type: text/plain; charset=\"utf-8\"\r\n"
             "\r\n"
             "the secret text\r\n",
             true},
            {"folded headers are still an entity",
             "Content-Type: multipart/mixed;\r\n boundary=\"x\"\r\n"
             "\r\n"
             "body\r\n",
             true},
            {"inline PGP plaintext is not",
             "Hello, this is the secret message.\r\n"
             "\r\n"
             "Second paragraph.\r\n",
             false},
            {"prose containing a colon is not",
             "Note: this is not a header, it is a sentence.\r\n"
             "\r\n"
             "and this is the body of the note\r\n",
             false},
            {"a single paragraph is not", "just one line of text\r\n", false},
        };
        for (const auto &c : cases) {
            const bool got = PgpMime::looksLikeMimeEntity(c.data);
            const bool ok = got == c.entity;
            out << (ok ? "ok   " : "FAIL ") << c.name << '\n';
            if (!ok)
                ++failures;
        }
    }

    // The stored column values (§8), which the message list reads directly.
    {
        const bool ok = PgpMime::storedKind(PgpMime::Kind::Encrypted)
                == PgpMime::StoredEncrypted
            && PgpMime::storedKind(PgpMime::Kind::InlineEncrypted) == PgpMime::StoredEncrypted
            && PgpMime::storedKind(PgpMime::Kind::Signed) == PgpMime::StoredSigned
            // A partly encrypted message gets no lock: the glyph would promise
            // a decryption that deliberately never happens.
            && PgpMime::storedKind(PgpMime::Kind::Partial) == PgpMime::StoredNone
            && PgpMime::storedKind(PgpMime::Kind::None) == PgpMime::StoredNone;
        out << (ok ? "ok   " : "FAIL ") << "stored column values\n";
        if (!ok)
            ++failures;
    }

    out << (failures == 0 ? "\nall vectors matched\n"
                          : QStringLiteral("\n%1 failure(s)\n").arg(failures));
    out.flush();
    return failures == 0 ? 0 : 1;
}
