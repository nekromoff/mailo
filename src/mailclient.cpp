// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "mailclient.h"
#include "attachmentstore.h"
#include "oauthhelper.h"
#include "pgpengine.h"
#include "pgpmime.h"
#include "spamheuristics.h"
#include "publicsuffixlist.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QLocale>
#include <QRandomGenerator>
#include <QSettings>
#include <QSqlQuery>
#include <QLoggingCategory>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <kimap/appendjob.h>
#include <kimap/capabilitiesjob.h>
#include <kimap/deletejob.h>
#include <kimap/expungejob.h>
#include <kimap/fetchjob.h>
#include <kimap/idlejob.h>
#include <kimap/listjob.h>
#include <kimap/loginjob.h>
#include <kimap/logoutjob.h>
#include <kimap/movejob.h>
#include <kimap/renamejob.h>
#include <kimap/searchjob.h>
#include <kimap/selectjob.h>
#include <kimap/session.h>
#include <kimap/statusjob.h>
#include <kimap/storejob.h>

#include <kmime/content.h>
#include <kmime/message.h>
#include <kmime/types.h>
#include <kmime/util.h>

#include <qt6keychain/keychain.h>

#include <ksmtp/loginjob.h>
#include <ksmtp/sendjob.h>
#include <ksmtp/session.h>

#include <QMimeDatabase>

#include <QTextDocumentFragment>

#include "viewersecurity.h"

#include <algorithm>

/// Depth-first search of the whole MIME tree — mainBodyPart() misses parts
/// nested in structures like multipart/mixed → multipart/related → text/html.
static KMime::Content *findPartByType(KMime::Content *root, const char *mimeType)
{
    if (const auto *ct = std::as_const(*root).contentType(); ct && ct->isMimeType(mimeType))
        return root;
    const auto children = root->contents();
    for (KMime::Content *child : children) {
        if (KMime::Content *found = findPartByType(child, mimeType))
            return found;
    }
    return nullptr;
}

static QSettings appSettings()
{
    return QSettings(QStringLiteral("mailo"), QStringLiteral("mailo"));
}

/// Walks the MIME tree in a fixed order, numbering parts "1", "2", "2.1", …
/// Split and restore both walk it the same way, so a part id written today
/// still identifies the same node when the message is read back.
static void walkParts(KMime::Content *node, const QString &prefix,
                      const std::function<void(KMime::Content *, const QString &)> &fn)
{
    const auto children = node->contents();
    for (int i = 0; i < children.size(); ++i) {
        const QString id = prefix.isEmpty() ? QString::number(i + 1)
                                            : prefix + QLatin1Char('.') + QString::number(i + 1);
        fn(children.at(i), id);
        walkParts(children.at(i), id, fn);
    }
}

/// True for a part whose payload is an attachment rather than the message
/// text — the only thing worth lifting out into the file store.
static bool isAttachmentPart(KMime::Content *part)
{
    if (!part->contents().isEmpty())
        return false; // a container, not a payload
    const auto *cd = std::as_const(*part).contentDisposition();
    if (cd && cd->disposition() == KMime::Headers::CDattachment)
        return true;
    // Inline images referenced by HTML mail are attachments for our purposes:
    // they are big, binary, and repeat across every message in a newsletter.
    return cd && !cd->filename().isEmpty();
}

/// Replaces every large attachment payload with an empty body, returning the
/// parts that were lifted out. The message keeps all of its headers — notably
/// Authentication-Results, which is what the SPF/DKIM display reads — so the
/// stub stays a valid, self-describing MIME message.
static QList<MailStore::PartRef> stripAttachments(KMime::Message *msg)
{
    QList<MailStore::PartRef> lifted;
    walkParts(msg, QString(), [&lifted](KMime::Content *part, const QString &id) {
        if (!isAttachmentPart(part))
            return;
        const QByteArray decoded = part->decodedBody();
        if (decoded.size() < AttachmentStore::kExternalizeThreshold)
            return; // small enough that a file of its own would cost more
        const AttachmentStore::Stored stored = AttachmentStore::put(decoded);
        if (stored.hash.isEmpty())
            return; // could not write it; leave the payload where it is
        MailStore::PartRef ref;
        ref.partId = id;
        ref.hash = stored.hash;
        ref.size = stored.size;
        ref.stored = stored.stored;
        ref.codec = stored.codec;
        const auto *cd = std::as_const(*part).contentDisposition();
        ref.filename = cd ? cd->filename() : QString();
        if (const auto *ct = std::as_const(*part).contentType())
            ref.mime = QString::fromLatin1(ct->mimeType());
        lifted.append(ref);
        part->setBody({});
        lifted.last().partId = id;
    });
    return lifted;
}

/// Confirms that a stub plus its stored payloads reproduces the original
/// parts. Used before the migration overwrites a cached message: the payload
/// has just made a round trip through hashing, zstd and the filesystem, and
/// the original bytes are about to be gone.
static bool verifyRoundTrip(const QByteArray &stub, const QList<MailStore::PartRef> &parts,
                            QString *reason);

/// Puts the payloads back into a parsed stub. Bodies are stored decoded, so
/// the transfer encoding is rewritten to match rather than re-encoding to
/// base64: every consumer reads decodedContent(), and this keeps the read
/// path allocation-cheap. A payload missing from disk leaves that part empty,
/// which the caller treats as a cache miss.
static bool restoreAttachments(KMime::Message *msg, const QList<MailStore::PartRef> &parts)
{
    if (parts.isEmpty())
        return true;
    QHash<QString, const MailStore::PartRef *> byId;
    for (const auto &p : parts)
        byId.insert(p.partId, &p);
    bool complete = true;
    walkParts(msg, QString(), [&byId, &complete](KMime::Content *part, const QString &id) {
        const auto it = byId.constFind(id);
        if (it == byId.cend())
            return;
        const QByteArray payload = AttachmentStore::get((*it)->hash, (*it)->codec);
        if (payload.isEmpty()) {
            complete = false;
            return;
        }
        if (auto *cte = part->contentTransferEncoding())
            cte->setEncoding(KMime::Headers::CEbinary);
        part->setBody(payload);
    });
    return complete;
}

static bool verifyRoundTrip(const QByteArray &stub, const QList<MailStore::PartRef> &parts,
                            QString *reason)
{
    KMime::Message check;
    check.setContent(KMime::CRLFtoLF(stub));
    check.parse();
    if (!restoreAttachments(&check, parts)) {
        *reason = QStringLiteral("a payload could not be read back from disk");
        return false;
    }
    QHash<QString, qint64> expect;
    for (const auto &p : parts)
        expect.insert(p.partId, p.size);
    bool ok = true;
    walkParts(&check, QString(), [&expect, &ok, reason](KMime::Content *part, const QString &id) {
        const auto it = expect.constFind(id);
        if (it == expect.cend())
            return;
        const qint64 got = part->decodedBody().size();
        if (got != it.value()) {
            // Back, but not with the bytes we stored.
            *reason = QStringLiteral("part %1 came back %2 bytes, expected %3")
                          .arg(id).arg(got).arg(it.value());
            ok = false;
        }
    });
    return ok;
}

static const auto kWalletService = QStringLiteral("mailo");
static const auto kWalletKey = QStringLiteral("imap-password");

// Background-sync pacing. Headers are cheap, bodies move real bandwidth, so
// they are fetched in modest windows with a deliberate pause between windows
// rather than back-to-back. This keeps sustained backfill under the rate/
// bandwidth limits that make servers like Gmail drop the connection. The
// adaptive backoff (backoffBackfill) still layers on top when a server pushes
// back regardless.
static constexpr int kHeaderWindow = 200;   ///< headers fetched per request
static constexpr int kHeaderPauseMs = 400;  ///< pause between header windows
static constexpr int kBodyPauseMs = 600;    ///< pause between body-fetch batches

// Truncated exponential backoff with full jitter, applied when the server
// throttles or drops the backfill (see backoffBackfill). Wait time on attempt
// n (1-based) = min(2^n seconds + [0,1000) ms jitter, cap). After the max
// number of attempts the backfill pauses until the next (re)connect or folder
// change, rather than retrying forever.
static constexpr int kBackoffBaseMs = 1000;   ///< 2^1 * 500 → ~1 s first wait
static constexpr int kBackoffCapMs = 64000;   ///< per-attempt ceiling (64 s)
static constexpr int kBackoffJitterMs = 1000; ///< full jitter added on top
static constexpr int kBackoffMaxAttempts = 8; ///< then pause syncing

/// True when the IMAP error text carries a throttling response code such as
/// Gmail's [THROTTLED] or [TOO-MANY-SIMULTANEOUS-CONNECTIONS]. Case-folded so
/// server casing differences don't matter.
static bool isThrottleError(const QString &err)
{
    const QString e = err.toUpper();
    return e.contains(QStringLiteral("THROTTL"))
        || e.contains(QStringLiteral("TOO-MANY-SIMULTANEOUS-CONNECTIONS"))
        || e.contains(QStringLiteral("TOO MANY SIMULTANEOUS CONNECTIONS"));
}

/// True specifically for the concurrent-connection cap, which we answer by
/// using fewer connections, not just by waiting.
static bool isTooManyConnections(const QString &err)
{
    return err.toUpper().contains(QStringLiteral("SIMULTANEOUS-CONNECTIONS"))
        || err.toUpper().contains(QStringLiteral("SIMULTANEOUS CONNECTIONS"));
}

/// Drops RFC 8601 comments "(…)" and quoted strings from an
/// Authentication-Results value. Both carry sender-supplied text — a genuine
/// header echoes the envelope sender in smtp.mailfrom= — and both may contain
/// ';' or the literal "dkim=pass", so they have to go before the value is
/// split into fields or a sender could smuggle a verdict into the display.
static QString stripAuthCommentsAndQuotes(const QString &value)
{
    QString out;
    out.reserve(value.size());
    int commentDepth = 0;
    bool inQuotes = false;
    for (int i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c == QLatin1Char('\\') && (inQuotes || commentDepth > 0)) {
            ++i; // skip the escaped character
            continue;
        }
        if (inQuotes) {
            if (c == QLatin1Char('"'))
                inQuotes = false;
            continue;
        }
        if (commentDepth > 0) {
            if (c == QLatin1Char('('))
                ++commentDepth;
            else if (c == QLatin1Char(')'))
                --commentDepth;
            continue;
        }
        if (c == QLatin1Char('"')) {
            inQuotes = true;
        } else if (c == QLatin1Char('(')) {
            ++commentDepth;
            out.append(QLatin1Char(' '));
        } else {
            out.append(c);
        }
    }
    return out;
}

/// The "method=result" verdicts of an Authentication-Results value, lowercased
/// and in header order. Only the leading token of each ';'-delimited field
/// counts: everything after it (smtp.mailfrom=, header.from=, reason=) echoes
/// sender-supplied data, so scanning the whole value would let a sender inject
/// a passing verdict into an otherwise genuine header.
static QStringList authResultVerdicts(const QString &value)
{
    static const QRegularExpression methodRe(
        QStringLiteral("^\\s*(spf|dkim|dmarc)\\s*=\\s*([a-z]+)"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList out;
    const QStringList fields =
        stripAuthCommentsAndQuotes(value).split(QLatin1Char(';'));
    // Field 0 is the authserv-id, never a verdict.
    for (qsizetype i = 1; i < fields.size(); ++i) {
        const auto m = methodRe.match(fields.at(i));
        if (m.hasMatch())
            out.append(m.captured(1).toLower() + QLatin1Char('=') + m.captured(2).toLower());
    }
    return out;
}

/// True when \a authservId is exactly one of \a trustedDomains or a host under
/// one. Must not be a substring test: "contains" would accept an authserv-id
/// of "gmail.com.attacker.example", which any sender can stamp on their own
/// message, turning the SPF/DKIM badge into attacker-controlled text.
static bool authservIdTrusted(const QString &authservId, const QStringList &trustedDomains)
{
    for (const QString &domain : trustedDomains) {
        if (authservId == domain || authservId.endsWith(QLatin1Char('.') + domain))
            return true;
    }
    return false;
}

/// First Authentication-Results header stamped by our own receiving server
/// (senders can forge their own AR headers, so foreign authserv-ids are
/// ignored). Empty when the message carries no trusted verdict.
static QString trustedAuthResults(const KMime::Message *msg,
                                  const QStringList &trustedAuthDomains)
{
    if (trustedAuthDomains.isEmpty())
        return {}; // nobody to trust — show nothing rather than a forgery
    const auto arHeaders = msg->headersByType("Authentication-Results");
    for (const KMime::Headers::Base *ar : arHeaders) {
        const QString value = ar->asUnicodeString();
        // authserv-id is the first field, optionally followed by a version
        // number: "purelymail.com 1; spf=pass …".
        const QString authservId = stripAuthCommentsAndQuotes(value)
                                       .section(QLatin1Char(';'), 0, 0)
                                       .simplified()
                                       .section(QLatin1Char(' '), 0, 0)
                                       .toLower();
        if (!authservIdTrusted(authservId, trustedAuthDomains))
            continue; // forged or foreign AR header — ignore
        return value; // first trusted header wins (newest — servers prepend)
    }
    return {};
}

/// A FETCH response entry that can become a real list row. Entries without a
/// uid or with an empty header block (aborted/partial deliveries) would show
/// up as "(no subject), 1970" ghosts and can never be opened.
static bool imapEntryUsable(const KIMAP::Message &m)
{
    return m.message && m.uid > 0 && !m.message->head().isEmpty();
}

/// Builds a list header from a KIMAP delivery, including the sender-
/// authentication verdict our receiving server stamped into the message.
static MessageListModel::Header headerFromImap(const KIMAP::Message &m,
                                               const QStringList &trustedAuthDomains)
{
    MessageListModel::Header h;
    h.uid = m.uid;
    const KMime::Message *msg = m.message.get();
    if (const auto *subject = msg->subject())
        h.subject = subject->asUnicodeString();
    if (const auto *from = msg->from())
        h.from = from->asUnicodeString();
    if (const auto *date = msg->date())
        h.date = date->dateTime();
    if (const auto *mid = msg->messageID(); mid && !mid->isEmpty())
        h.msgid = QString::fromLatin1(mid->identifier());
    for (const QByteArray &flag : m.flags) {
        if (flag.compare("\\Seen", Qt::CaseInsensitive) == 0)
            h.seen = true;
    }
    // Header-only heuristic: real attachments arrive as multipart/mixed.
    // Must inspect the raw head — KMime's parsed Content-Type reports
    // text/plain for a multipart message that has no body parts yet.
    h.attachKind = MailStore::headIndicatesAttachment(msg->head())
        ? MessageListModel::GenericAttachment
        : MessageListModel::NoAttachment;
    // Same trick for the lock glyph: the outer content type is in the head, so
    // the list knows an encrypted message without waiting for its body.
    h.crypto = PgpMime::storedKind(PgpMime::kindFromHead(msg->head()));
    h.authInfo = trustedAuthResults(m.message.get(), trustedAuthDomains);
    const QStringList verdicts = authResultVerdicts(h.authInfo);
    for (const QString &verdict : verdicts) {
        const QString result = verdict.section(QLatin1Char('='), 1);
        // fail, softfail, hardfail, permerror — anything but pass/neutral/none
        if (result.endsWith(QLatin1String("fail")) || result == QLatin1String("permerror"))
            h.suspicious = true;
    }
    return h;
}

/// True when a trusted Authentication-Results reported a pass for any method.
/// Kept apart from Header::suspicious, which records the opposite: a message
/// can carry neither (no trusted verdict at all), and "no evidence" must not be
/// confused with either outcome.
static bool authResultsPassed(const QString &authInfo)
{
    const QStringList verdicts = authResultVerdicts(authInfo);
    for (const QString &verdict : verdicts) {
        if (verdict.endsWith(QLatin1String("=pass")))
            return true;
    }
    return false;
}

/// The authserv-id domains whose Authentication-Results headers we trust for a
/// given IMAP host. Providers rarely stamp the domain you connect to — Gmail is
/// reached at imap.gmail.com but stamps "mx.google.com" — so deriving the
/// registrable domain of the host alone would discard every genuine header and
/// leave only forged ones able to match.
static QStringList trustedAuthDomainsForHost(const QString &host)
{
    const QString h = host.toLower();
    struct Provider {
        const char *hostSuffix;
        const char *authDomains; // space-separated
    };
    static const Provider providers[] = {
        {"gmail.com", "mx.google.com google.com"},
        {"googlemail.com", "mx.google.com google.com"},
        {"google.com", "mx.google.com google.com"},
        {"outlook.com", "outlook.com protection.outlook.com"},
        {"office365.com", "outlook.com protection.outlook.com"},
        {"hotmail.com", "outlook.com protection.outlook.com"},
        {"live.com", "outlook.com protection.outlook.com"},
        {"yahoo.com", "yahoo.com"},
        {"fastmail.com", "messagingengine.com fastmail.com"},
        {"messagingengine.com", "messagingengine.com fastmail.com"},
        {"icloud.com", "icloud.com me.com"},
        {"me.com", "icloud.com me.com"},
        {"zoho.com", "zoho.com zohomail.com"},
        {"zohomail.com", "zoho.com zohomail.com"},
        {"proton.me", "proton.me protonmail.ch"},
        {"protonmail.ch", "proton.me protonmail.ch"},
        {"yandex.ru", "yandex.ru"},
        {"gmx.net", "gmx.net"},
        {"web.de", "web.de"},
        {"mail.ru", "mail.ru"},
    };
    for (const Provider &p : providers) {
        const QString suffix = QLatin1String(p.hostSuffix);
        if (h == suffix || h.endsWith(QLatin1Char('.') + suffix))
            return QString::fromLatin1(p.authDomains).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    }
    // Unknown provider: the host itself and its registrable domain, which is
    // what a self-hosted or small-provider server stamps.
    QStringList out{h};
    const QStringList labels = h.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (labels.size() >= 3)
        out.append(labels.mid(1).join(QLatin1Char('.')));
    return out;
}

QStringList MailClient::trustedAuthDomains() const
{
    // Trusting nobody is exactly the "off" behaviour: trustedAuthResults()
    // returns nothing for an empty list, so no verdict is read off a message,
    // none is stored on a header, and the list marker never appears.
    if (!m_authVerification)
        return {};
    return trustedAuthDomainsForHost(m_host);
}

/// "1 message" / "42 messages". Qt only picks plural forms for %n when a
/// translator is installed — without one, "(s)"-style source strings leak
/// into the UI verbatim.
static QString countNoun(qint64 n, const char *singular, const char *plural)
{
    return QStringLiteral("%1 %2").arg(n).arg(
        QLatin1String(n == 1 ? singular : plural));
}

/// Opt-in diagnostics (Settings -> General -> "Log activity to console"). Off
/// by default so a normal run stays quiet; toggling needs no restart.
Q_LOGGING_CATEGORY(logTrace, "mailo.trace")

/// Condense a verbose/multi-line error (often a raw server or KJob string)
/// into a terse status crumb: first line only, trailing punctuation trimmed,
/// and clipped so it never bloats the status line.
static QString shortenError(const QString &err)
{
    QString s = err.section(QLatin1Char('\n'), 0, 0).simplified();
    while (!s.isEmpty() && (s.endsWith(QLatin1Char('.')) || s.endsWith(QLatin1Char(':'))))
        s.chop(1);
    constexpr int kMaxLen = 60;
    if (s.size() > kMaxLen)
        s = s.left(kMaxLen - 1).trimmed() + QStringLiteral("…");
    return s;
}

MailClient::MailClient(QObject *parent)
    : QObject(parent)
{
    // The reading pane's message context. Detached windows clone it; this one
    // lives as long as the client. The legacy Mail.* message properties
    // delegate to it, so its change signals feed theirs.
    m_reading = new MessageContext(this);
    connect(m_reading, &MessageContext::messageChanged,
            this, &MailClient::attachmentsChanged);
    connect(m_reading, &MessageContext::messageChanged,
            this, &MailClient::junkTextOnlyChanged);
    connect(m_reading, &MessageContext::remoteContentAllowedChanged,
            this, &MailClient::remoteContentAllowedChanged);

    // Before the verifier thread exists, so the list's networking belongs to
    // the GUI thread: alignment reads it from the verifier thread, and whoever
    // touches the singleton first decides where it lives.
    PublicSuffixList::instance().start();

    // DKIM verification lives on its own thread for its whole life: the DNS
    // round trip alone would stall the GUI for as long as a resolver takes.
    qRegisterMetaType<DkimResult>();
    m_dkimThread = new QThread(this);
    m_dkimThread->setObjectName(QStringLiteral("dkim"));
    m_dkimVerifier = new DkimVerifier;
    m_dkimVerifier->moveToThread(m_dkimThread);
    connect(m_dkimThread, &QThread::finished, m_dkimVerifier, &QObject::deleteLater);
    connect(m_dkimVerifier, &DkimVerifier::finished, this, &MailClient::applyDkimResult);
    m_dkimThread->start();

    // One-time Message-ID backfill for rows cached before the column existed.
    // 100 rows a tick reads roughly 1.5 MB of message heads — well inside a
    // frame — and the timer stops itself the moment there is nothing left.
    m_msgidBackfillTimer.setInterval(2000);
    connect(&m_msgidBackfillTimer, &QTimer::timeout, this, [this] {
        if (m_store.backfillMessageIds(100) == 0) {
            m_msgidBackfillTimer.stop();
            qCDebug(logTrace, "message-id backfill complete");
        }
    });
    QTimer::singleShot(10000, this, [this] { m_msgidBackfillTimer.start(); });

    // Errors go into the status breadcrumb (short), not passive popups.
    connect(this, &MailClient::errorOccurred, this, [this](const QString &msg) {
        setStatus(shortenError(msg));
    });
    loadAccount();
    m_folderModel.setAccountKey(accountKey());
    m_store.open();
    // Claim any pre-multi-account cache rows for the account they were
    // written by (the one active at upgrade time), then scope everything.
    // Never for a local archive: its key was born after the migration and
    // must not adopt another account's leftovers.
    if (!m_local)
        m_store.adoptLegacyCache(accountKey());
    m_store.setAccountKey(accountKey());

    // Instant startup from cache: folders and INBOX appear before (and
    // without) any network connection.
    loadCachedFolderModel();
    m_selectedFolder = QStringLiteral("INBOX");
    const auto cachedInbox = m_store.cachedHeaders(m_selectedFolder);
    updatePageAnchor(cachedInbox);
    if (!cachedInbox.isEmpty()) {
        m_messageModel.setHeaders(cachedInbox);
        // Real cache size from SQL — cachedInbox is one page (max 1000 rows).
        setStatus(tr("INBOX — %1 cached")
                      .arg(m_store.cachedHeaderCount(m_selectedFolder)));
    }

    // Keepalive ping so the server doesn't drop us as an idle connection.
    m_keepAlive.setInterval(3 * 60 * 1000);
    connect(&m_keepAlive, &QTimer::timeout, this, [this] {
        if (m_connected && m_session)
            (new KIMAP::CapabilitiesJob(m_session))->start();
    });

    // Fallback poll of the open folder for servers where IDLE push is not
    // running; a no-op whenever the IDLE job is alive.
    m_refreshMinutes = qBound(
        0, appSettings().value(QStringLiteral("ui/refreshMinutes"), 5).toInt(), 24 * 60);
    m_maxBodyMB =
        qBound(0, appSettings().value(QStringLiteral("ui/maxBodyMB"), 5).toInt(), 1024);
    m_spamRetentionDays = qBound(
        0, appSettings().value(QStringLiteral("ui/spamRetentionDays"), 30).toInt(), 3650);
    m_spamSkipTrash =
        appSettings().value(QStringLiteral("ui/spamSkipTrash"), true).toBool();
    m_authVerification =
        appSettings().value(QStringLiteral("ui/authVerification"), true).toBool();
    setDebugLogging(appSettings().value(QStringLiteral("ui/debugLogging"), false).toBool());
    connect(&m_pollTimer, &QTimer::timeout, this, [this] {
        if (m_connected && !m_idleJob)
            refreshCurrentFolder();
        // The open folder is covered by IDLE or the line above; every other
        // account has no connection at all, so this is the only thing that
        // ever moves their unread counts.
        pollOtherAccounts();
    });
    if (m_refreshMinutes > 0)
        m_pollTimer.start(m_refreshMinutes * 60 * 1000);

    m_dateFormat = appSettings()
                       .value(QStringLiteral("ui/dateFormat"), QStringLiteral("yyyy-MM-dd"))
                       .toString();
    m_messageModel.setDateFormat(m_dateFormat);

    // Idle-time backfill: while the user reads recent mail (or writes one,
    // or does nothing), quietly walk the remaining older header windows and
    // then cache the missing bodies. Headers strictly first, so a freshly
    // added account shows the complete list before any body downloads.
    m_backfillTimer.setSingleShot(true);
    m_backfillTimer.setInterval(4000);
    connect(&m_backfillTimer, &QTimer::timeout, this, [this] {
        if (!m_connected || !m_session || m_searchActive || m_syncPaused)
            return;
        // The dedicated sync connection may have been dropped on its own (e.g.
        // Gmail throttling the backfill while leaving the main session up).
        // Bring it back before the next window so background work doesn't
        // silently stall or serialize onto the user's connection.
        if (!m_syncSession) {
            startSyncSession();
            m_backfillTimer.start(1000); // give the new session time to log in
            return;
        }
        // Something user-triggered (or a prefetch) is running — retry soon.
        if (m_busy || m_headerFetch || bodyFetchActive() || !m_prefetchQueue.isEmpty()) {
            m_backfillTimer.start(500);
            return;
        }
        if (m_oldestFetchedSeq > 1) {
            m_backfill = true;
            fetchOlderFromServer();
            return;
        }
        if (backfillBodies(m_selectedFolder))
            return;
        // The open folder is completely cached — sync the account's other
        // folders so offline reading and search cover the whole mailbox.
        continueFolderBackfill();
    });

    // Search-index repair: cached bodies queued for re-indexing (after an
    // FTS rebuild) are processed a couple at a time so the GUI thread never
    // stalls; the timer stops itself once the queue is empty.
    m_reindexTimer.setInterval(300);
    connect(&m_reindexTimer, &QTimer::timeout, this, [this] { reindexPendingBodies(); });
    m_reindexTimer.start();

    // Old mail still has its attachments inside the message BLOBs; move them
    // out in the background. Deferred so it never competes with startup.
    if (m_store.attachmentMigrationPending())
        QTimer::singleShot(8000, this, [this] { startAttachmentMigration(); });

    // Same idea for the search index built before diacritic folding. Deferred
    // further than the attachment migration so the two do not fight over the
    // write lock in the first seconds.
    if (m_store.ftsNeedsRebuild())
        QTimer::singleShot(15000, this, [this] { startIndexRebuild(); });
}

/// The delimiter a stored mailbox path is built with. Servers use one or the
/// other and never mix them within a path; imported archives always use '/'.
static QChar folderSeparatorOf(const QString &mailBox)
{
    return mailBox.contains(QLatin1Char('/')) ? QLatin1Char('/') : QLatin1Char('.');
}

/// Depth-first order with parents directly above their children — the order
/// the sidebar's tree rendering depends on (a plain string sort could wedge
/// "Inbox-old" between "Inbox" and "Inbox/Work").
///
/// The inbox sorts above its siblings at every level, the same rule the
/// server's folder listing is sorted by. An archive has no special-use flags
/// to go on, so it is recognised by name — which is all Thunderbird's on-disk
/// layout, where these paths come from, ever had either.
static bool mailBoxPathLess(const QString &a, const QString &b, QChar sep)
{
    const QStringList pa = a.split(sep);
    const QStringList pb = b.split(sep);
    for (int i = 0; i < pa.size() && i < pb.size(); ++i) {
        const bool ai = pa.at(i).compare(QLatin1String("inbox"), Qt::CaseInsensitive) == 0;
        const bool bi = pb.at(i).compare(QLatin1String("inbox"), Qt::CaseInsensitive) == 0;
        if (ai != bi)
            return ai;
        const int c = QString::compare(pa.at(i), pb.at(i), Qt::CaseInsensitive);
        if (c != 0)
            return c < 0;
    }
    return pa.size() < pb.size();
}

/// The delimiter a whole stored list is built with — see folderSeparatorOf().
static QChar separatorOfPaths(const QStringList &boxes)
{
    for (const QString &box : boxes) {
        if (box.contains(QLatin1Char('/')))
            return QLatin1Char('/');
    }
    return QLatin1Char('.');
}

static void sortMailBoxPaths(QStringList *boxes, QChar sep)
{
    std::sort(boxes->begin(), boxes->end(), [sep](const QString &a, const QString &b) {
        return mailBoxPathLess(a, b, sep);
    });
}

/// An archive's folder list in sidebar order. A connected account gets its
/// order from the server's LIST on every refresh; an archive has no server, so
/// the stored row order is all it has — and archives imported before a change
/// to the ordering rule keep whatever order they were written with. Sorting on
/// read repairs those without a re-import.
static QStringList sortedLocalFolders(const QStringList &boxes)
{
    QStringList sorted = boxes;
    sortMailBoxPaths(&sorted, separatorOfPaths(boxes));
    return sorted;
}

/// How every path in \a boxes draws in the tree: its indent depth, and the
/// part of the path that is its own name.
///
/// Both are read from the ancestors that are really in the list, not from the
/// separators in the string. An imported Thunderbird profile keeps its mail
/// under the server directory it came from ("mail.example.com/Inbox") without
/// that directory being a mailbox of its own, and a plain IMAP folder is
/// allowed a dot in its name ("phish.id") on a server whose delimiter is a
/// dot — counting separators indented both of those a step past the account
/// name they hang under, with nothing drawn at the step above.
struct PathRow {
    int level = 0;
    QString name;
};

/// \a knownSep is the delimiter the server reported, when there is one to ask;
/// a null QChar falls back to guessing it per path (all a cached list of some
/// other account has to go on).
static QList<PathRow> pathRows(const QStringList &boxes, QChar knownSep = {})
{
    const QSet<QString> known(boxes.cbegin(), boxes.cend());
    QList<PathRow> rows;
    rows.reserve(boxes.size());
    for (const QString &mailBox : boxes) {
        const QChar sep = knownSep.isNull() ? folderSeparatorOf(mailBox) : knownSep;
        const QStringList parts = mailBox.split(sep);
        PathRow row;
        int shown = 0; // characters covered by the deepest ancestor on screen
        QString prefix;
        for (int i = 0; i + 1 < parts.size(); ++i) {
            prefix += (i == 0 ? QString() : QString(sep)) + parts.at(i);
            if (known.contains(prefix)) {
                ++row.level;
                shown = int(prefix.size()) + 1; // + the separator after it
            }
        }
        row.name = mailBox.mid(shown);
        rows.append(row);
    }
    return rows;
}

QList<FolderModel::Folder> MailClient::foldersFromPaths(const QStringList &paths)
{
    const QList<PathRow> rows = pathRows(paths);
    QList<FolderModel::Folder> folders;
    folders.reserve(paths.size());
    for (int i = 0; i < paths.size(); ++i) {
        FolderModel::Folder f;
        f.mailBox = paths.at(i);
        f.level = rows.at(i).level;
        f.displayName = rows.at(i).name;
        folders.append(f);
    }
    return folders;
}

void MailClient::loadCachedFolderModel()
{
    QStringList boxes = m_store.cachedFolders(accountKey());
    if (m_local) {
        const QStringList sorted = sortedLocalFolders(boxes);
        if (sorted != boxes) {
            m_store.storeFolders(accountKey(), sorted); // repair it once, not on every switch
            boxes = sorted;
        }
    }
    const QList<FolderModel::Folder> folders = foldersFromPaths(boxes);
    if (!folders.isEmpty())
        m_folderModel.setFolders(folders);
    // setFolders() cleared nothing, but the pills belong to this account —
    // hand over what the last pass found and ask for a fresh one.
    m_folderModel.setUnreadCounts(m_unreadByAccount.value(accountKey()));
    scheduleUnreadRecount();
}

void MailClient::setSpamRetentionDays(int days)
{
    days = qBound(0, days, 3650);
    if (m_spamRetentionDays == days)
        return;
    m_spamRetentionDays = days;
    appSettings().setValue(QStringLiteral("ui/spamRetentionDays"), days);
    // A shortened window should bite now, not at the next connection.
    m_spamSwept = false;
    Q_EMIT spamRetentionDaysChanged();
    if (m_connected)
        sweepOldSpam();
}

void MailClient::setSpamSkipTrash(bool skip)
{
    if (m_spamSkipTrash == skip)
        return;
    m_spamSkipTrash = skip;
    appSettings().setValue(QStringLiteral("ui/spamSkipTrash"), skip);
    Q_EMIT spamSkipTrashChanged();
}

bool MailClient::deleteIsPermanent() const
{
    return isTrashFolder() || (m_spamSkipTrash && isJunkFolder(m_selectedFolder));
}

void MailClient::setRefreshMinutes(int minutes)
{
    minutes = qBound(0, minutes, 24 * 60);
    if (m_refreshMinutes == minutes)
        return;
    m_refreshMinutes = minutes;
    appSettings().setValue(QStringLiteral("ui/refreshMinutes"), minutes);
    if (minutes > 0)
        m_pollTimer.start(minutes * 60 * 1000);
    else
        m_pollTimer.stop();
    Q_EMIT refreshMinutesChanged();
}

void MailClient::setDebugLogging(bool on)
{
    m_debugLogging = on;
    appSettings().setValue(QStringLiteral("ui/debugLogging"), on);
    // Rules, not a boolean check at each call site: disabled categories cost
    // nothing, and QT_LOGGING_RULES in the environment still wins for a
    // developer who wants the trace without touching the setting.
    QLoggingCategory::setFilterRules(on ? QStringLiteral("mailo.trace.debug=true")
                                        : QStringLiteral("mailo.trace.debug=false"));
    Q_EMIT debugLoggingChanged();
}

void MailClient::setMaxBodyMB(int mb)
{
    mb = qBound(0, mb, 1024);
    if (m_maxBodyMB == mb)
        return;
    const bool raised = mb == 0 || mb > m_maxBodyMB;
    m_maxBodyMB = mb;
    appSettings().setValue(QStringLiteral("ui/maxBodyMB"), mb);
    if (raised) {
        // Messages refused under the old, smaller limit become eligible again.
        const int freed = m_store.unskipBodiesUpTo(qint64(mb) * 1024 * 1024);
        if (freed > 0) {
            invalidateMissingBodies();
            scheduleBackfill(1000);
        }
    }
    Q_EMIT maxBodyMBChanged();
}

void MailClient::setAuthVerification(bool on)
{
    if (m_authVerification == on)
        return;
    m_authVerification = on;
    appSettings().setValue(QStringLiteral("ui/authVerification"), on);

    // Take effect on what is already on screen rather than only on the next
    // message: switching this off is a privacy choice, and leaving a stale
    // verdict visible would misrepresent it as still being checked.
    if (m_reading) {
        m_reading->m_dkimStatus.clear();
        m_reading->m_dkimDetail.clear();
        m_reading->m_arcStatus.clear();
        m_reading->m_arcSealer.clear();
        m_reading->m_arcDetail.clear();
        m_reading->m_dkimTrusted = false;
        m_reading->m_dkimChecking = false;
        m_reading->m_authInfo.clear();
        Q_EMIT m_reading->dkimChanged();
        Q_EMIT m_reading->messageChanged();
        // Re-check the open message when switching back on; the header-derived
        // verdicts in the list heal on the next sync of each folder.
        if (on && m_reading->m_hasMessage)
            startDkimVerification(m_reading);
    }
    Q_EMIT authVerificationChanged();
}

void MailClient::setDateFormat(const QString &format)
{
    if (m_dateFormat == format || format.isEmpty())
        return;
    m_dateFormat = format;
    appSettings().setValue(QStringLiteral("ui/dateFormat"), format);
    m_messageModel.setDateFormat(format);
    Q_EMIT dateFormatChanged();
}

/// Storage key of account \a index straight from settings — same rule as
/// accountKey(), for the sidebar paths that inspect non-active accounts.
/// Empty when the index is unknown or the account has no identity yet.
static QString storedAccountKeyAt(QSettings &s, int index, bool *local = nullptr)
{
    QString user, host, cacheKey;
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    if (index >= 0 && index < count) {
        s.setArrayIndex(index);
        user = s.value(QStringLiteral("user")).toString();
        host = s.value(QStringLiteral("host")).toString();
        cacheKey = s.value(QStringLiteral("cacheKey")).toString();
        if (local)
            *local = s.value(QStringLiteral("local"), false).toBool();
    }
    s.endArray();
    if (!cacheKey.isEmpty())
        return cacheKey;
    if (host.isEmpty() && user.isEmpty())
        return {};
    return user + QLatin1Char('@') + host;
}

QVariantList MailClient::cachedFolderList(int index)
{
    QSettings s = appSettings();
    bool local = false;
    const QString key = storedAccountKeyAt(s, index, &local);
    if (key.isEmpty())
        return {};

    QVariantList out;
    const QSet<QString> collapsed = FolderModel::savedCollapsed(key);
    const QHash<QString, int> unread = m_unreadByAccount.value(key);
    QStringList boxes = m_store.cachedFolders(key);
    // Same repair as loadCachedFolderModel(), for an archive that is shown in
    // the pane but has not been switched to yet. Read-only here: the account
    // this is rendering is not the one the store is keyed to.
    if (local)
        boxes = sortedLocalFolders(boxes);
    // Rows of the WHOLE list up front: hasChildren must see the next row even
    // when a collapsed ancestor hides it from the output.
    const QList<PathRow> rows = pathRows(boxes);

    // Which rows the collapse state hides, and — for the rows that stay — how
    // much unread is folded away underneath them. Same rule as
    // FolderModel::recomputeHiddenUnread(): a folded subfolder must not be
    // able to hide new mail from the parent that stands in for it.
    QList<bool> hidden(boxes.size(), false);
    {
        int skip = -1;
        for (int i = 0; i < boxes.size(); ++i) {
            const int level = rows.at(i).level;
            if (skip >= 0 && level > skip) {
                hidden[i] = true;
                continue;
            }
            skip = collapsed.contains(boxes.at(i)) ? level : -1;
        }
    }
    QList<int> hiddenUnread(boxes.size(), 0);
    for (int i = 0; i < boxes.size(); ++i) {
        if (!hidden.at(i))
            continue;
        const int count = unread.value(boxes.at(i), 0);
        if (count == 0)
            continue;
        for (int j = i - 1, level = rows.at(i).level; j >= 0 && level > 0; --j) {
            if (rows.at(j).level < level) {
                level = rows.at(j).level;
                hiddenUnread[j] += count;
            }
        }
    }

    int skipDeeperThan = -1; // hide rows below a collapsed ancestor
    for (int i = 0; i < boxes.size(); ++i) {
        const QString &mailBox = boxes.at(i);
        const int level = rows.at(i).level;
        if (skipDeeperThan >= 0 && level > skipDeeperThan)
            continue;
        const bool isCollapsed = collapsed.contains(mailBox);
        skipDeeperThan = isCollapsed ? level : -1;
        out.append(QVariantMap{
            {QStringLiteral("name"), rows.at(i).name},
            {QStringLiteral("mailBox"), mailBox},
            {QStringLiteral("level"), level},
            {QStringLiteral("hasChildren"),
             i + 1 < boxes.size() && rows.at(i + 1).level > level},
            {QStringLiteral("expanded"), !isCollapsed},
            {QStringLiteral("unread"), unread.value(mailBox, 0)},
            {QStringLiteral("hiddenUnread"), hiddenUnread.at(i)},
        });
    }
    return out;
}

void MailClient::toggleCachedCollapsed(int index, const QString &mailBox)
{
    QSettings s = appSettings();
    const QString key = storedAccountKeyAt(s, index);
    if (key.isEmpty())
        return;
    FolderModel::toggleSavedCollapsed(key, mailBox);
    ++m_cachedFolderRevision;
    Q_EMIT cachedFoldersChanged();
}

void MailClient::openFolderInAccount(int index, const QString &mailBox)
{
    qCDebug(logTrace, "openFolderInAccount(%d, %s)  current=%d",
            index, qUtf8Printable(mailBox), m_currentAccount);
    if (index == m_currentAccount) {
        openFolder(mailBox);
        return;
    }
    // The switch itself shows this folder's cache and opens it once the new
    // account's connection has listed its folders.
    switchAccountInternal(index, QString(), mailBox);
}

bool MailClient::hasAccount() const
{
    return !m_host.isEmpty() && !m_user.isEmpty();
}

static QString walletKeyFor(const QString &user, const QString &host)
{
    return kWalletKey + QLatin1Char(':') + user + QLatin1Char('@') + host;
}

QString MailClient::walletKey() const
{
    return walletKeyFor(m_user, m_host);
}

/// One-time migration of the legacy single-account keys ("account/…",
/// "smtp/…") into slot 0 of the accounts array.
static void migrateLegacyAccount(QSettings &s)
{
    if (s.value(QStringLiteral("accounts/size"), 0).toInt() > 0)
        return;
    const QString host = s.value(QStringLiteral("account/host")).toString();
    const QString user = s.value(QStringLiteral("account/user")).toString();
    if (host.isEmpty() && user.isEmpty())
        return;

    s.beginWriteArray(QStringLiteral("accounts"), 1);
    s.setArrayIndex(0);
    s.setValue(QStringLiteral("host"), host);
    s.setValue(QStringLiteral("port"), s.value(QStringLiteral("account/port"), 993));
    s.setValue(QStringLiteral("security"),
               s.value(QStringLiteral("account/security"), int(MailClient::SslTls)));
    s.setValue(QStringLiteral("user"), user);
    s.setValue(QStringLiteral("smtpHost"), s.value(QStringLiteral("smtp/host")));
    s.setValue(QStringLiteral("smtpPort"), s.value(QStringLiteral("smtp/port"), 587));
    s.setValue(QStringLiteral("smtpSecurity"), s.value(QStringLiteral("smtp/security"), 1));
    s.endArray();
    s.setValue(QStringLiteral("currentAccount"), 0);
    s.remove(QStringLiteral("account/host"));
    s.remove(QStringLiteral("account/port"));
    s.remove(QStringLiteral("account/security"));
    s.remove(QStringLiteral("account/user"));
    s.remove(QStringLiteral("smtp"));
    // "account/secret" (pre-wallet plaintext) is handled by loadAccount below.
}

/// One-time migration: before the address had its own field, accounts kept it
/// in the login, and that is what they sent as. Copy it across so the address
/// is stored outright rather than re-derived on every send. Logins that are
/// not addresses are left alone — there is nothing to copy, and ownAddress()
/// keeps guessing for them until the account dialog is filled in.
static void migrateAccountEmail(QSettings &s)
{
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    QList<int> needsEmail;
    for (int i = 0; i < count; ++i) {
        s.setArrayIndex(i);
        if (!s.value(QStringLiteral("email")).toString().isEmpty())
            continue;
        if (s.value(QStringLiteral("user")).toString().contains(QLatin1Char('@')))
            needsEmail.append(i);
    }
    s.endArray();
    if (needsEmail.isEmpty())
        return;

    s.beginWriteArray(QStringLiteral("accounts"), count);
    for (const int i : needsEmail) {
        s.setArrayIndex(i);
        s.setValue(QStringLiteral("email"), s.value(QStringLiteral("user")));
    }
    s.endArray();
}

void MailClient::loadAccount()
{
    QSettings s = appSettings();
    migrateLegacyAccount(s);
    migrateAccountEmail(s);
    m_currentAccount = s.value(QStringLiteral("currentAccount"), 0).toInt();
    loadAccountFields();

    // A local archive owns no secret — don't touch the keyring for it.
    if (m_local) {
        m_secretReady = true;
        return;
    }

    // One-time migration: pre-wallet builds kept the password base64-encoded
    // in the config file. Move it into the system wallet and wipe it.
    const QByteArray legacy = s.value(QStringLiteral("account/secret")).toByteArray();
    if (!legacy.isEmpty()) {
        m_password = QString::fromUtf8(QByteArray::fromBase64(legacy));
        m_secretReady = true;
        writeSecretToWallet();
        return;
    }
    readWalletPassword();
}

/// Reads the active account's config fields (no password) from the array.
void MailClient::loadAccountFields()
{
    QSettings s = appSettings();
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    if (count == 0) {
        s.endArray();
        m_host.clear();
        m_user.clear();
        m_email.clear();
        m_displayName.clear();
        m_organization.clear();
        m_smtpHost.clear();
        m_signature.clear();
        m_local = false;
        m_cacheKey.clear();
        return;
    }
    m_currentAccount = qBound(0, m_currentAccount, count - 1);
    s.setArrayIndex(m_currentAccount);
    m_host = s.value(QStringLiteral("host")).toString();
    m_port = s.value(QStringLiteral("port"), 993).toInt();
    m_security = s.value(QStringLiteral("security"), int(SslTls)).toInt();
    m_user = s.value(QStringLiteral("user")).toString();
    m_email = s.value(QStringLiteral("email")).toString();
    m_displayName = s.value(QStringLiteral("displayName")).toString();
    m_organization = s.value(QStringLiteral("organization")).toString();
    m_smtpHost = s.value(QStringLiteral("smtpHost")).toString();
    m_smtpPort = s.value(QStringLiteral("smtpPort"), 587).toInt();
    m_smtpSecurity = s.value(QStringLiteral("smtpSecurity"), 1).toInt();
    m_authType = s.value(QStringLiteral("authType"), 0).toInt();
    m_clientId = s.value(QStringLiteral("clientId")).toString();
    m_clientSecret = s.value(QStringLiteral("clientSecret")).toString();
    m_signature = s.value(QStringLiteral("signature")).toString();
    m_htmlMail = s.value(QStringLiteral("htmlMail"), true).toBool();
    m_local = s.value(QStringLiteral("local"), false).toBool();
    // Per-account, and only the account's own LIST may set it. Carrying the
    // previous account's delimiter over meant every path split the wrong way —
    // a '.' server followed by an archive turned "mail.example.com/Inbox" into
    // a leaf of "com/Inbox", and a rename then wrote that to disk.
    m_folderSeparator = {};
    m_cacheKey = s.value(QStringLiteral("cacheKey")).toString();
    s.endArray();
    m_accessToken.clear(); // tokens never survive an account switch
    m_refreshToken.clear();
    if (m_smtpHost.isEmpty() && !m_host.isEmpty()) {
        // Sensible default: imap.example.com → smtp.example.com
        m_smtpHost = m_host;
        m_smtpHost.replace(QRegularExpression(QStringLiteral("^imap")), QStringLiteral("smtp"));
    }
}

QString MailClient::oauthWalletKey() const
{
    return QStringLiteral("oauth-refresh:") + m_user + QLatin1Char('@') + m_host;
}

void MailClient::readWalletPassword()
{
    m_password.clear();
    m_refreshToken.clear();
    m_secretReady = false;
    const int gen = ++m_walletGen;

    auto finish = [this, gen] {
        if (gen != m_walletGen)
            return; // the account changed while we were reading — stale result
        m_secretReady = true;
        if (m_connectWhenReady) {
            m_connectWhenReady = false;
            connectAccount();
        }
    };

    // OAuth accounts keep a refresh token in the wallet instead of a password.
    if (m_authType != 0) {
        auto *read = new QKeychain::ReadPasswordJob(kWalletService, this);
        read->setKey(oauthWalletKey());
        connect(read, &QKeychain::Job::finished, this, [this, read, gen, finish] {
            if (gen != m_walletGen)
                return;
            if (!read->error())
                m_refreshToken = read->textData();
            finish();
        });
        read->start();
        return;
    }

    auto *read = new QKeychain::ReadPasswordJob(kWalletService, this);
    read->setKey(walletKey());
    connect(read, &QKeychain::Job::finished, this, [this, read, gen, finish] {
        if (gen != m_walletGen)
            return;
        if (!read->error()) {
            m_password = read->textData();
            finish();
            return;
        }
        if (read->error() != QKeychain::EntryNotFound) {
            qWarning() << "mailo: wallet read failed:" << read->errorString();
            finish();
            return;
        }
        // Single-account era stored the password under a fixed key. Read it
        // once and re-store it under the per-account key.
        auto *legacy = new QKeychain::ReadPasswordJob(kWalletService, this);
        legacy->setKey(kWalletKey);
        connect(legacy, &QKeychain::Job::finished, this, [this, legacy, gen, finish] {
            if (gen != m_walletGen)
                return;
            if (!legacy->error()) {
                m_password = legacy->textData();
                writeSecretToWallet();
            }
            finish();
        });
        legacy->start();
    });
    read->start();
}

void MailClient::writeSecretToWallet()
{
    auto *write = new QKeychain::WritePasswordJob(kWalletService, this);
    write->setKey(walletKey());
    write->setTextData(m_password);
    connect(write, &QKeychain::Job::finished, this, [this, write] {
        if (write->error()) {
            Q_EMIT errorOccurred(
                tr("Could not store the password in the system wallet: %1")
                    .arg(write->errorString()));
            return;
        }
        // Only drop the plaintext once the wallet definitely has it.
        appSettings().remove(QStringLiteral("account/secret"));
    });
    write->start();
}

QStringList MailClient::accountNames() const
{
    QSettings s = appSettings();
    QStringList names;
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    for (int i = 0; i < count; ++i) {
        s.setArrayIndex(i);
        // The list is user-facing, so it shows the address; the login and the
        // host are only fallbacks for accounts that have no address stored.
        const QString email = s.value(QStringLiteral("email")).toString();
        const QString user = s.value(QStringLiteral("user")).toString();
        if (!email.isEmpty())
            names.append(email);
        else
            names.append(user.isEmpty() ? s.value(QStringLiteral("host")).toString() : user);
    }
    s.endArray();
    return names;
}

QVariantMap MailClient::accountDetails(int index) const
{
    QSettings s = appSettings();
    QVariantMap out;
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    if (index >= 0 && index < count) {
        s.setArrayIndex(index);
        out.insert(QStringLiteral("host"), s.value(QStringLiteral("host")).toString());
        out.insert(QStringLiteral("port"), s.value(QStringLiteral("port"), 993).toInt());
        out.insert(QStringLiteral("security"),
                   s.value(QStringLiteral("security"), int(SslTls)).toInt());
        out.insert(QStringLiteral("user"), s.value(QStringLiteral("user")).toString());
        out.insert(QStringLiteral("email"), s.value(QStringLiteral("email")).toString());
        out.insert(QStringLiteral("displayName"),
                   s.value(QStringLiteral("displayName")).toString());
        out.insert(QStringLiteral("organization"),
                   s.value(QStringLiteral("organization")).toString());
        out.insert(QStringLiteral("smtpHost"), s.value(QStringLiteral("smtpHost")).toString());
        out.insert(QStringLiteral("smtpPort"), s.value(QStringLiteral("smtpPort"), 587).toInt());
        out.insert(QStringLiteral("smtpSecurity"),
                   s.value(QStringLiteral("smtpSecurity"), 1).toInt());
        out.insert(QStringLiteral("authType"), s.value(QStringLiteral("authType"), 0).toInt());
        out.insert(QStringLiteral("clientId"), s.value(QStringLiteral("clientId")).toString());
        out.insert(QStringLiteral("clientSecret"),
                   s.value(QStringLiteral("clientSecret")).toString());
        out.insert(QStringLiteral("signature"),
                   s.value(QStringLiteral("signature")).toString());
        out.insert(QStringLiteral("htmlMail"),
                   s.value(QStringLiteral("htmlMail"), true).toBool());
        out.insert(QStringLiteral("local"), s.value(QStringLiteral("local"), false).toBool());
        // OpenPGP: the fingerprint of this identity's key and what to do with
        // it. No key material — that stays in the user's GnuPG home, and this
        // is only a pointer into it (doc/openpgp.md §8).
        out.insert(QStringLiteral("pgpKeyFp"), s.value(QStringLiteral("pgpKeyFp")).toString());
        out.insert(QStringLiteral("pgpSignByDefault"),
                   s.value(QStringLiteral("pgpSignByDefault"), false).toBool());
        out.insert(QStringLiteral("pgpEncryptByDefault"),
                   s.value(QStringLiteral("pgpEncryptByDefault"), false).toBool());
        out.insert(QStringLiteral("pgpAutoWkd"),
                   s.value(QStringLiteral("pgpAutoWkd"), true).toBool());
    }
    s.endArray();
    return out;
}

/// The settings a live session is built from: change one and the connection
/// has to be made again, which is exactly what saveAccountPrefs() must never
/// do. "local" and "cacheKey" are in here because they decide whether there is
/// a session at all and which cache it reads.
static const QStringList kSessionKeys = {
    QStringLiteral("host"),         QStringLiteral("port"),
    QStringLiteral("security"),     QStringLiteral("user"),
    QStringLiteral("email"),        QStringLiteral("smtpHost"),
    QStringLiteral("smtpPort"),     QStringLiteral("smtpSecurity"),
    QStringLiteral("authType"),     QStringLiteral("clientId"),
    QStringLiteral("clientSecret"), QStringLiteral("local"),
    QStringLiteral("cacheKey")};

void MailClient::saveAccountDetails(int index, const QVariantMap &d)
{
    const QString trimmedHost = d.value(QStringLiteral("host")).toString().trimmed();
    const QString trimmedUser = d.value(QStringLiteral("user")).toString().trimmed();
    const QString trimmedEmail = d.value(QStringLiteral("email")).toString().trimmed();
    // The display name goes into a header, so a newline in it would be a
    // header-injection point — same treatment the compose fields get.
    static const QRegularExpression headerCrlfRe(QStringLiteral("[\\r\\n]"));
    const QString displayName = d.value(QStringLiteral("displayName"))
                                    .toString()
                                    .remove(headerCrlfRe)
                                    .trimmed();
    const QString organization = d.value(QStringLiteral("organization"))
                                     .toString()
                                     .remove(headerCrlfRe)
                                     .trimmed();
    const QString password = d.value(QStringLiteral("password")).toString();
    const bool savePassword = d.value(QStringLiteral("savePassword"), true).toBool();
    const int authType = d.value(QStringLiteral("authType"), 0).toInt();

    QSettings s = appSettings();
    // An imported archive keeps its storage key forever, and stops being a
    // local (never-connecting) account the moment a server is filled in —
    // that is the whole upgrade path from "dead archive" to live account.
    bool wasLocal = false;
    QString cacheKey;
    // The encryption settings as they stand, so a caller that does not mention
    // them — the archive importer, say — does not silently unset the account's
    // key by omission.
    QVariantMap pgpNow;
    // The session-deciding fields as they stand, so an unchanged save does not
    // drop the connection (see the comparison at the end of this function).
    QVariantMap sessionNow;
    bool existed = false;
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    if (index >= 0 && index < count) {
        s.setArrayIndex(index);
        existed = true;
        for (const QString &k : kSessionKeys)
            sessionNow.insert(k, s.value(k));
        wasLocal = s.value(QStringLiteral("local"), false).toBool();
        cacheKey = s.value(QStringLiteral("cacheKey")).toString();
        pgpNow.insert(QStringLiteral("pgpKeyFp"), s.value(QStringLiteral("pgpKeyFp"), QString()));
        pgpNow.insert(QStringLiteral("pgpSignByDefault"),
                      s.value(QStringLiteral("pgpSignByDefault"), false));
        pgpNow.insert(QStringLiteral("pgpEncryptByDefault"),
                      s.value(QStringLiteral("pgpEncryptByDefault"), false));
        pgpNow.insert(QStringLiteral("pgpAutoWkd"),
                      s.value(QStringLiteral("pgpAutoWkd"), true));
    }
    s.endArray();
    if (index < 0 || index > count)
        index = count; // append as a new account
    // The account dialog states the type outright ("Imported account"), so
    // take it at its word. The old rule — an archive stays one while it has no
    // server — remains the fallback for callers that do not say, which is what
    // the import path itself relies on.
    const bool stillLocal = d.contains(QStringLiteral("local"))
        ? d.value(QStringLiteral("local")).toBool()
        : (wasLocal && trimmedHost.isEmpty());

    s.beginWriteArray(QStringLiteral("accounts"), qMax(count, index + 1));
    s.setArrayIndex(index);
    s.setValue(QStringLiteral("host"), trimmedHost);
    s.setValue(QStringLiteral("port"), d.value(QStringLiteral("port"), 993).toInt());
    s.setValue(QStringLiteral("security"), d.value(QStringLiteral("security"), 0).toInt());
    s.setValue(QStringLiteral("user"), trimmedUser);
    s.setValue(QStringLiteral("email"), trimmedEmail);
    s.setValue(QStringLiteral("displayName"), displayName);
    s.setValue(QStringLiteral("organization"), organization);
    s.setValue(QStringLiteral("smtpHost"),
               d.value(QStringLiteral("smtpHost")).toString().trimmed());
    s.setValue(QStringLiteral("smtpPort"), d.value(QStringLiteral("smtpPort"), 587).toInt());
    s.setValue(QStringLiteral("smtpSecurity"),
               d.value(QStringLiteral("smtpSecurity"), 1).toInt());
    s.setValue(QStringLiteral("authType"), authType);
    s.setValue(QStringLiteral("clientId"), d.value(QStringLiteral("clientId")).toString());
    s.setValue(QStringLiteral("clientSecret"),
               d.value(QStringLiteral("clientSecret")).toString());
    s.setValue(QStringLiteral("signature"), d.value(QStringLiteral("signature")).toString());
    s.setValue(QStringLiteral("htmlMail"), d.value(QStringLiteral("htmlMail"), true).toBool());
    s.setValue(QStringLiteral("local"), stillLocal);
    s.setValue(QStringLiteral("cacheKey"), cacheKey);
    s.setValue(QStringLiteral("pgpKeyFp"),
               d.value(QStringLiteral("pgpKeyFp"),
                       pgpNow.value(QStringLiteral("pgpKeyFp"), QString()))
                   .toString()
                   .trimmed());
    s.setValue(QStringLiteral("pgpSignByDefault"),
               d.value(QStringLiteral("pgpSignByDefault"),
                       pgpNow.value(QStringLiteral("pgpSignByDefault"), false))
                   .toBool());
    s.setValue(QStringLiteral("pgpEncryptByDefault"),
               d.value(QStringLiteral("pgpEncryptByDefault"),
                       pgpNow.value(QStringLiteral("pgpEncryptByDefault"), false))
                   .toBool());
    s.setValue(QStringLiteral("pgpAutoWkd"),
               d.value(QStringLiteral("pgpAutoWkd"),
                       pgpNow.value(QStringLiteral("pgpAutoWkd"), true))
                   .toBool());
    s.endArray();

    if (authType == 0) {
        if (!password.isEmpty() && savePassword) {
            auto *write = new QKeychain::WritePasswordJob(kWalletService, this);
            write->setKey(walletKeyFor(trimmedUser, trimmedHost));
            write->setTextData(password);
            write->start();
        } else if (!savePassword) {
            auto *del = new QKeychain::DeletePasswordJob(kWalletService, this);
            del->setKey(walletKeyFor(trimmedUser, trimmedHost));
            del->start();
        }
    }

    // What was just written, for the comparison below.
    QVariantMap sessionNext;
    const int written = s.beginReadArray(QStringLiteral("accounts"));
    if (index < written) {
        s.setArrayIndex(index);
        for (const QString &k : kSessionKeys)
            sessionNext.insert(k, s.value(k));
    }
    s.endArray();

    // Reconnecting is the right answer to a changed server, a new password or
    // a different account — and the wrong one to a changed signature, which
    // used to cost a full teardown and dial (and, on Gmail, a connection out
    // of a small budget) for a settings write that no session depends on.
    // A save while nothing is connected still dials: that is how a failed
    // connection is retried.
    const bool needsSession = !existed || index != m_currentAccount || !m_session
        || sessionNext != sessionNow || (authType == 0 && !password.isEmpty());
    if (needsSession) {
        switchAccountInternal(index, authType == 0 ? password : QString());
        return;
    }
    // Same session, new preferences: take the fields the composer reads from
    // the settings just written. Deliberately not loadAccountFields(), which
    // drops the OAuth tokens this still-live session is using.
    m_displayName = displayName;
    m_organization = organization;
    m_signature = d.value(QStringLiteral("signature")).toString();
    m_htmlMail = d.value(QStringLiteral("htmlMail"), true).toBool();
    Q_EMIT accountChanged();
    Q_EMIT accountsChanged();
}

/// Every per-account setting, in one place: the account array is rewritten
/// wholesale when an account is removed or reordered, and a key missing from
/// this list is a key silently dropped from every account by that rewrite.
static QStringList accountSettingKeys()
{
    return {QStringLiteral("host"),         QStringLiteral("port"),
            QStringLiteral("security"),     QStringLiteral("user"),
            QStringLiteral("email"),        QStringLiteral("displayName"),
            QStringLiteral("organization"),
            QStringLiteral("smtpHost"),     QStringLiteral("smtpPort"),
            QStringLiteral("smtpSecurity"), QStringLiteral("authType"),
            QStringLiteral("clientId"),
            QStringLiteral("clientSecret"), QStringLiteral("signature"),
            QStringLiteral("htmlMail"),     QStringLiteral("local"),
            QStringLiteral("cacheKey"),
            QStringLiteral("pgpKeyFp"),     QStringLiteral("pgpSignByDefault"),
            QStringLiteral("pgpEncryptByDefault"),
            QStringLiteral("pgpAutoWkd")};
}

/// Reads the whole account array out of \a s so it can be rewritten.
static QList<QVariantMap> readAccountArray(QSettings &s)
{
    const QStringList keys = accountSettingKeys();
    QList<QVariantMap> accounts;
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    for (int i = 0; i < count; ++i) {
        s.setArrayIndex(i);
        QVariantMap a;
        for (const QString &k : keys)
            a.insert(k, s.value(k));
        accounts.append(a);
    }
    s.endArray();
    return accounts;
}

/// Replaces the account array in \a s with \a accounts.
static void writeAccountArray(QSettings &s, const QList<QVariantMap> &accounts)
{
    s.remove(QStringLiteral("accounts"));
    s.beginWriteArray(QStringLiteral("accounts"), accounts.size());
    for (int i = 0; i < accounts.size(); ++i) {
        s.setArrayIndex(i);
        for (auto it = accounts.at(i).constBegin(); it != accounts.at(i).constEnd(); ++it)
            s.setValue(it.key(), it.value());
    }
    s.endArray();
}

void MailClient::saveAccountPrefs(int index, const QVariantMap &d)
{
    QSettings s = appSettings();
    QList<QVariantMap> accounts = readAccountArray(s);
    // An account that moved or was removed under an open settings page: the
    // write would land on somebody else's row, so it does not happen.
    if (index < 0 || index >= accounts.size())
        return;

    static const QRegularExpression headerCrlfRe(QStringLiteral("[\\r\\n]"));
    QVariantMap account = accounts.at(index);
    bool touched = false;
    for (auto it = d.constBegin(); it != d.constEnd(); ++it) {
        if (kSessionKeys.contains(it.key()))
            continue; // explicit save only — see the header
        QVariant value = it.value();
        // Both go into headers, where a newline would be an injection point.
        if (it.key() == QLatin1String("displayName")
            || it.key() == QLatin1String("organization"))
            value = value.toString().remove(headerCrlfRe).trimmed();
        if (account.value(it.key()) == value)
            continue;
        account.insert(it.key(), value);
        touched = true;
    }
    if (!touched)
        return; // nothing to write, nothing to announce

    accounts[index] = account;
    writeAccountArray(s, accounts);

    // The OpenPGP settings are read from QSettings at send time, so they are
    // already live; these four are cached and have to be told.
    if (index == m_currentAccount) {
        m_displayName = account.value(QStringLiteral("displayName")).toString();
        m_organization = account.value(QStringLiteral("organization")).toString();
        m_signature = account.value(QStringLiteral("signature")).toString();
        m_htmlMail = account.value(QStringLiteral("htmlMail"), true).toBool();
        Q_EMIT accountChanged();
    }
    Q_EMIT accountsChanged();
}

void MailClient::removeAccount(int index)
{
    QSettings s = appSettings();
    QList<QVariantMap> accounts = readAccountArray(s);
    if (index < 0 || index >= accounts.size())
        return;

    const QString delUser = accounts.at(index).value(QStringLiteral("user")).toString();
    const QString delHost = accounts.at(index).value(QStringLiteral("host")).toString();
    for (const QString &key : {walletKeyFor(delUser, delHost),
                               QStringLiteral("oauth-refresh:") + delUser
                                   + QLatin1Char('@') + delHost}) {
        auto *del = new QKeychain::DeletePasswordJob(kWalletService, this);
        del->setKey(key);
        del->start();
    }

    accounts.removeAt(index);
    writeAccountArray(s, accounts);

    if (accounts.isEmpty()) {
        teardownSession();
        m_folderModel.setFolders({});
        m_messageModel.clear();
        m_host.clear();
        m_user.clear();
        m_email.clear();
        Q_EMIT accountChanged();
        Q_EMIT accountsChanged();
        return;
    }
    switchAccountInternal(qMin(m_currentAccount, int(accounts.size()) - 1), QString());
}

void MailClient::moveAccount(int from, int to)
{
    QSettings s = appSettings();
    QList<QVariantMap> accounts = readAccountArray(s);
    if (from == to || from < 0 || from >= accounts.size() || to < 0 || to >= accounts.size())
        return;

    accounts.move(from, to);
    writeAccountArray(s, accounts);

    // Order is presentation only: nothing keyed on an account (wallet entry,
    // message cache) uses its position, so no session teardown is needed. The
    // stored current-account index does, though — it has to follow the account
    // it pointed at, or a reorder would silently switch accounts.
    if (m_currentAccount == from)
        m_currentAccount = to;
    else if (from < m_currentAccount && m_currentAccount <= to)
        --m_currentAccount;
    else if (to <= m_currentAccount && m_currentAccount < from)
        ++m_currentAccount;
    s.setValue(QStringLiteral("currentAccount"), m_currentAccount);
    Q_EMIT accountsChanged();
}

// --- Thunderbird import ---------------------------------------------------

// The body-text extractor lives with the viewer code below; the importer
// reuses it so imported mail is searchable exactly like fetched mail.
static QString indexTextFor(KMime::Message *msg);

/// One mbox file found under the import root, and the mailbox it becomes.
struct MboxSource {
    QString filePath;
    QString mailBox;
};

/// True for a file that holds Thunderbird mbox mail. The ".msf" index next to
/// it is the strong signal; without one, accept a file that begins like an
/// mbox. Everything else Thunderbird keeps next to its mail — indexes, filter
/// rules, junk training data — is ruled out by suffix first.
static bool looksLikeMbox(const QFileInfo &info)
{
    static const QStringList kNoise = {
        QStringLiteral("msf"),    QStringLiteral("dat"),  QStringLiteral("html"),
        QStringLiteral("sqlite"), QStringLiteral("json")};
    if (!info.isFile() || kNoise.contains(info.suffix().toLower()))
        return false;
    if (QFileInfo::exists(info.filePath() + QLatin1String(".msf")))
        return true;
    QFile f(info.filePath());
    return f.open(QIODevice::ReadOnly) && f.read(5) == "From ";
}

/// Collects every mbox under \a dir, depth first. Thunderbird materialises
/// the folder hierarchy as "<name>" (the mail) plus "<name>.sbd/" (the
/// children), so stripping ".sbd" recovers the mailbox path.
static void collectMboxFiles(const QDir &dir, const QString &prefix, QList<MboxSource> *out)
{
    const auto entries =
        dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &info : entries) {
        if (info.isDir()) {
            QString name = info.fileName();
            if (name.endsWith(QLatin1String(".sbd")))
                name.chop(4);
            if (name.isEmpty())
                continue;
            collectMboxFiles(QDir(info.filePath()),
                             prefix.isEmpty() ? name : prefix + QLatin1Char('/') + name, out);
        } else if (looksLikeMbox(info)) {
            out->append({info.filePath(), prefix.isEmpty()
                                              ? info.fileName()
                                              : prefix + QLatin1Char('/') + info.fileName()});
        }
    }
}

/// Depth-first order with parents directly above their children — the sidebar
/// derives the tree from row order, so "Inbox" must sit immediately before
/// "Inbox/Work". Imported paths are always built with '/'.
static void sortMailboxTree(QList<MboxSource> *sources)
{
    std::sort(sources->begin(), sources->end(), [](const MboxSource &a, const MboxSource &b) {
        return mailBoxPathLess(a.mailBox, b.mailBox, QLatin1Char('/'));
    });
}

/// True for a real mbox From_ separator. Starting with "From " is not enough:
/// Thunderbird does not always quote body paragraphs that begin with "From ",
/// so the line must also end the way every writer's From_ line does — in a
/// ctime-style timestamp ("Thu Jan 01 10:00:00 2015", sender optional).
static bool isMboxSeparator(const QByteArray &line)
{
    if (!line.startsWith("From "))
        return false;
    static const QRegularExpression kFromLine(QStringLiteral(
        "^From (?:\\S+ )?\\w{3} \\w{3} [ \\d]?\\d \\d{1,2}:\\d\\d(?::\\d\\d)? \\d{4}"));
    return kFromLine.match(QString::fromLatin1(line)).hasMatch();
}

/// Streams \a path message by message: the classic mbox rule, a From_ line
/// right after a blank line (or at file start) begins a new message.
/// Returns false when the callback asked to stop.
static bool forEachMboxMessage(const QString &path, const std::function<bool(QByteArray &&)> &fn)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return true; // unreadable: nothing to deliver, but not a stop
    QByteArray current;
    bool prevBlank = true;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        if (prevBlank && isMboxSeparator(line)) {
            if (!current.isEmpty() && !fn(std::move(current)))
                return false;
            current = QByteArray(); // the separator line is not part of the message
        } else {
            current += line;
        }
        prevBlank = line == "\n" || line == "\r\n";
    }
    if (!current.isEmpty() && !fn(std::move(current)))
        return false;
    return true;
}

void MailClient::importThunderbird(const QUrl &dir)
{
    if (m_importThread) {
        Q_EMIT errorOccurred(tr("An import is already running."));
        return;
    }
    const QString path = dir.isLocalFile() ? dir.toLocalFile() : dir.toString();
    if (!QFileInfo(path).isDir()) {
        Q_EMIT importFinished(false, tr("Not a folder: %1").arg(path));
        return;
    }

    // The account appears in the pane right away, named after the profile
    // folder; its folder list fills in when the worker finishes. It is an
    // ordinary account in every visible way — the "local" flag that keeps it
    // from ever connecting stays out of sight.
    QString name = QDir(path).dirName();
    if (name.endsWith(QLatin1String(".sbd")))
        name.chop(4);
    if (name.isEmpty())
        name = tr("Imported mail");
    QSettings s = appSettings();
    QList<QVariantMap> accounts = readAccountArray(s);
    // A second import of the same profile gets its own account and its own
    // storage key — never silently merged into the first one's cache.
    QStringList takenNames, takenKeys;
    for (const QVariantMap &a : std::as_const(accounts)) {
        takenNames << a.value(QStringLiteral("user")).toString();
        takenKeys << a.value(QStringLiteral("cacheKey")).toString();
    }
    QString unique = name;
    for (int n = 2; takenNames.contains(unique)
                    || takenKeys.contains(QStringLiteral("import:") + unique);
         ++n)
        unique = name + QStringLiteral(" (%1)").arg(n);
    const QString storeKey = QStringLiteral("import:") + unique;

    QVariantMap account;
    account.insert(QStringLiteral("user"), unique);
    account.insert(QStringLiteral("local"), true);
    account.insert(QStringLiteral("cacheKey"), storeKey);
    accounts.append(account);
    writeAccountArray(s, accounts);
    Q_EMIT accountsChanged();

    setStatus(tr("Importing %1").arg(unique));
    const bool fts = m_store.ftsAvailable();
    m_importStop.storeRelaxed(0);
    m_importThread = QThread::create([this, path, storeKey, unique, fts] {
        QSqlDatabase db = MailStore::openWorkerConnection(QStringLiteral("mailstore-import"));
        QList<MboxSource> sources;
        if (db.isOpen()) {
            collectMboxFiles(QDir(path), QString(), &sources);
            sortMailboxTree(&sources);
        }
        QStringList folders;
        qint64 total = 0;
        bool stopped = false;
        for (const MboxSource &src : std::as_const(sources)) {
            if (m_importStop.loadRelaxed()) {
                stopped = true;
                break;
            }
            folders.append(src.mailBox);
            const QString scoped = storeKey + QChar(0x1f) + src.mailBox;
            // For the rare message without a Date header: the mbox's own
            // timestamp beats 1970 (and keeps the row out of ghost territory).
            const QDateTime fallbackDate = QFileInfo(src.filePath).lastModified();
            qint64 uid = 0;
            QList<MessageListModel::Header> headers;
            QList<MailStore::BodyWrite> bodies;
            auto flush = [&] {
                MailStore::storeHeadersOn(db, scoped, headers, fts);
                MailStore::writeBodiesOn(db, bodies);
                headers.clear();
                bodies.clear();
            };
            const bool completed = forEachMboxMessage(src.filePath, [&](QByteArray &&raw) {
                if (m_importStop.loadRelaxed())
                    return false;
                KMime::Message msg;
                msg.setContent(KMime::CRLFtoLF(raw));
                msg.parse();
                // Thunderbird's own flags travel inside the message: bit 0x1
                // is read, 0x8 is "deleted, not compacted yet" — not mail.
                const auto *status = msg.headerByType("X-Mozilla-Status");
                const uint flags =
                    status ? status->asUnicodeString().trimmed().toUInt(nullptr, 16) : 0;
                if (flags & 0x0008)
                    return true;
                MessageListModel::Header h;
                h.uid = ++uid;
                if (const auto *subject = std::as_const(msg).subject())
                    h.subject = subject->asUnicodeString();
                if (const auto *from = std::as_const(msg).from())
                    h.from = from->asUnicodeString();
                if (const auto *date = std::as_const(msg).date())
                    h.date = date->dateTime();
                if (!h.date.isValid())
                    h.date = fallbackDate;
                if (const auto *mid = std::as_const(msg).messageID(); mid && !mid->isEmpty())
                    h.msgid = QString::fromLatin1(mid->identifier());
                // Without Thunderbird flags everything counts as read — an
                // archive must not arrive as ten years of unread badges.
                h.seen = status ? (flags & 0x0001) : true;
                h.attachKind = MailStore::headIndicatesAttachment(msg.head())
                    ? MessageListModel::GenericAttachment
                    : MessageListModel::NoAttachment;
                h.crypto = PgpMime::storedKind(PgpMime::kindFromHead(msg.head()));
                // Deliberately no authInfo and no suspicious flag: imported
                // mail is never validated (SPF/DKIM verdicts from another
                // client's era would only produce noise).
                MailStore::BodyWrite w;
                w.scopedFolder = scoped;
                w.uid = h.uid;
                w.indexText = indexTextFor(&msg);
                w.parts = stripAttachments(&msg);
                if (!w.parts.isEmpty()) {
                    msg.assemble();
                    w.raw = msg.encodedContent();
                } else {
                    w.raw = raw;
                }
                headers.append(h);
                bodies.append(std::move(w));
                ++total;
                if (headers.size() >= 50) {
                    flush();
                    QMetaObject::invokeMethod(
                        this,
                        [this, unique, total] {
                            setStatus(tr("Importing %1 — %2 messages").arg(unique).arg(total));
                        },
                        Qt::QueuedConnection);
                }
                return true;
            });
            flush();
            if (!completed) {
                stopped = true;
                break;
            }
        }
        if (db.isOpen())
            db.close();
        QSqlDatabase::removeDatabase(QStringLiteral("mailstore-import"));

        QMetaObject::invokeMethod(
            this,
            [this, storeKey, folders, total, unique, stopped] {
                if (folders.isEmpty()) {
                    // Nothing found: take the just-created account back out
                    // rather than leaving an empty shell in the pane.
                    QSettings s = appSettings();
                    QList<QVariantMap> accounts = readAccountArray(s);
                    for (int i = accounts.size() - 1; i >= 0; --i) {
                        if (accounts.at(i).value(QStringLiteral("cacheKey")) == storeKey)
                            accounts.removeAt(i);
                    }
                    writeAccountArray(s, accounts);
                    Q_EMIT accountsChanged();
                    setStatus({});
                    Q_EMIT importFinished(false, tr("No Thunderbird mail found in that folder."));
                    return;
                }
                m_store.storeFolders(storeKey, folders);
                ++m_cachedFolderRevision;
                Q_EMIT cachedFoldersChanged();
                Q_EMIT accountsChanged();
                // On a fresh install the pane is already "on" the imported
                // slot (currentAccount 0), but nothing ever loaded it — switch
                // for real so the archive appears without a restart.
                {
                    QSettings s = appSettings();
                    const QList<QVariantMap> accounts = readAccountArray(s);
                    for (int i = 0; i < accounts.size(); ++i) {
                        if (accounts.at(i).value(QStringLiteral("cacheKey")) == storeKey
                            && i == m_currentAccount) {
                            switchAccountInternal(i, QString(), folders.first());
                            break;
                        }
                    }
                }
                if (stopped) {
                    setStatus(tr("Import interrupted"));
                    Q_EMIT importFinished(false,
                                          tr("Import of %1 was interrupted — what was already "
                                             "imported is browsable.")
                                              .arg(unique));
                    return;
                }
                setStatus(tr("Imported %1 — %2 in %3")
                              .arg(unique)
                              .arg(countNoun(total, "message", "messages"))
                              .arg(countNoun(folders.size(), "folder", "folders")));
                Q_EMIT importFinished(true,
                                      tr("Imported %1: %2 in %3.")
                                          .arg(unique)
                                          .arg(countNoun(total, "message", "messages"))
                                          .arg(countNoun(folders.size(), "folder", "folders")));
            },
            Qt::QueuedConnection);
    });
    connect(m_importThread, &QThread::finished, this, [this] {
        m_importThread->deleteLater();
        m_importThread = nullptr;
    });
    m_importThread->setPriority(QThread::LowPriority);
    m_importThread->start();
}

void MailClient::switchAccount(int index)
{
    switchAccountInternal(index, QString());
}

void MailClient::switchAccountInternal(int index, const QString &sessionPassword,
                                       const QString &targetFolder)
{
    qCDebug(logTrace, "switchAccountInternal(%d)  pendingWas=%s",
            index, qUtf8Printable(m_pendingFolder));
    QSettings s = appSettings();
    s.setValue(QStringLiteral("currentAccount"), index);
    m_currentAccount = index;

    // Set the destination before anything is torn down, so no observer ever
    // sees a blank selection: the sidebar highlight binds to it, and a moment
    // of "" made it fall back to row 0 — INBOX — mid-switch.
    m_selectedFolder = targetFolder.isEmpty() ? QStringLiteral("INBOX") : targetFolder;
    m_pendingFolder = targetFolder;
    Q_EMIT selectedFolderChanged();

    // Special-use folders belong to the account that advertised them. They are
    // re-detected by the next listFolders(), but until then a stale path would
    // point "Save as draft" (or the Sent copy) at the previous account's
    // mailbox — which the new connection may not even have.
    m_sentFolder.clear();
    m_draftsFolder.clear();
    m_allMailFolder.clear();
    Q_EMIT draftsFolderChanged();

    teardownSession();
    m_folderModel.setFolders({});
    m_messageModel.clear();
    m_oldestFetchedSeq = 0;
    m_searchActive = false;

    loadAccountFields();
    m_folderModel.setAccountKey(accountKey());
    m_store.setAccountKey(accountKey());
    // The switched-to account's sidebar and the target folder come straight
    // from cache; the network refresh merges into them once connected.
    loadCachedFolderModel();
    // Cached contents of the folder being opened — not INBOX's, which is what
    // made the message list show INBOX until the server's folder list arrived.
    const auto cached = m_store.cachedHeaders(m_selectedFolder);
    updatePageAnchor(cached);
    m_messageModel.setHeaders(cached);
    if (m_local) {
        // A local archive has no server and no secret — don't touch the
        // keyring, and never try to connect. The cache shown above is all
        // there is, and that is the point.
        ++m_walletGen; // cancel any in-flight wallet read
        m_password.clear();
        m_secretReady = true;
    } else if (!sessionPassword.isEmpty()) {
        ++m_walletGen; // cancel any in-flight wallet read
        m_password = sessionPassword;
        m_secretReady = true;
    } else {
        readWalletPassword();
    }

    Q_EMIT accountChanged();
    Q_EMIT accountsChanged();

    if (m_local) {
        setStatus(tr("%1 — %2 messages")
                      .arg(m_selectedFolder)
                      .arg(m_store.cachedHeaderCount(m_selectedFolder)));
        return;
    }
    if (!hasAccount())
        return;
    if (m_secretReady)
        connectAccount();
    else
        m_connectWhenReady = true;
}

/// The MIME construction shared by sendMail() and saveDraft(). \a strict is
/// what separates them: sending refuses a malformed recipient, while a draft
/// is saved from whatever is on screen — including an address halfway through
/// being typed — so it keeps the text as-is rather than throwing the draft away.
std::shared_ptr<KMime::Message> MailClient::composeMessage(
    const QString &to, const QString &cc, const QString &bcc, const QString &subject,
    const QString &html, const QList<QUrl> &attachments, bool strict,
    QStringList *toOut, QStringList *ccOut, QStringList *bccOut)
{
    // Defense against header/SMTP-command injection: no CR/LF survives, and
    // every recipient must look like a bare address.
    static const QRegularExpression crlfRe(QStringLiteral("[\\r\\n]"));
    static const QRegularExpression addrRe(
        QStringLiteral("^[^@\\s<>,;\"]+@[^@\\s<>,;\"]+\\.[^@\\s<>,;\"]+$"));
    auto parseAddresses = [this, strict](QString raw, bool *ok) -> QStringList {
        raw.remove(crlfRe);
        QStringList out;
        *ok = true;
        const QStringList parts = raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            const QString addr = part.trimmed();
            if (!addrRe.match(addr).hasMatch() && strict) {
                Q_EMIT sendFailed(tr("Invalid recipient address: %1").arg(addr));
                *ok = false;
                return {};
            }
            out.append(addr);
        }
        return out;
    };

    bool ok = false;
    const QStringList toList = parseAddresses(to, &ok);
    if (!ok)
        return {};
    const QStringList ccList = parseAddresses(cc, &ok);
    if (!ok)
        return {};
    const QStringList bccList = parseAddresses(bcc, &ok);
    if (!ok)
        return {};
    if (toOut)
        *toOut = toList;
    if (ccOut)
        *ccOut = ccList;
    if (bccOut)
        *bccOut = bccList;
    QString cleanSubject = subject;
    cleanSubject.remove(crlfRe);

    // --- Build the MIME message ---
    const QString fromAddr = ownAddress();

    auto msg = std::make_shared<KMime::Message>();
    // Built as a Mailbox rather than a "Name <addr>" string so KMime does the
    // quoting and RFC 2047 encoding — a display name may hold a comma, a
    // quote, or non-ASCII, none of which survive naive concatenation.
    KMime::Types::Mailbox fromMailbox;
    fromMailbox.setAddress(fromAddr.toUtf8());
    if (!m_displayName.isEmpty())
        fromMailbox.setName(m_displayName);
    msg->from()->addAddress(fromMailbox);
    // Organization is optional, and an empty one is not a header worth
    // sending — recipients would see a blank field rather than nothing.
    if (!m_organization.isEmpty()) {
        auto org = std::make_unique<KMime::Headers::Generic>("Organization");
        org->fromUnicodeString(m_organization);
        msg->setHeader(std::move(org));
    }
    for (const QString &addr : toList)
        msg->to()->fromUnicodeString(msg->to()->asUnicodeString().isEmpty()
                                         ? addr.trimmed()
                                         : msg->to()->asUnicodeString() + QStringLiteral(", ")
                                             + addr.trimmed());
    if (!ccList.isEmpty())
        msg->cc()->fromUnicodeString(ccList.join(QStringLiteral(", ")));
    msg->subject()->fromUnicodeString(cleanSubject);
    msg->date()->setDateTime(QDateTime::currentDateTime());
    msg->messageID()->generate(m_smtpHost.toUtf8());
    msg->userAgent()->fromUnicodeString(QStringLiteral("mailo/" MAILO_VERSION));

    const QString plain = QTextDocumentFragment::fromHtml(html).toPlainText();

    auto textPart = std::make_unique<KMime::Content>();
    textPart->contentType()->setMimeType("text/plain");
    textPart->contentType()->setCharset("utf-8");
    textPart->contentTransferEncoding()->setEncoding(KMime::Headers::CEquPr);
    textPart->fromUnicodeString(plain);

    msg->contentType()->setMimeType("multipart/mixed");
    msg->contentType()->setBoundary(KMime::multiPartBoundary());

    if (m_htmlMail) {
        // text + html alternative pair — receiving clients pick their format
        auto alternative = std::make_unique<KMime::Content>();
        alternative->contentType()->setMimeType("multipart/alternative");
        alternative->contentType()->setBoundary(KMime::multiPartBoundary());
        alternative->appendContent(std::move(textPart));

        auto htmlPart = std::make_unique<KMime::Content>();
        htmlPart->contentType()->setMimeType("text/html");
        htmlPart->contentType()->setCharset("utf-8");
        htmlPart->contentTransferEncoding()->setEncoding(KMime::Headers::CEquPr);
        htmlPart->fromUnicodeString(html);
        alternative->appendContent(std::move(htmlPart));

        msg->appendContent(std::move(alternative));
    } else {
        // Plain-text-only account: the text part stands alone.
        msg->appendContent(std::move(textPart));
    }

    {
        QMimeDatabase mimeDb;
        for (const QUrl &url : attachments) {
            QFile file(url.toLocalFile());
            if (!file.open(QIODevice::ReadOnly)) {
                Q_EMIT sendFailed(tr("Could not read attachment %1.").arg(url.toLocalFile()));
                return {};
            }
            const QString name = QFileInfo(file).fileName();
            auto part = std::make_unique<KMime::Content>();
            part->contentType()->setMimeType(
                mimeDb.mimeTypeForFile(url.toLocalFile()).name().toUtf8());
            part->contentType()->setName(name);
            part->contentDisposition()->setDisposition(KMime::Headers::CDattachment);
            part->contentDisposition()->setFilename(name);
            part->contentTransferEncoding()->setEncoding(KMime::Headers::CEbase64);
            part->setBody(file.readAll());
            msg->appendContent(std::move(part));
        }
    }
    msg->assemble();

    return msg;
}

QString MailClient::accountPgpKey() const
{
    QSettings s = appSettings();
    QString fp;
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    if (m_currentAccount >= 0 && m_currentAccount < count) {
        s.setArrayIndex(m_currentAccount);
        fp = s.value(QStringLiteral("pgpKeyFp")).toString();
    }
    s.endArray();
    return fp;
}

/// One of the active account's boolean OpenPGP settings. Read from QSettings
/// rather than cached: they change in the settings page, which rewrites the
/// array wholesale, and a stale copy here would sign mail the user just told
/// mailo not to sign.
static bool accountPgpFlag(int index, const char *key, bool fallback)
{
    QSettings s = appSettings();
    bool value = fallback;
    const int count = s.beginReadArray(QStringLiteral("accounts"));
    if (index >= 0 && index < count) {
        s.setArrayIndex(index);
        value = s.value(QString::fromLatin1(key), fallback).toBool();
    }
    s.endArray();
    return value;
}

bool MailClient::accountPgpSignByDefault() const
{
    return accountPgpFlag(m_currentAccount, "pgpSignByDefault", false);
}

bool MailClient::accountPgpEncryptByDefault() const
{
    return accountPgpFlag(m_currentAccount, "pgpEncryptByDefault", false);
}

bool MailClient::accountPgpAutoWkd() const
{
    return accountPgpFlag(m_currentAccount, "pgpAutoWkd", true);
}

void MailClient::applyOutgoingCrypto(const std::shared_ptr<KMime::Message> &msg,
                                     const QStringList &recipients, bool sign, bool encrypt,
                                     std::function<void(const QByteArray &)> done)
{
    const QByteArray plain = msg->encodedContent(KMime::NewlineType::CRLF);
    if (!sign && !encrypt) {
        done(plain);
        return;
    }
    if (!m_pgp || !m_pgp->available()) {
        Q_EMIT sendFailed(m_pgp ? m_pgp->unavailableReason()
                                : tr("OpenPGP support is not available."));
        return;
    }
    const QString ownKey = accountPgpKey();
    if (ownKey.isEmpty()) {
        Q_EMIT sendFailed(tr("This account has no OpenPGP key. Choose one in "
                             "Settings before signing or encrypting."));
        return;
    }

    // Every recipient must have a key, and so must the sender — leaving the
    // sender out is how a Sent copy becomes unreadable to the person who sent
    // it (doc/openpgp.md §6).
    QStringList encryptTo;
    if (encrypt) {
        QStringList missing;
        const QVariantMap keys = m_pgp->encryptionKeysFor(recipients);
        for (auto it = keys.cbegin(); it != keys.cend(); ++it) {
            const QString fp = it.value().toString();
            if (fp.isEmpty())
                missing.append(it.key());
            else if (!encryptTo.contains(fp))
                encryptTo.append(fp);
        }
        if (!missing.isEmpty()) {
            // Never a silent downgrade: the compose window is expected to have
            // asked already, so reaching here means something changed under it.
            Q_EMIT sendFailed(tr("No OpenPGP key for %1. The message was not "
                                 "sent — encrypting to the others would leave "
                                 "them out, and sending in the clear is not "
                                 "something Mailo will do on its own.")
                                  .arg(missing.join(QStringLiteral(", "))));
            return;
        }
        if (!encryptTo.contains(ownKey))
            encryptTo.append(ownKey);
    }

    const PgpMime::OutgoingParts parts = PgpMime::splitForCrypto(plain);
    if (!parts.valid) {
        Q_EMIT sendFailed(tr("The message could not be prepared for encryption."));
        return;
    }

    // Encryption of an already-built body, shared by "encrypt only" and the
    // second half of "sign, then encrypt".
    auto encryptStep = [this, done, encryptTo](const PgpMime::OutgoingParts &p) {
        const quint64 job = m_pgp->encryptTo(p.contentPart, encryptTo);
        if (!job) {
            Q_EMIT sendFailed(tr("The message could not be encrypted."));
            return;
        }
        auto *conn = new QMetaObject::Connection;
        *conn = connect(m_pgp, &PgpEngine::encryptFinished, this,
                        [this, conn, job, p, done](quint64 id, const QByteArray &cipher,
                                                   const QString &error) {
                            if (id != job)
                                return;
                            disconnect(*conn);
                            delete conn;
                            if (!error.isEmpty() || cipher.isEmpty()) {
                                Q_EMIT sendFailed(
                                    error.isEmpty() ? tr("The message could not be "
                                                         "encrypted.")
                                                    : error);
                                return;
                            }
                            done(PgpMime::buildEncrypted(p, cipher));
                        });
    };

    if (!sign) {
        encryptStep(parts);
        return;
    }

    // Sign first, encrypt the resulting multipart/signed — the order RFC 3156
    // specifies, and the one that keeps the signature hidden from anyone who
    // cannot decrypt the message.
    const quint64 job = m_pgp->signDetached(parts.contentPart, ownKey);
    if (!job) {
        Q_EMIT sendFailed(tr("The message could not be signed."));
        return;
    }
    auto *conn = new QMetaObject::Connection;
    *conn = connect(m_pgp, &PgpEngine::signFinished, this,
                    [this, conn, job, parts, encrypt, encryptStep, done](
                        quint64 id, const QByteArray &signature, const QString &micalg,
                        const QString &error) {
                        if (id != job)
                            return;
                        disconnect(*conn);
                        delete conn;
                        if (!error.isEmpty() || signature.isEmpty()) {
                            Q_EMIT sendFailed(error.isEmpty()
                                                  ? tr("The message could not be signed.")
                                                  : error);
                            return;
                        }
                        const QByteArray signedMsg =
                            PgpMime::buildSigned(parts, signature, micalg);
                        if (signedMsg.isEmpty()) {
                            Q_EMIT sendFailed(tr("The signed message could not be built."));
                            return;
                        }
                        if (!encrypt) {
                            done(signedMsg);
                            return;
                        }
                        const PgpMime::OutgoingParts signedParts =
                            PgpMime::splitForCrypto(signedMsg);
                        if (!signedParts.valid) {
                            Q_EMIT sendFailed(tr("The message could not be prepared "
                                                 "for encryption."));
                            return;
                        }
                        encryptStep(signedParts);
                    });
}

void MailClient::sendMail(const QString &to, const QString &cc, const QString &bcc,
                          const QString &subject, const QString &html,
                          const QList<QUrl> &attachments, bool sign, bool encrypt)
{
    const bool haveCredential = m_authType != 0 ? !m_accessToken.isEmpty()
                                                : !m_password.isEmpty();
    if (m_smtpHost.isEmpty() || m_user.isEmpty() || !haveCredential) {
        Q_EMIT sendFailed(tr("SMTP is not configured (check account settings)."));
        return;
    }
    QStringList toList, ccList, bccList;
    auto msg = composeMessage(to, cc, bcc, subject, html, attachments, true,
                              &toList, &ccList, &bccList);
    if (!msg)
        return;
    if (toList.isEmpty()) {
        Q_EMIT sendFailed(tr("No recipient given."));
        return;
    }
    const QString fromAddr = ownAddress();

    // Build -> (optional crypto) -> send. The crypto step is asynchronous —
    // gpg-agent may be putting a passphrase prompt on screen — so everything
    // that touches SMTP moves into the continuation (doc/openpgp.md §6).
    applyOutgoingCrypto(
        msg, toList + ccList + bccList, sign, encrypt,
        [this, toList, ccList, bccList, fromAddr](const QByteArray &wire) {
    // --- Ship it over SMTP ---
    // Sending is silent in the status log: success just closes the compose
    // window (mailSent), failure keeps it open with a dismissible dialog
    // (sendFailed). The busy spinner covers the in-progress state.
    setBusy(true);

    auto *session = new KSmtp::Session(m_smtpHost, quint16(m_smtpPort), this);
    switch (m_smtpSecurity) {
    case 0:
        session->setEncryptionMode(KSmtp::Session::TLS);
        break;
    case 2:
        session->setEncryptionMode(KSmtp::Session::Unencrypted);
        break;
    default:
        session->setEncryptionMode(KSmtp::Session::STARTTLS);
        break;
    }

    auto finish = [this, session, wire, toList, ccList, bccList](const QString &error) {
        setBusy(false);
        if (error.isEmpty()) {
            for (const QString &addr : toList + ccList + bccList)
                m_store.addRecipient(addr);
            Q_EMIT mailSent(); // compose window closes on this
            // The Sent copy is the encrypted one — it is also encrypted to the
            // sender's own key, so it stays readable.
            appendToSentFolder(wire);
        } else {
            // Keep the compose window open and show the full error there.
            Q_EMIT sendFailed(error);
        }
        session->quit();
        session->deleteLater();
    };

    connect(session, &KSmtp::Session::stateChanged, this,
            [this, session, wire, toList, ccList, bccList, fromAddr, finish](KSmtp::Session::State state) {
                if (state != KSmtp::Session::NotAuthenticated)
                    return;
                auto *login = new KSmtp::LoginJob(session);
                login->setUserName(m_user);
                if (m_authType != 0) {
                    login->setPreferedAuthMode(KSmtp::LoginJob::XOAuth2);
                    login->setPassword(m_accessToken);
                } else {
                    login->setPassword(m_password);
                }
                connect(login, &KJob::result, this,
                        [session, wire, toList, ccList, bccList, fromAddr, finish](KJob *job) {
                            if (job->error()) {
                                finish(job->errorString());
                                return;
                            }
                            auto *send = new KSmtp::SendJob(session);
                            send->setFrom(fromAddr);
                            send->setTo(toList);
                            if (!ccList.isEmpty())
                                send->setCc(ccList);
                            // Bcc goes into the SMTP envelope only — never a
                            // header, so recipients can't see the Bcc list.
                            if (!bccList.isEmpty())
                                send->setBcc(bccList);
                            send->setData(wire);
                            QObject::connect(send, &KJob::result, session, [finish](KJob *job) {
                                finish(job->error() ? job->errorString() : QString());
                            });
                            send->start();
                        });
                login->start();
            });
    session->open();
        });
}

void MailClient::saveDraft(const QString &to, const QString &cc, const QString &bcc,
                           const QString &subject, const QString &html,
                           const QList<QUrl> &attachments, qint64 replacesUid,
                           bool sign, bool encrypt)
{
    if (!m_connected || !m_session) {
        Q_EMIT sendFailed(tr("Cannot save a draft while offline."));
        return;
    }
    if (m_draftsFolder.isEmpty()) {
        Q_EMIT sendFailed(tr("No Drafts folder found on the server."));
        return;
    }
    // Bcc is a header here rather than an envelope field: a draft is not being
    // delivered, and dropping it would lose it when the draft is reopened.
    auto msg = composeMessage(to, cc, bcc, subject, html, attachments, false);
    if (!msg)
        return;
    if (!bcc.trimmed().isEmpty()) {
        msg->bcc()->fromUnicodeString(bcc.trimmed());
        msg->assemble();
    }

    // A draft of an encrypted message is encrypted to the sender's own key
    // before it goes anywhere: the Drafts folder is on the server, and a draft
    // left in the clear there defeats the point of encrypting the message it
    // becomes (doc/openpgp.md §6). Only the sender's key — the recipients are
    // still being decided, and one of them may not even be a recipient yet.
    // Never signed: a signature over a half-written message would be a
    // statement the user has not made.
    const QString ownKey = accountPgpKey();
    const bool encryptDraft = encrypt && !ownKey.isEmpty();
    Q_UNUSED(sign)
    applyOutgoingCrypto(
        msg, {ownAddress()}, false, encryptDraft,
        [this, replacesUid](const QByteArray &wire) {
    setBusy(true);
    auto *append = new KIMAP::AppendJob(m_session);
    append->setMailBox(m_draftsFolder);
    append->setContent(wire);
    // \\Draft is what makes other clients treat it as editable rather than as
    // received mail; \\Seen keeps it from showing up as unread.
    append->setFlags({QByteArrayLiteral("\\Draft"), QByteArrayLiteral("\\Seen")});
    append->setInternalDate(QDateTime::currentDateTime());
    connect(append, &KJob::result, this, [this, replacesUid](KJob *job) {
        setBusy(false);
        if (job->error()) {
            Q_EMIT sendFailed(tr("Saving the draft failed: %1").arg(job->errorString()));
            return;
        }
        setStatus(tr("Draft saved to %1").arg(m_draftsFolder));
        Q_EMIT draftSaved(); // the compose window closes on this
        // IMAP cannot replace a message in place, so re-saving is append-new
        // then delete-old. Both steps happen here, in that order, and the list
        // is refreshed once at the end — refreshing in between showed the old
        // and new copies side by side, and not refreshing at all left the new
        // one invisible until the next background sync.
        if (replacesUid > 0) {
            discardDraft(replacesUid); // refreshes once the expunge lands
        } else if (viewingDrafts()) {
            refreshCurrentFolder();
        }
    });
    append->start();
        });
}

void MailClient::appendToSentFolder(const QByteArray &rawMessage)
{
    if (!m_connected || !m_session) {
        setStatus(tr("Sent — not copied to the Sent folder (IMAP offline)"));
        return;
    }
    if (m_sentFolder.isEmpty()) {
        setStatus(tr("Sent — no Sent folder found on the server to copy into"));
        return;
    }
    auto *append = new KIMAP::AppendJob(m_session);
    append->setMailBox(m_sentFolder);
    append->setContent(rawMessage);
    append->setFlags({QByteArrayLiteral("\\Seen")});
    append->setInternalDate(QDateTime::currentDateTime());
    connect(append, &KJob::result, this, [this](KJob *job) {
        // Success is silent — the closed compose window and the message showing
        // up in Sent are confirmation enough. Only a copy failure is logged.
        if (job->error()) {
            Q_EMIT errorOccurred(
                tr("Sent, but copying to %1 failed: %2").arg(m_sentFolder, job->errorString()));
        }
    });
    append->start();
}

QString MailClient::ownAddress() const
{
    // What the account explicitly says it sends as, when it says so. The rest
    // is the pre-"email"-field fallback: it only ever guessed right when the
    // login happened to be the address, or the mail domain happened to match
    // the SMTP host's parent. Kept so accounts saved before the field existed
    // behave exactly as they did.
    if (!m_email.isEmpty())
        return m_email;
    return m_user.contains(QLatin1Char('@'))
        ? m_user
        : m_user + QLatin1Char('@') + m_smtpHost.section(QLatin1Char('.'), 1);
}

QString MailClient::signatureBlock() const
{
    // The signature editor hands us a full QTextDocument HTML page; splicing
    // <html>/<body> tags into another body string confuses the rich-text
    // parser, so cut the fragment out of the body element first.
    if (QTextDocumentFragment::fromHtml(m_signature).toPlainText().trimmed().isEmpty())
        return {};
    QString fragment = m_signature;
    static const QRegularExpression bodyRe(
        QStringLiteral("<body[^>]*>(.*)</body>"),
        QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
    if (const auto m = bodyRe.match(fragment); m.hasMatch())
        fragment = m.captured(1);
    return fragment;
}

// Inner <body> content of an HTML mail, prepared for embedding into the
// compose editor: <style>/<script> blocks go (QTextDocument would render
// their text), and cid: images go (the editor cannot resolve them).
static QString quotableHtml(QString html)
{
    static const QRegularExpression bodyRe(
        QStringLiteral("<body[^>]*>(.*)</body>"),
        QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
    if (const auto m = bodyRe.match(html); m.hasMatch())
        html = m.captured(1);
    static const QRegularExpression styleScriptRe(
        QStringLiteral("<(style|script)\\b[^>]*>.*?</\\1\\s*>"),
        QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
    html.remove(styleScriptRe);
    static const QRegularExpression cidImgRe(
        QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*[\"']cid:[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    html.remove(cidImgRe);
    return html;
}

QString MailClient::newMessageBody() const
{
    const QString sig = signatureBlock();
    return sig.isEmpty() ? QString() : QStringLiteral("<p><br></p>") + sig;
}

QString MailClient::loadHtmlFile(const QUrl &fileUrl)
{
    QFile file(fileUrl.toLocalFile());
    // A signature is a short snippet — reject anything that clearly isn't.
    constexpr qint64 maxSize = 1 * 1024 * 1024;
    if (!file.open(QIODevice::ReadOnly) || file.size() > maxSize) {
        Q_EMIT errorOccurred(file.size() > maxSize
                                 ? tr("%1 is too large for a signature (max 1 MB).")
                                       .arg(fileUrl.fileName())
                                 : tr("Could not read %1.").arg(fileUrl.toLocalFile()));
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QVariantMap MailClient::replyData(bool replyAll)
{
    return replyDataFor(m_reading, replyAll);
}

QVariantMap MailClient::replyDataFor(MessageContext *ctx, bool replyAll)
{
    if (!ctx->m_message)
        return {};
    const KMime::Message *msg = ctx->m_message.get();

    // Bare addr-specs only — that is the shape sendMail() accepts back.
    auto addressesOf = [](const auto *header) {
        QStringList out;
        if (!header)
            return out;
        const auto mailboxes = header->mailboxes();
        for (const auto &mb : mailboxes) {
            const QString addr = QString::fromLatin1(mb.address());
            if (addr.contains(QLatin1Char('@')))
                out.append(addr);
        }
        return out;
    };

    // Reply target: Reply-To when the sender set one, else From.
    QStringList to = addressesOf(msg->replyTo());
    if (to.isEmpty())
        to = addressesOf(msg->from());
    to.removeDuplicates();

    // Reply-all: everyone in the original To/Cc except us and the target.
    QStringList cc;
    if (replyAll) {
        QSet<QString> seen{ownAddress().toLower()};
        for (const QString &addr : std::as_const(to))
            seen.insert(addr.toLower());
        const QStringList others = addressesOf(msg->to()) + addressesOf(msg->cc());
        for (const QString &addr : others) {
            if (!seen.contains(addr.toLower())) {
                seen.insert(addr.toLower());
                cc.append(addr);
            }
        }
    }

    QString subject = msg->subject() ? msg->subject()->asUnicodeString() : QString();
    if (!subject.startsWith(QLatin1String("Re:"), Qt::CaseInsensitive))
        subject = QStringLiteral("Re: ") + subject;

    // Quote the HTML part when there is one so the reply keeps the original's
    // formatting; the plain-text part is the fallback.
    const QString quoted = ctx->m_htmlBody.isEmpty()
        ? ctx->m_textBody.trimmed().toHtmlEscaped()
              .replace(QLatin1Char('\n'), QLatin1String("<br>"))
        : quotableHtml(ctx->m_htmlBody);
    const QString fromDisplay = msg->from() ? msg->from()->asUnicodeString() : QString();
    QString date;
    if (msg->date()) {
        date = msg->date()->dateTime().toLocalTime().toString(
            m_dateFormat + QStringLiteral(" hh:mm"));
    }
    // Signature goes above the quoted original, right under the cursor line.
    const QString body = QStringLiteral("<p><br></p>") + signatureBlock()
        + QStringLiteral("<p>")
        + tr("On %1, %2 wrote:").arg(date, fromDisplay).toHtmlEscaped()
        + QStringLiteral("</p><blockquote>") + quoted
        + QStringLiteral("</blockquote>");

    return {{QStringLiteral("to"), to.join(QStringLiteral(", "))},
            {QStringLiteral("cc"), cc.join(QStringLiteral(", "))},
            {QStringLiteral("subject"), subject},
            {QStringLiteral("body"), body}};
}

/// The message as it stands, for reopening a draft in the composer. Unlike
/// replyData/forwardData nothing is quoted or prefixed — a draft is resumed,
/// not responded to.
QVariantMap MailClient::draftData()
{
    MessageContext *ctx = m_reading;
    if (!ctx->m_message)
        return {};
    const KMime::Message *msg = ctx->m_message.get();

    auto addressesOf = [](const auto *header) {
        QStringList out;
        if (!header)
            return out;
        const auto mailboxes = header->mailboxes();
        for (const auto &mb : mailboxes) {
            const QString addr = QString::fromLatin1(mb.address());
            if (addr.contains(QLatin1Char('@')))
                out.append(addr);
        }
        return out;
    };

    // The HTML part when the draft has one, so formatting survives a
    // save/reopen round trip; the plain part is the fallback.
    const QString body = !ctx->m_htmlBody.isEmpty()
        ? ctx->m_htmlBody
        : ctx->m_textBody.toHtmlEscaped().replace(QLatin1Char('\n'), QLatin1String("<br>"));

    return {{QStringLiteral("to"), addressesOf(msg->to()).join(QStringLiteral(", "))},
            {QStringLiteral("cc"), addressesOf(msg->cc()).join(QStringLiteral(", "))},
            {QStringLiteral("bcc"), addressesOf(msg->bcc()).join(QStringLiteral(", "))},
            {QStringLiteral("subject"),
             msg->subject() ? msg->subject()->asUnicodeString() : QString()},
            {QStringLiteral("body"), body},
            {QStringLiteral("uid"), ctx->m_uid}};
}

/// Removes the draft a composer was opened from, once its replacement has been
/// sent or re-saved — otherwise editing a draft would leave the old copy
/// beside the new one every time.
void MailClient::discardDraft(qint64 uid)
{
    if (uid <= 0 || m_draftsFolder.isEmpty() || !m_connected || !m_session)
        return;
    KIMAP::ImapSet set;
    set.add(uid);

    // Browsing SELECTs read-only (EXAMINE); STORE and EXPUNGE need read-write.
    auto *select = new KIMAP::SelectJob(m_session);
    select->setMailBox(m_draftsFolder);
    connect(select, &KJob::result, this, [this, set, uid](KJob *job) {
        if (job->error())
            return;
        m_folderReadWrite = true;
        auto *store = new KIMAP::StoreJob(m_session);
        store->setUidBased(true);
        store->setSequenceSet(set);
        store->setMode(KIMAP::StoreJob::AppendFlags);
        store->setFlags({QByteArrayLiteral("\\Deleted")});
        connect(store, &KJob::result, this, [this, uid](KJob *job) {
            if (job->error())
                return;
            auto *expunge = new KIMAP::ExpungeJob(m_session);
            connect(expunge, &KJob::result, this, [this, uid](KJob *) {
                // Drop it from the open list too, when Drafts is on screen.
                if (viewingDrafts()) {
                    m_messageModel.removeByUids({uid});
                    m_store.removeMessages(m_draftsFolder, {uid});
                    // The replacement was APPENDed just before this; show it
                    // in the same breath as the old row disappearing.
                    refreshCurrentFolder();
                }
            });
            expunge->start();
        });
        store->start();
    });
    select->start();
}

QVariantMap MailClient::forwardData()
{
    return forwardDataFor(m_reading);
}

QVariantMap MailClient::forwardDataFor(MessageContext *ctx)
{
    if (!ctx->m_message)
        return {};
    const KMime::Message *msg = ctx->m_message.get();

    QString subject = msg->subject() ? msg->subject()->asUnicodeString() : QString();
    if (!subject.startsWith(QLatin1String("Fwd:"), Qt::CaseInsensitive)
        && !subject.startsWith(QLatin1String("Fw:"), Qt::CaseInsensitive))
        subject = QStringLiteral("Fwd: ") + subject;

    const QString quoted = ctx->m_htmlBody.isEmpty()
        ? ctx->m_textBody.trimmed().toHtmlEscaped()
              .replace(QLatin1Char('\n'), QLatin1String("<br>"))
        : quotableHtml(ctx->m_htmlBody);
    const QString from = msg->from() ? msg->from()->asUnicodeString() : QString();
    const QString origTo = msg->to() ? msg->to()->asUnicodeString() : QString();
    const QString origSubject =
        msg->subject() ? msg->subject()->asUnicodeString() : QString();
    QString date;
    if (msg->date()) {
        date = msg->date()->dateTime().toLocalTime().toString(
            m_dateFormat + QStringLiteral(" hh:mm"));
    }

    QString header = tr("---------- Forwarded message ----------");
    header += QStringLiteral("<br>") + tr("From: %1").arg(from.toHtmlEscaped());
    header += QStringLiteral("<br>") + tr("Date: %1").arg(date.toHtmlEscaped());
    header += QStringLiteral("<br>") + tr("Subject: %1").arg(origSubject.toHtmlEscaped());
    header += QStringLiteral("<br>") + tr("To: %1").arg(origTo.toHtmlEscaped());

    // Signature goes above the forwarded block, right under the cursor line.
    const QString body = QStringLiteral("<p><br></p>") + signatureBlock()
        + QStringLiteral("<p>") + header
        + QStringLiteral("</p><blockquote>") + quoted
        + QStringLiteral("</blockquote>");

    return {{QStringLiteral("to"), QString()},
            {QStringLiteral("cc"), QString()},
            {QStringLiteral("subject"), subject},
            {QStringLiteral("body"), body}};
}

QStringList MailClient::recipientSuggestions(const QString &prefix)
{
    return m_store.recipientCompletions(prefix);
}

void MailClient::scoreHeader(MessageListModel::Header &h, const QByteArray &head,
                             const QSet<QString> &knownSenders)
{
    SpamHeuristics::Context ctx;
    ctx.knownCorrespondent = knownSenders.contains(SpamHeuristics::addressOf(h.from));
    ctx.authInfo = h.authInfo;
    // headerFromImap() already reduced the trusted Authentication-Results to
    // this; re-deriving it here would be a second chance to disagree with the
    // "!" marker the list already shows.
    ctx.authFailed = h.suspicious;
    ctx.authPassed = authResultsPassed(h.authInfo);
    ctx.crypto = h.crypto;
    const SpamHeuristics::Score s = SpamHeuristics::score({head, {}, {}}, ctx);
    h.spamScore = s.total;
    h.spamState = s.exempt ? 3 : 1;
    h.spamDetail = s.exempt ? s.exemptReason : s.explanation();
}

void MailClient::appendScoredHeaders(QList<MessageListModel::Header> &out,
                                     const QMap<qint64, KIMAP::Message> &messages,
                                     const QStringList &authDomains, qint64 minUid)
{
    QList<MessageListModel::Header> batch;
    QList<QByteArray> heads;
    QSet<QString> senders;
    for (auto it = messages.cbegin(); it != messages.cend(); ++it) {
        if (!imapEntryUsable(it.value()) || it.value().uid <= minUid)
            continue;
        batch.append(headerFromImap(it.value(), authDomains));
        heads.append(it.value().message->head());
        senders.insert(SpamHeuristics::addressOf(batch.constLast().from));
    }
    if (batch.isEmpty())
        return;
    // One allowlist query for the whole FETCH batch rather than one per
    // message: this runs on the GUI thread between deliveries, and a few
    // hundred round trips there would be felt in the list.
    const QSet<QString> known = m_store.knownCorrespondents(senders);
    for (qsizetype i = 0; i < batch.size(); ++i)
        scoreHeader(batch[i], heads.at(i), known);
    out += batch;
}

void MailClient::harvestRecipients(const KMime::Message *msg)
{
    auto add = [this](const auto *header) {
        if (!header)
            return;
        const auto mailboxes = header->mailboxes();
        for (const auto &mb : mailboxes)
            m_store.addRecipient(QString::fromLatin1(mb.address()),
                                 mb.hasName() ? mb.name() : QString());
    };
    add(std::as_const(*msg).to());
    add(std::as_const(*msg).cc());
}

void MailClient::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    Q_EMIT busyChanged();
}

// Collapse key for the breadcrumb: statuses about the same subject replace
// each other in place instead of stacking. Folder statuses are formatted as
// "FOLDER — detail…", so the key is the part before the em-dash — every
// "INBOX — checking… / cached, refreshing… / N of M headers…" update becomes
// one INBOX crumb that updates in place. Statuses without an em-dash (errors,
// "Loaded from cache", "Message sent") each key on their own digit-stripped
// text, so distinct ones still accrue but repeats don't.
static QString statusStem(const QString &s)
{
    const int dash = s.indexOf(QStringLiteral(" — "));
    if (dash > 0)
        return s.left(dash).simplified();
    QString stem;
    stem.reserve(s.size());
    for (const QChar &c : s) {
        if (!c.isDigit())
            stem.append(c);
    }
    return stem.simplified();
}

void MailClient::setStatus(const QString &text)
{
    // Keep a short breadcrumb of recent statuses instead of overwriting, so the
    // user can see the recent history (e.g. "fetching older… · cached ·
    // connected"), newest first. Progress updates that only change their
    // numbers replace the head rather than piling up.
    // An empty status means "the transient op finished" — don't add or wipe
    // anything; the trail keeps showing the last real state (folder sync etc.).
    if (text.isEmpty())
        return;

    const int maxTrail = 6;
    // Collapse against the whole trail, not just its head. Two operations
    // reporting progress at once take turns at the head, so a head-only check
    // never matched and the trail filled with one alternating pair repeated
    // over and over — "Importing Mail — 43931 messages · Trash — caching 1
    // body · Importing Mail — 43581 messages · …". One crumb per subject is
    // what the collapse was always meant to give.
    int existing = -1;
    const QString stem = statusStem(text);
    for (int i = 0; i < m_statusTrail.size(); ++i) {
        if (statusStem(m_statusTrail.at(i)) == stem) {
            existing = i;
            break;
        }
    }
    if (existing >= 0) {
        if (m_statusTrail.at(existing) == text)
            return; // identical — nothing changed
        // Updated in place rather than moved to the front: with two operations
        // running, promoting every update would have them swapping positions
        // on each tick, which reads as flicker even though it is only ever two
        // crumbs. Their numbers change where they stand.
        m_statusTrail[existing] = text;
    } else {
        m_statusTrail.prepend(text);
        while (m_statusTrail.size() > maxTrail)
            m_statusTrail.removeLast();
    }

    const QString composed = m_statusTrail.join(QStringLiteral("  ·  "));
    if (m_statusText == composed)
        return;
    m_statusText = composed;
    Q_EMIT statusTextChanged();
}

void MailClient::copyToClipboard(const QString &text) const
{
    if (auto *cb = QGuiApplication::clipboard())
        cb->setText(text);
}

QString MailClient::aboutText() const
{
    // ABOUT.md is compiled into the binary as a Qt resource at build time.
    QFile f(QStringLiteral(":/ABOUT.md"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

/// Cached "how many bodies are still missing here". The underlying query is a
/// LEFT JOIN over the whole folder — 200 ms on a large one — and the backfill
/// asked for it on every 600 ms tick purely to word a status crumb. It is
/// recomputed on folder change and when new headers arrive, and counted down
/// locally as bodies land.
int MailClient::missingBodiesIn(const QString &folder)
{
    if (folder != m_missingBodiesFolder || m_missingBodies < 0) {
        m_missingBodiesFolder = folder;
        m_missingBodies = m_store.missingBodyCount(folder);
    }
    return m_missingBodies;
}

void MailClient::noteBodyStored(const QString &folder)
{
    if (folder == m_missingBodiesFolder && m_missingBodies > 0)
        --m_missingBodies;
}

void MailClient::invalidateMissingBodies()
{
    m_missingBodies = -1;
}

QString MailClient::openFolderSyncStatus(const QString &folder)
{
    if (folder.isEmpty() || folder != m_selectedFolder)
        return {};

    QStringList parts;
    // Header sync: still older messages to pull from the server.
    if (m_folderMessageCount > 0 && m_oldestFetchedSeq > 1) {
        const qint64 synced = m_folderMessageCount - m_oldestFetchedSeq + 1;
        parts << tr("%1 of %2 headers")
                     .arg(synced)
                     .arg(m_folderMessageCount);
    }
    // Body caching: message bodies still to fetch for offline/search.
    const int missingBodies = missingBodiesIn(folder);
    if (missingBodies > 0) {
        parts << (missingBodies == 1
                      ? tr("caching 1 body")
                      : tr("caching %1 bodies").arg(missingBodies));
    }
    if (parts.isEmpty())
        return {};
    // "INBOX — 500 of 1200 headers · caching 42 bodies…"
    return tr("%1 — %2").arg(folder, parts.join(QStringLiteral(" · ")));
}

void MailClient::teardownSession()
{
    m_spamSwept = false; // the next connection sweeps again
    m_keepAlive.stop();
    m_backfillTimer.stop();
    m_backfill = false;
    m_bodyBackfill = false;
    resetBackfillBackoff();
    m_folderBackfillQueue.clear();
    m_backfillFolder.clear();
    m_folderBackfillPassDone = false;
    m_prefetchQueue.clear();
    m_prefetching = false;
    for (const auto &conn : std::as_const(m_bodyPool)) {
        if (conn->session)
            conn->session->deleteLater();
    }
    m_bodyPool.clear();
    m_bodyPoolBroken = false;
    stopIdle();
    m_syncReady = false;
    m_syncFolder.clear();
    if (m_syncSession) {
        m_syncSession->disconnect(this); // same reason as the main session below
        m_syncSession->deleteLater();
        m_syncSession.clear();
    }
    if (m_session) {
        // Drop our handlers before closing. close() makes the socket emit
        // connectionLost, which is meant for an *unexpected* drop: it stashes
        // the open folder in m_pendingFolder to reopen after reconnecting.
        // During a deliberate teardown that signal arrives asynchronously —
        // after openFolderInAccount() has already set m_pendingFolder to the
        // folder the user clicked — and overwrote it with the new account's
        // default INBOX, so switching accounts always landed on INBOX.
        m_session->disconnect(this);
        m_session->close();
        m_session->deleteLater();
        m_session = nullptr;
    }
    if (m_connected) {
        m_connected = false;
        Q_EMIT connectedChanged();
    }
    // m_selectedFolder deliberately survives a teardown. connectAccount() tears
    // the session down as its first step, so clearing here wiped the folder an
    // account switch had just chosen — leaving every consumer (the sidebar
    // highlight, openCurrent()'s "already open?" guard, and the keep-current
    // fallback when the folder list arrives) comparing against an empty string.
    // Everything that acts on it is already guarded by m_connected, and the
    // paths that really mean "nothing is open" clear it themselves.
}

void MailClient::configureLogin(KIMAP::LoginJob *login) const
{
    login->setUserName(m_user);
    if (m_authType != 0) {
        login->setAuthenticationMode(KIMAP::LoginJob::XOAuth2);
        login->setPassword(m_accessToken);
    } else {
        login->setAuthenticationMode(KIMAP::LoginJob::Plain);
        login->setPassword(m_password);
    }
    switch (m_security) {
    case StartTls:
        login->setEncryptionMode(KIMAP::LoginJob::STARTTLS);
        break;
    case None:
        login->setEncryptionMode(KIMAP::LoginJob::Unencrypted);
        break;
    default:
        login->setEncryptionMode(KIMAP::LoginJob::SSLorTLS);
        break;
    }
}

void MailClient::stopIdle()
{
    m_idleJob.clear(); // owned by the session; dies with it
    if (m_idleSession) {
        m_idleSession->deleteLater();
        m_idleSession.clear();
    }
}

/// IMAP IDLE push on a dedicated connection: the server tells us about new
/// mail in the open folder instantly instead of waiting for a manual refresh.
/// Best-effort — any failure just means we fall back to manual refreshes.
void MailClient::startIdle()
{
    stopIdle();
    if (!m_connected || !hasAccount() || m_selectedFolder.isEmpty())
        return;

    m_idleSession = new KIMAP::Session(m_host, quint16(m_port), this);
    auto *login = new KIMAP::LoginJob(m_idleSession);
    configureLogin(login);
    connect(login, &KJob::result, this, [this](KJob *job) {
        if (job->error() || !m_idleSession) {
            qWarning() << "mailo: idle login failed:" << job->errorString();
            return;
        }
        auto *select = new KIMAP::SelectJob(m_idleSession);
        select->setMailBox(m_selectedFolder);
        select->setOpenReadOnly(true);
        connect(select, &KJob::result, this, [this](KJob *job) {
            if (job->error() || !m_idleSession) {
                qWarning() << "mailo: idle select failed:" << job->errorString();
                return;
            }
            auto *idle = new KIMAP::IdleJob(m_idleSession);
            m_idleJob = idle;
            connect(idle, &KIMAP::IdleJob::mailBoxStats, this,
                    [this](KIMAP::IdleJob *, const QString &mailBox, int, int) {
                        if (mailBox == m_selectedFolder)
                            refreshCurrentFolder();
                    });
            connect(idle, &KJob::result, this, [this](KJob *job) {
                // Server ended IDLE (timeout, capability missing, …) — retry
                // later unless we tore the session down ourselves.
                if (job->error())
                    qWarning() << "mailo: idle ended:" << job->errorString();
                if (m_idleSession && m_connected)
                    QTimer::singleShot(30 * 1000, this, [this] { startIdle(); });
            });
            idle->start();
        });
        select->start();
    });
    login->start();
}

/// Third IMAP connection, dedicated to background transfers (header backfill,
/// body prefetch). KIMAP runs jobs on one connection strictly in order, so
/// without it a folder click queues behind whatever multi-second FETCH the
/// backfill has in flight. Best-effort: if the server refuses the extra
/// connection, background work falls back to the main session.
void MailClient::startSyncSession()
{
    if (m_syncSession)
        return;
    m_syncSession = new KIMAP::Session(m_host, quint16(m_port), this);
    const auto drop = [this] {
        // A connection dropped while a backfill fetch was in flight is the
        // server pushing back on heavy fetching (Gmail does this rather than
        // failing the job cleanly). Treat it as throttling: grow the backoff
        // so we don't reconnect and immediately hammer it at full speed again.
        const bool wasFetching = m_backfill || bodyFetchActive();
        m_syncReady = false;
        m_syncFolder.clear();
        m_backfill = false;
        if (m_syncSession) {
            m_syncSession->deleteLater();
            m_syncSession.clear();
        }
        if (wasFetching)
            backoffBackfill();
    };
    connect(m_syncSession, &KIMAP::Session::connectionFailed, this, drop);
    connect(m_syncSession, &KIMAP::Session::connectionLost, this, drop);
    auto *login = new KIMAP::LoginJob(m_syncSession);
    configureLogin(login);
    connect(login, &KJob::result, this, [this, drop](KJob *job) {
        if (job->error() || !m_syncSession) {
            qWarning() << "mailo: sync-session login failed:" << job->errorString();
            drop();
            return;
        }
        m_syncReady = true;
    });
    login->start();
}

void MailClient::withSyncSession(const QString &folder,
                                 const std::function<void(KIMAP::Session *)> &fn)
{
    if (!m_syncSession || !m_syncReady) {
        // The main session has the folder selected only while it is the open
        // one — anything else cannot be served right now.
        fn(m_session && folder == m_selectedFolder ? m_session.data() : nullptr);
        return;
    }
    if (m_syncFolder == folder) {
        fn(m_syncSession.data());
        return;
    }
    auto *select = new KIMAP::SelectJob(m_syncSession);
    select->setMailBox(folder);
    select->setOpenReadOnly(true);
    connect(select, &KJob::result, this, [this, folder, fn](KJob *job) {
        if (job->error() || !m_syncSession) {
            fn(nullptr);
            return;
        }
        m_syncFolder = folder;
        fn(m_syncSession.data());
    });
    select->start();
}

void MailClient::refreshCurrentFolder()
{
    if (!m_connected || !m_session || m_selectedFolder.isEmpty() || m_busy)
        return;
    auto *select = new KIMAP::SelectJob(m_session);
    select->setMailBox(m_selectedFolder);
    select->setOpenReadOnly(true);
    m_folderReadWrite = false;
    connect(select, &KJob::result, this, [this](KJob *job) {
        if (job->error())
            return;
        auto *select = static_cast<KIMAP::SelectJob *>(job);
        const int count = select->messageCount();
        if (count <= 0)
            return;
        m_folderMessageCount = count;
        // Newest few headers; appendHeaders() dedupes and updates in place.
        fetchHeaders(qMax(qint64(1), qint64(count) - 19), count, true);
    });
    select->start();
}

void MailClient::acquireTokenAndConnect()
{
    if (!m_oauth) {
        m_oauth = new OAuthHelper(this);
        connect(m_oauth, &OAuthHelper::tokensReady, this,
                [this](const QString &accessToken, const QString &refreshToken,
                       const QDateTime &expiry) {
                    m_accessToken = accessToken;
                    m_accessTokenExpiry = expiry;
                    if (!refreshToken.isEmpty() && refreshToken != m_refreshToken) {
                        m_refreshToken = refreshToken;
                        auto *write = new QKeychain::WritePasswordJob(kWalletService, this);
                        write->setKey(oauthWalletKey());
                        write->setTextData(refreshToken);
                        write->start();
                    }
                    connectAccount();
                });
        connect(m_oauth, &OAuthHelper::failed, this, [this](const QString &message) {
            setBusy(false);
            // A dead refresh token would fail forever — drop it so the next
            // attempt goes through the browser again.
            if (!m_refreshToken.isEmpty()) {
                m_refreshToken.clear();
                auto *del = new QKeychain::DeletePasswordJob(kWalletService, this);
                del->setKey(oauthWalletKey());
                del->start();
            }
            setStatus(tr("Sign-in failed"));
            Q_EMIT errorOccurred(message);
        });
    }
    const auto provider = OAuthHelper::Provider(m_authType);

    // Built-in desktop-client IDs so sign-in needs no manual setup (same
    // publicly-documented installed-app credentials Thunderbird ships; a
    // clientId in the account config overrides them).
    QString clientId = m_clientId;
    QString clientSecret = m_clientSecret;
    if (clientId.isEmpty()) {
        if (provider == OAuthHelper::Gmail) {
            clientId = QStringLiteral(
                "406964657835-aq8lmia8j95dhl1a2bvharmfk3t1hgqj.apps.googleusercontent.com");
            clientSecret = QStringLiteral("kSmqreRr0qwBWJgbf5Y-PjSU");
        } else {
            clientId = QStringLiteral("9e5f94bc-e8a4-4e73-b8be-63364c29d753");
            clientSecret.clear();
        }
    }

    setBusy(true);
    if (!m_refreshToken.isEmpty()) {
        setStatus(tr("Refreshing sign-in"));
        m_oauth->refresh(provider, clientId, clientSecret, m_refreshToken);
    } else {
        setStatus(tr("Sign in in your browser"));
        m_oauth->authorize(provider, clientId, clientSecret);
    }
}

void MailClient::connectAccount()
{
    // A local archive never connects — silently, so nothing in the UI ever
    // hints that this account is anything other than a quiet one.
    if (m_local)
        return;
    if (!hasAccount()) {
        Q_EMIT errorOccurred(tr("No account configured yet."));
        return;
    }
    if (!m_secretReady) {
        // Wallet lookup still in flight — connect as soon as it lands.
        m_connectWhenReady = true;
        setStatus(tr("Waiting for wallet"));
        return;
    }
    if (m_authType != 0) {
        // OAuth: make sure we hold a live access token first.
        if (m_accessToken.isEmpty()
            || m_accessTokenExpiry <= QDateTime::currentDateTimeUtc()) {
            acquireTokenAndConnect();
            return;
        }
    } else if (m_password.isEmpty()) {
        Q_EMIT errorOccurred(tr("No password set for this account."));
        return;
    }

    // Deliberately do NOT clear the folder/message models here: the cached
    // view stays on screen until the fresh server data merges into it —
    // clearing caused seconds of blank panes on every (re)connect.
    teardownSession();

    setBusy(true);
    setStatus(tr("Connecting to %1:%2").arg(m_host).arg(m_port));

    m_session = new KIMAP::Session(m_host, quint16(m_port), this);
    // No SessionUiProxy is installed on purpose: KIMAP then rejects invalid
    // TLS certificates instead of asking. Surface that case explicitly.
    connect(m_session, &KIMAP::Session::connectionFailed, this, [this] {
        setBusy(false);
        teardownSession();
        setStatus(tr("Connection failed"));
        Q_EMIT errorOccurred(
            tr("Could not establish a secure connection to %1:%2 — wrong host/port, "
               "or the server's TLS certificate was rejected.")
                .arg(m_host)
                .arg(m_port));
    });
    connect(m_session, &KIMAP::Session::connectionLost, this, [this] {
        setBusy(false);
        m_pendingFolder = m_selectedFolder; // restore it after reconnect
        teardownSession();
        setStatus(tr("Reconnecting"));
        QTimer::singleShot(2000, this, [this] {
            if (!m_connected && hasAccount())
                connectAccount();
        });
    });

    auto *login = new KIMAP::LoginJob(m_session);
    configureLogin(login);

    connect(login, &KJob::result, this, [this](KJob *job) {
        if (job->error()) {
            setBusy(false);
            setStatus(tr("Login failed"));
            Q_EMIT errorOccurred(job->errorString());
            teardownSession();
            return;
        }
        m_connected = true;
        Q_EMIT connectedChanged();
        m_keepAlive.start();
        startSyncSession();
        // No "loading folders" crumb — the busy spinner shows the activity.
        listFolders();
    });
    login->start();
}

/// Hands a fetched body to the writer thread. Writing one costs tens of ms —
/// a ~100 KB blob plus its FTS rows — and bodies stream in 50 to a batch, so
/// doing it inline was tens of stalls in a row on the GUI thread.
void MailClient::queueBodyWrite(MailStore::BodyWrite &&write)
{
    {
        QMutexLocker lock(&m_bodyWriteMutex);
        m_bodyWriteQueue.append(std::move(write));
    }
    if (!m_bodyWriterThread) {
        m_bodyWriterStop.storeRelaxed(0);
        m_bodyWriterThread = QThread::create([this] { runBodyWriter(); });
        m_bodyWriterThread->setPriority(QThread::LowPriority);
        m_bodyWriterThread->start();
    }
    m_bodyWriteWake.wakeOne();
}

void MailClient::runBodyWriter()
{
    QSqlDatabase db = MailStore::openWorkerConnection(QStringLiteral("mailstore-bodies"));
    while (!m_bodyWriterStop.loadRelaxed()) {
        QList<MailStore::BodyWrite> batch;
        {
            QMutexLocker lock(&m_bodyWriteMutex);
            if (m_bodyWriteQueue.isEmpty()) {
                // 500 ms so a stop request is noticed promptly even when idle.
                m_bodyWriteWake.wait(&m_bodyWriteMutex, 500);
                if (m_bodyWriteQueue.isEmpty())
                    continue;
            }
            // Whatever has piled up goes in one transaction, capped so a burst
            // never builds an unbounded write.
            const int take = qMin(m_bodyWriteQueue.size(), 25);
            batch = m_bodyWriteQueue.mid(0, take);
            m_bodyWriteQueue.remove(0, take);
        }
        // Lift the attachments out here rather than at queue time: hashing,
        // compressing and writing a payload file is exactly the kind of work
        // the GUI thread must never do. Parsing a private copy also keeps the
        // caller's message object (which may be on screen) untouched.
        for (MailStore::BodyWrite &w : batch) {
            KMime::Message copy;
            copy.setContent(KMime::CRLFtoLF(w.raw));
            copy.parse();
            w.parts = stripAttachments(&copy);
            if (!w.parts.isEmpty()) {
                copy.assemble();
                w.raw = copy.encodedContent();
            }
        }
        MailStore::writeBodiesOn(db, batch);
        // A body indexed while the folded index is being built lands in the old
        // table, and the copy may already be past that row — so queue it for
        // the background re-indexer, which runs after the swap and heals it.
        if (m_indexRebuildActive.loadRelaxed())
            MailStore::queueForReindex(db, batch);
    }
    // Flush whatever is left so a quit does not lose fetched bodies.
    QList<MailStore::BodyWrite> rest;
    {
        QMutexLocker lock(&m_bodyWriteMutex);
        rest.swap(m_bodyWriteQueue);
    }
    MailStore::writeBodiesOn(db, rest);
    db.close();
    QSqlDatabase::removeDatabase(QStringLiteral("mailstore-bodies"));
}

/// Stops the writer thread and waits for it to flush. It restarts by itself on
/// the next queueBodyWrite(), so callers only need this to reach a state where
/// nothing else holds a write lock on the cache.
void MailClient::stopBodyWriter()
{
    if (!m_bodyWriterThread)
        return;
    m_bodyWriterStop.storeRelaxed(1);
    m_bodyWriteWake.wakeAll();
    m_bodyWriterThread->wait();
    delete m_bodyWriterThread;
    m_bodyWriterThread = nullptr;
}

MailClient::~MailClient()
{
    // Shutdown joins five worker threads. If quitting ever hangs, these say
    // which join it is sitting in rather than leaving a silent process behind.
    abandonLocalSearch(); // a search worker must not outlive the model it fills
    qCDebug(logTrace, "shutdown: stopping DKIM verifier");
    if (m_dkimThread) {
        m_dkimThread->quit();
        // A verification in flight may be inside a DNS wait; that wait is
        // capped at 10s, so this join is bounded.
        m_dkimThread->wait();
    }
    qCDebug(logTrace, "shutdown: stopping body writer");
    stopBodyWriter();
    qCDebug(logTrace, "shutdown: stopping purge");
    stopAllMailPurge();
    qCDebug(logTrace, "shutdown: stopping folder maintenance");
    stopFolderOps();
    qCDebug(logTrace, "shutdown: stopping attachment migration");
    stopAttachmentMigration();
    // Cancelled between slices, never mid-slice: the cursor is committed with
    // each one, so the next run picks up where this leaves off.
    qCDebug(logTrace, "shutdown: stopping index rebuild");
    stopIndexRebuild();
    qCDebug(logTrace, "shutdown: joining reindex");
    if (m_reindexThread)
        m_reindexThread->wait();
    // The importer checks the stop flag per message, so this join is short; a
    // partial import stays browsable and a re-import gets its own account.
    qCDebug(logTrace, "shutdown: stopping import");
    m_importStop.storeRelaxed(1);
    if (m_importThread)
        m_importThread->wait();
    // A vacuum cannot be interrupted; joining is the only safe option, and it
    // is why the UI warns before starting one.
    qCDebug(logTrace, "shutdown: joining vacuum");
    if (m_vacuumThread)
        m_vacuumThread->wait();
    qCDebug(logTrace, "shutdown: workers joined");
}

namespace
{
/// Below this, rebuilding the file costs minutes to hand back nothing anyone
/// would notice.
constexpr qint64 kReclaimWorthwhile = 16 * 1024 * 1024;
}

bool MailClient::reclaimWorthwhile()
{
    return m_store.reclaimableBytes() >= kReclaimWorthwhile;
}

QString MailClient::cacheSizeText()
{
    // The cache is two stores now: the database and the attachment files.
    // Reporting only the first would look like mail had gone missing.
    const qint64 db = m_store.databaseBytes();
    const qint64 files = m_store.attachmentBytes();
    const qint64 free = m_store.reclaimableBytes();
    const QLocale loc;
    const QString total = loc.formattedDataSize(db + files);
    if (free < kReclaimWorthwhile)
        return tr("%1 (%2 attachments)").arg(total, loc.formattedDataSize(files));
    return tr("%1 (%2 attachments, %3 reclaimable)")
        .arg(total, loc.formattedDataSize(files), loc.formattedDataSize(free));
}

void MailClient::reclaimDiskSpace()
{
    if (m_reclaiming)
        return;
    m_reclaiming = true;
    Q_EMIT reclaimingChanged();
    // Pause every background writer: VACUUM takes an exclusive lock, and a
    // backfill batch landing mid-rebuild would just block on it.
    m_syncPaused = true;
    m_backfillTimer.stop();
    m_reindexTimer.stop();
    // Ask the writers to stop, but do NOT join them here. A worker only checks
    // its cancel flag between batches, and a batch mid-statement on a large
    // cache can run for many seconds — joining on the GUI thread blocked the
    // event loop for exactly that long, so the desktop marked the window
    // unresponsive and offered to kill it, all before the "Reclaiming disk
    // space" dialog had had a chance to paint.
    m_purgeCancel.storeRelaxed(1);
    m_migrateCancel.storeRelaxed(1);
    if (m_bodyWriterThread) {
        // The body writer was the one background writer left running through a
        // rebuild: its batches would have sat on busy_timeout for 15 s a piece
        // against the exclusive lock, and any that timed out would have lost a
        // fetched body. Stopping it flushes the queue first.
        m_bodyWriterStop.storeRelaxed(1);
        m_bodyWriteWake.wakeAll();
    }
    setStatus(tr("Reclaiming disk space — this can take several minutes"));
    startVacuumWhenWritersIdle();
}

/// True once every background writer has stopped. The purge and migration
/// threads clear their own pointer from a queued finished handler, so a null
/// pointer means stopped *and* reaped; the body writer is joined here, which
/// costs nothing once it has finished.
bool MailClient::writersIdle() const
{
    return !m_purgeThread && !m_migrateThread
        && (!m_bodyWriterThread || m_bodyWriterThread->isFinished());
}

/// VACUUM takes an exclusive lock, so every other writer must have finished —
/// not merely been asked to stop — before it starts. Waiting for that by
/// polling keeps the event loop running, which is what lets the modal dialog
/// appear and the window keep answering the compositor.
void MailClient::startVacuumWhenWritersIdle()
{
    if (!writersIdle()) {
        QTimer::singleShot(100, this, [this] { startVacuumWhenWritersIdle(); });
        return;
    }
    stopBodyWriter(); // already finished: joins and deletes immediately

    const qint64 before = m_store.databaseBytes();
    m_vacuumThread = QThread::create([this, before] {
        QString error;
        const bool ok = MailStore::vacuum(&error);
        // Back to the GUI thread before touching any state it owns.
        QMetaObject::invokeMethod(this, [this, ok, error, before] {
            m_reclaiming = false;
            Q_EMIT reclaimingChanged();
            m_syncPaused = false;
            m_reindexTimer.start();
            scheduleBackfill(2000);
            if (!m_allMailFolder.isEmpty())
                startAllMailPurge();
            startAttachmentMigration();
            if (!ok) {
                Q_EMIT errorOccurred(tr("Reclaiming disk space failed: %1").arg(error));
                return;
            }
            const qint64 freed = before - m_store.databaseBytes();
            const QLocale loc;
            setStatus(freed > 0
                          ? tr("Reclaimed %1").arg(loc.formattedDataSize(freed))
                          : tr("Nothing to reclaim"));
        }, Qt::QueuedConnection);
    });
    connect(m_vacuumThread, &QThread::finished, this, [this] {
        m_vacuumThread->deleteLater();
        m_vacuumThread = nullptr;
    });
    m_vacuumThread->start();
}

/// Walks the existing cache and moves attachment payloads out of the message
/// BLOBs into the content-addressed file store. Runs on a worker thread with
/// its own connection: every message has to be parsed and every payload
/// hashed and compressed, which is far past anything the GUI thread may do.
/// Progress is persisted, so quitting mid-way simply resumes next launch.
void MailClient::startAttachmentMigration()
{
    if (m_migrateThread || m_reclaiming || !m_store.attachmentMigrationPending())
        return;
    m_migrateCancel.storeRelaxed(0);
    m_migrateThread = QThread::create([this] {
        QSqlDatabase db =
            MailStore::openWorkerConnection(QStringLiteral("mailstore-migrate"));
        if (!db.isOpen())
            return;
        qint64 cursor = 0;
        {
            QSqlQuery q(db);
            q.prepare(QStringLiteral(
                "SELECT value FROM meta_values WHERE key = 'attach_migrate_cursor'"));
            if (q.exec() && q.next())
                cursor = q.value(0).toLongLong();
        }
        qint64 saved = 0;
        int done = 0;
        // MIME work lives here, behind a callback, so MailStore stays free of
        // any KMime dependency.
        const auto split = [](const QByteArray &raw, QList<MailStore::PartRef> *parts) {
            KMime::Message msg;
            msg.setContent(KMime::CRLFtoLF(raw));
            msg.parse();
            *parts = stripAttachments(&msg);
            if (parts->isEmpty())
                return raw;
            msg.assemble();
            const QByteArray stub = msg.encodedContent();
            // The original bytes are about to be overwritten, so prove the
            // stub plus the stored payloads reproduces them first. Anything
            // that fails is simply left inline — a message that stays big is
            // an inconvenience, one that loses its attachment is a bug.
            QString reason;
            if (!verifyRoundTrip(stub, *parts, &reason)) {
                qWarning() << "mailo: attachment migration skipped a message —" << reason;
                parts->clear();
                return raw;
            }
            return stub;
        };
        while (!m_migrateCancel.loadRelaxed()) {
            const int n = MailStore::migrateAttachmentsChunk(db, cursor, 50, saved, split);
            if (n == 0) {
                MailStore::finishAttachmentMigration(db);
                break;
            }
            done += n;
            const qint64 savedSoFar = saved;
            const int doneSoFar = done;
            if (done % 2000 < 50) {
                QMetaObject::invokeMethod(this, [this, doneSoFar, savedSoFar] {
                    const QLocale loc;
                    setStatus(tr("Compacting attachments — %1 messages, %2 saved")
                                  .arg(doneSoFar)
                                  .arg(loc.formattedDataSize(savedSoFar)));
                }, Qt::QueuedConnection);
            }
            // Yield the write lock so the UI's own writes never queue behind
            // a migration chunk.
            QThread::msleep(25);
        }
        const bool finished = !m_migrateCancel.loadRelaxed();
        const qint64 savedTotal = saved;
        db.close();
        QSqlDatabase::removeDatabase(QStringLiteral("mailstore-migrate"));
        QMetaObject::invokeMethod(this, [this, finished, savedTotal] {
            if (!finished)
                return;
            m_store.sweepOrphanAttachments();
            // Nothing moved means there was nothing to move — an empty or
            // already-migrated cache. Announcing "0 bytes freed" on a first
            // run reported housekeeping the user never asked for and could
            // not act on.
            if (savedTotal <= 0)
                return;
            const QLocale loc;
            // The space is only handed back to the filesystem by a vacuum —
            // deleting from a SQLite table just marks pages reusable.
            setStatus(tr("Attachments compacted — %1 freed inside the cache; "
                         "use Reclaim disk space in Settings")
                          .arg(loc.formattedDataSize(savedTotal)));
        }, Qt::QueuedConnection);
    });
    connect(m_migrateThread, &QThread::finished, this, [this] {
        m_migrateThread->deleteLater();
        m_migrateThread = nullptr;
    });
    m_migrateThread->setPriority(QThread::LowestPriority);
    m_migrateThread->start();
}

void MailClient::stopAttachmentMigration()
{
    if (!m_migrateThread)
        return;
    m_migrateCancel.storeRelaxed(1);
    m_migrateThread->wait();
}

void MailClient::stopIndexRebuild()
{
    if (!m_indexThread)
        return;
    m_indexCancel.storeRelaxed(1);
    m_indexThread->wait();
}

/// Name fallback for servers that do not advertise RFC 6154 \All. Gmail always
/// does, so this only catches an odd proxy — deliberately narrow, because a
/// false positive would hide a folder the user actually wants.
bool MailClient::isAllMailName(const QString &mailBox)
{
    return mailBox.compare(QLatin1String("[Gmail]/All Mail"), Qt::CaseInsensitive) == 0
        || mailBox.compare(QLatin1String("[Google Mail]/All Mail"), Qt::CaseInsensitive) == 0;
}

/// Deletes the excluded archive's cached rows on a worker thread. Releasing the
/// blob pages of a ~100 KB body is far too slow to do on the GUI thread, and at
/// 200k messages there is no chunk size that both finishes this decade and
/// stays inside the 20 ms budget — so it runs on its own connection instead,
/// yielding the write lock between small chunks.
void MailClient::startAllMailPurge()
{
    if (m_allMailFolder.isEmpty() || m_purgeThread || m_reclaiming)
        return;
    m_purgeCancel.storeRelaxed(0);
    const QString key = m_store.scopedKey(m_allMailFolder);
    m_purgeThread = QThread::create([this, key] {
        MailStore::purgeFolder(key, m_purgeCancel, [this](int total) {
            // Status text belongs to the GUI thread.
            QMetaObject::invokeMethod(this, [this, total] {
                m_purgedRows = total;
                if (total % 5000 < 100)
                    setStatus(tr("Clearing archive cache — %1 messages").arg(total));
            }, Qt::QueuedConnection);
        });
        QMetaObject::invokeMethod(this, [this] {
            if (!m_purgeCancel.loadRelaxed() && m_purgedRows > 0)
                setStatus(tr("Archive cache cleared — reclaim disk space in Settings"));
            m_purgedRows = 0;
        }, Qt::QueuedConnection);
    });
    connect(m_purgeThread, &QThread::finished, this, [this] {
        m_purgeThread->deleteLater();
        m_purgeThread = nullptr;
    });
    m_purgeThread->start();
    // Below the UI's own work: this must never make a folder switch wait.
    m_purgeThread->setPriority(QThread::LowestPriority);
}

/// Stops the purge worker and waits for it, so no connection outlives the
/// object that owns it (and so a vacuum never starts while it still writes).
void MailClient::stopAllMailPurge()
{
    if (!m_purgeThread)
        return;
    m_purgeCancel.storeRelaxed(1);
    m_purgeThread->wait();
}

void MailClient::listFolders()
{
    auto *list = new KIMAP::ListJob(m_session);
    list->setOption(KIMAP::ListJob::IncludeUnsubscribed);

    auto folders = std::make_shared<QList<FolderModel::Folder>>();
    connect(list, &KIMAP::ListJob::mailBoxesReceived, this,
            [this, folders](const QList<KIMAP::MailBoxDescriptor> &descriptors,
                            const QList<QList<QByteArray>> &flagList) {
                for (int i = 0; i < descriptors.size(); ++i) {
                    const auto &d = descriptors.at(i);
                    FolderModel::Folder f;
                    f.mailBox = d.name;
                    const QChar sep = d.separator;
                    // The delimiter every folder path is built with; needed to
                    // form destination paths when a folder is reparented.
                    if (!sep.isNull())
                        m_folderSeparator = sep;
                    // Level and display name are filled in once the whole list
                    // is in, by pathRows() — they depend on which ancestors are
                    // really mailboxes, which a single descriptor cannot say.
                    bool isAllMail = false;
                    if (i < flagList.size()) {
                        for (const QByteArray &flag : flagList.at(i)) {
                            if (flag.compare("\\Noselect", Qt::CaseInsensitive) == 0)
                                f.selectable = false;
                            // RFC 6154 special-use: the server tells us where
                            // sent mail belongs.
                            if (flag.compare("\\Sent", Qt::CaseInsensitive) == 0)
                                m_sentFolder = d.name;
                            // Same mechanism for drafts. Servers that support
                            // the LIST special-use extension tell us outright;
                            // the name guess below is only for those that
                            // do not.
                            if (flag.compare("\\Drafts", Qt::CaseInsensitive) == 0)
                                m_draftsFolder = d.name;
                            // ...and which mailbox is the "everything" archive.
                            if (flag.compare("\\All", Qt::CaseInsensitive) == 0)
                                isAllMail = true;
                        }
                    }
                    // Gmail's All Mail duplicates every message that is already
                    // in INBOX and in each label, so caching it stores the same
                    // bytes two to four times over. Skipping it here keeps it
                    // out of the folder list, out of storeFolders() and out of
                    // the backfill queue in one go.
                    if (isAllMail || isAllMailName(d.name)) {
                        m_allMailFolder = d.name;
                        continue;
                    }
                    folders->append(f);
                }
            });

    connect(list, &KJob::result, this, [this, folders](KJob *job) {
        setBusy(false);
        if (job->error()) {
            setStatus(tr("Listing folders failed"));
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        // The same order the rest of the app builds trees in: inbox first,
        // then depth first with every parent directly above its children.
        //
        // The old rule compared whole paths as strings, which put "INBOX-old"
        // between "INBOX" and "INBOX/Work" ('-' sorts below '/') — and the
        // sidebar reads the tree off row order, so the child was orphaned from
        // its parent and the parent lost its expander. It also matched the
        // inbox with startsWith(), so "INBOX-old" was hoisted to the top as if
        // it were the inbox itself.
        const QChar sep = folderSeparator();
        std::sort(folders->begin(), folders->end(),
                  [sep](const FolderModel::Folder &a, const FolderModel::Folder &b) {
                      return mailBoxPathLess(a.mailBox, b.mailBox, sep);
                  });
        // Indent and display name, once the whole set is known — see pathRows().
        {
            QStringList paths;
            paths.reserve(folders->size());
            for (const auto &f : std::as_const(*folders))
                paths.append(f.mailBox);
            const QList<PathRow> rows = pathRows(paths, sep);
            for (int i = 0; i < folders->size(); ++i) {
                (*folders)[i].level = rows.at(i).level;
                (*folders)[i].displayName = rows.at(i).name;
            }
        }
        // Fallback when the server doesn't advertise \Sent special-use.
        if (m_sentFolder.isEmpty()) {
            static const QStringList sentNames = {
                QStringLiteral("sent"), QStringLiteral("sent messages"),
                QStringLiteral("sent items"), QStringLiteral("sent mail")};
            for (const auto &f : std::as_const(*folders)) {
                if (sentNames.contains(f.displayName.toLower())) {
                    m_sentFolder = f.mailBox;
                    break;
                }
            }
        }
        // Fallback when the server doesn't advertise \\Drafts special-use.
        if (m_draftsFolder.isEmpty()) {
            static const QStringList draftNames = {
                QStringLiteral("drafts"), QStringLiteral("draft"),
                QStringLiteral("draft messages")};
            for (const auto &f : std::as_const(*folders)) {
                if (draftNames.contains(f.displayName.toLower())) {
                    m_draftsFolder = f.mailBox;
                    break;
                }
            }
        }
        Q_EMIT draftsFolderChanged();
        m_folderModel.setFolders(*folders);
        // A fresh folder list restarts the all-folders background sync pass.
        m_folderBackfillPassDone = false;
        m_folderBackfillQueue.clear();
        QStringList names;
        names.reserve(folders->size());
        for (const auto &f : std::as_const(*folders))
            names.append(f.mailBox);
        m_store.storeFolders(accountKey(), names);
        // Seed the compose autocompletion from cached Sent bodies (once per account).
        m_store.harvestSentRecipients(m_sentFolder);
        scheduleUnreadRecount(); // the tree just changed; so did which pills exist
        sweepOldSpam(); // needs junkFolderName(), which the listing just settled
        setStatus(countNoun(folders->size(), "folder", "folders"));
        // Drop whatever an earlier version cached for the now-excluded archive.
        // Detection repeats on every connect, so an interrupted purge simply
        // resumes next session instead of leaving rows stranded.
        if (!m_allMailFolder.isEmpty())
            startAllMailPurge();
        QString target = m_pendingFolder;
        qCDebug(logTrace, "listFolders done: pending=%s selected=%s folders=%d",
                qUtf8Printable(m_pendingFolder), qUtf8Printable(m_selectedFolder),
                int(names.size()));
        m_pendingFolder.clear();
        if (target.isEmpty()) {
            // Whatever is already open wins. Listing folders is a background
            // step that also runs on every reconnect, and forcing INBOX here
            // yanked the user out of the folder they had just opened — the
            // account switch opens the clicked folder from cache first, and
            // this landed a moment later and overrode it.
            target = (!m_selectedFolder.isEmpty() && names.contains(m_selectedFolder))
                ? m_selectedFolder
                : QStringLiteral("INBOX");
        }
        if (!m_allMailFolder.isEmpty() && target == m_allMailFolder)
            target = QStringLiteral("INBOX"); // it is no longer in the list
        openFolder(target);
    });
    list->start();
}

bool MailClient::isJunkFolder(const QString &mailBox) const
{
    // This drives the hostile-content defaults (plain text, no remote content),
    // so a miss here silently downgrades protection. Matching on names is a
    // heuristic — the robust answer is the IMAP SPECIAL-USE "\Junk" attribute,
    // which is not plumbed through the folder model yet.
    static const QStringList junkNames = {
        QStringLiteral("spam"),         QStringLiteral("junk"),
        QStringLiteral("junk e-mail"),  QStringLiteral("junk email"),
        QStringLiteral("junk mail"),    QStringLiteral("bulk mail"),
        QStringLiteral("bulk"),         QStringLiteral("quarantine"),
        // Localized names used by the major providers' web UIs
        QStringLiteral("correo no deseado"), QStringLiteral("no deseado"),
        QStringLiteral("courrier indésirable"), QStringLiteral("indésirables"),
        QStringLiteral("pourriel"),     QStringLiteral("unerwünscht"),
        QStringLiteral("posta indesiderata"), QStringLiteral("indesiderata"),
        QStringLiteral("lixo eletrônico"), QStringLiteral("lixo eletronico"),
        QStringLiteral("ongewenst"),    QStringLiteral("ongewenste e-mail"),
        QStringLiteral("uønsket e-post"), QStringLiteral("skräppost"),
        QStringLiteral("roskaposti"),   QStringLiteral("uønsket post"),
        QStringLiteral("wiadomości-śmieci"), QStringLiteral("niechciane"),
        QStringLiteral("nevyžádaná pošta"), QStringLiteral("nevyžiadaná pošta"),
        QStringLiteral("levélszemét"),  QStringLiteral("спам"),
        QStringLiteral("нежелательная почта"), QStringLiteral("垃圾邮件"),
        QStringLiteral("垃圾郵件"),      QStringLiteral("迷惑メール"),
        QStringLiteral("스팸")};
    const QChar sep = mailBox.contains(QLatin1Char('/')) ? QLatin1Char('/')
                                                         : QLatin1Char('.');
    const QString leaf = mailBox.section(sep, -1).toLower();
    if (junkNames.contains(leaf))
        return true;
    // Providers decorate the leaf ("Spam (2)", "Junk-E-Mail"); a substring test
    // on these two roots costs nothing and catches the decorated variants.
    return leaf.contains(QLatin1String("spam")) || leaf.contains(QLatin1String("junk"));
}

QString MailClient::trashFolderName() const
{
    static const QStringList trashNames = {
        QStringLiteral("trash"), QStringLiteral("deleted items"),
        QStringLiteral("deleted messages"), QStringLiteral("deleted"),
        QStringLiteral("bin")};
    const QStringList boxes = m_folderModel.allMailBoxes();
    for (const QString &mailBox : boxes) {
        const QChar sep = mailBox.contains(QLatin1Char('/')) ? QLatin1Char('/')
                                                             : QLatin1Char('.');
        if (trashNames.contains(mailBox.section(sep, -1).toLower()))
            return mailBox;
    }
    return {};
}

QString MailClient::junkFolderName() const
{
    const QStringList boxes = m_folderModel.allMailBoxes();
    for (const QString &mailBox : boxes) {
        if (isJunkFolder(mailBox))
            return mailBox;
    }
    return {};
}

bool MailClient::isTrashFolder() const
{
    return !m_selectedFolder.isEmpty() && m_selectedFolder == trashFolderName();
}

/// Removes the uids from the visible list and the on-disk cache.
void MailClient::purgeDeleted(const QList<qint64> &uids)
{
    m_messageModel.removeByUids(uids);
    m_store.removeMessages(m_selectedFolder, uids);
    invalidateMissingBodies();
}

void MailClient::deleteMessages(const QVariantList &rows)
{
    if (!m_connected || !m_session) {
        Q_EMIT errorOccurred(tr("Not connected — cannot delete messages."));
        return;
    }
    KIMAP::ImapSet set;
    auto uids = std::make_shared<QList<qint64>>();
    for (const QVariant &v : rows) {
        const qint64 uid = m_messageModel.uidAt(v.toInt());
        if (uid > 0) {
            set.add(uid);
            uids->append(uid);
        }
    }
    if (uids->isEmpty())
        return;

    // Spam skips the trash when the setting says so — filing junk under
    // "deleted" only means clearing it out twice.
    const bool permanent = deleteIsPermanent();
    const QString trash = trashFolderName();
    if (!permanent && trash.isEmpty()) {
        Q_EMIT errorOccurred(tr("No trash folder found on the server."));
        return;
    }

    setBusy(true);
    // No in-progress crumb — the busy spinner covers it; only the result shows.

    // The browsing SELECT is read-only (EXAMINE); STORE/MOVE need read-write.
    auto *select = new KIMAP::SelectJob(m_session);
    select->setMailBox(m_selectedFolder);
    connect(select, &KJob::result, this, [this, set, uids, permanent, trash](KJob *job) {
        if (job->error()) {
            setBusy(false);
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        m_folderReadWrite = true;
        if (permanent) {
            auto *store = new KIMAP::StoreJob(m_session);
            store->setUidBased(true);
            store->setSequenceSet(set);
            store->setMode(KIMAP::StoreJob::AppendFlags);
            store->setFlags({QByteArrayLiteral("\\Deleted")});
            connect(store, &KJob::result, this, [this, uids](KJob *job) {
                if (job->error()) {
                    setBusy(false);
                    Q_EMIT errorOccurred(job->errorString());
                    return;
                }
                auto *expunge = new KIMAP::ExpungeJob(m_session);
                connect(expunge, &KJob::result, this, [this, uids](KJob *job) {
                    setBusy(false);
                    if (job->error()) {
                        Q_EMIT errorOccurred(job->errorString());
                        return;
                    }
                    purgeDeleted(*uids);
                    setStatus(tr("%1 deleted permanently").arg(uids->size()));
                });
                expunge->start();
            });
            store->start();
        } else {
            auto *move = new KIMAP::MoveJob(m_session);
            move->setUidBased(true);
            move->setSequenceSet(set);
            move->setMailBox(trash);
            connect(move, &KJob::result, this, [this, uids](KJob *job) {
                setBusy(false);
                if (job->error()) {
                    Q_EMIT errorOccurred(job->errorString());
                    return;
                }
                purgeDeleted(*uids);
                setStatus(tr("%1 moved to trash").arg(uids->size()));
            });
            move->start();
        }
    });
    select->start();
}

void MailClient::markAsNotSpam(const QVariantList &rows)
{
    int cleared = 0;
    for (const QVariant &v : rows) {
        const int row = v.toInt();
        const qint64 uid = m_messageModel.uidAt(row);
        if (uid < 0)
            continue;
        // Allowlist the sender, not just this message. A false positive means
        // the rule was wrong about the person; correcting only the one message
        // would let the next one from them be marked all over again.
        const QString sender = SpamHeuristics::addressOf(m_messageModel.fromAt(row));
        if (!sender.isEmpty())
            m_store.addRecipient(sender);
        m_messageModel.clearSpam(uid);
        m_store.setSpamVerdict(m_selectedFolder, uid, 0, 3, QString());
        ++cleared;
    }
    if (cleared > 0)
        setStatus(tr("Not spam — sender added to your known contacts"));
}

void MailClient::markAsJunk(const QVariantList &rows)
{
    if (!m_connected || !m_session) {
        Q_EMIT errorOccurred(tr("Not connected — cannot move messages to spam."));
        return;
    }
    const QString junk = junkFolderName();
    if (junk.isEmpty()) {
        Q_EMIT errorOccurred(tr("No spam folder found on the server."));
        return;
    }
    if (m_selectedFolder == junk)
        return;

    KIMAP::ImapSet set;
    auto uids = std::make_shared<QList<qint64>>();
    for (const QVariant &v : rows) {
        const qint64 uid = m_messageModel.uidAt(v.toInt());
        if (uid > 0) {
            set.add(uid);
            uids->append(uid);
        }
    }
    if (uids->isEmpty())
        return;

    setBusy(true);
    // No in-progress crumb — the busy spinner covers it; only the result shows.

    // The browsing SELECT is read-only (EXAMINE); MOVE needs read-write.
    auto *select = new KIMAP::SelectJob(m_session);
    select->setMailBox(m_selectedFolder);
    connect(select, &KJob::result, this, [this, set, uids, junk](KJob *job) {
        if (job->error()) {
            setBusy(false);
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        m_folderReadWrite = true;
        auto *move = new KIMAP::MoveJob(m_session);
        move->setUidBased(true);
        move->setSequenceSet(set);
        move->setMailBox(junk);
        connect(move, &KJob::result, this, [this, uids](KJob *job) {
            setBusy(false);
            if (job->error()) {
                Q_EMIT errorOccurred(job->errorString());
                return;
            }
            purgeDeleted(*uids);
            setStatus(tr("%1 moved to spam").arg(uids->size()));
        });
        move->start();
    });
    select->start();
}

void MailClient::moveMessagesTo(const QVariantList &rows, const QString &targetFolder)
{
    if (!m_connected || !m_session) {
        Q_EMIT errorOccurred(tr("Not connected — cannot move messages."));
        return;
    }
    if (targetFolder.isEmpty() || targetFolder == m_selectedFolder)
        return;

    KIMAP::ImapSet set;
    auto uids = std::make_shared<QList<qint64>>();
    for (const QVariant &v : rows) {
        const qint64 uid = m_messageModel.uidAt(v.toInt());
        if (uid > 0) {
            set.add(uid);
            uids->append(uid);
        }
    }
    if (uids->isEmpty())
        return;

    setBusy(true);
    // The browsing SELECT is read-only (EXAMINE); MOVE needs read-write.
    auto *select = new KIMAP::SelectJob(m_session);
    select->setMailBox(m_selectedFolder);
    connect(select, &KJob::result, this, [this, set, uids, targetFolder](KJob *job) {
        if (job->error()) {
            setBusy(false);
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        m_folderReadWrite = true;
        auto *move = new KIMAP::MoveJob(m_session);
        move->setUidBased(true);
        move->setSequenceSet(set);
        move->setMailBox(targetFolder);
        connect(move, &KJob::result, this, [this, uids, targetFolder](KJob *job) {
            setBusy(false);
            if (job->error()) {
                Q_EMIT errorOccurred(job->errorString());
                return;
            }
            purgeDeleted(*uids);
            // The destination's cached header list no longer matches what the
            // server holds; its next open re-syncs it anyway, but the missing
            // -bodies estimate is shared and would be stale immediately.
            invalidateMissingBodies();
            setStatus(tr("%1 moved to %2")
                          .arg(uids->size())
                          .arg(folderLeaf(targetFolder)));
        });
        move->start();
    });
    select->start();
}

QChar MailClient::folderSeparator() const
{
    // An archive is built by the importer, which always joins with '/'. Asking
    // it to guess is what let a previously-connected IMAP account's '.' leak in
    // and turn "mail.example.com/Inbox" into a leaf of "com/Inbox".
    if (m_local)
        return QLatin1Char('/');
    if (!m_folderSeparator.isNull())
        return m_folderSeparator;
    // Before the first LIST lands, infer it the way the special-folder
    // lookups do: '/' when any path uses it, '.' otherwise.
    const QStringList boxes = m_folderModel.allMailBoxes();
    for (const QString &box : boxes) {
        if (box.contains(QLatin1Char('/')))
            return QLatin1Char('/');
    }
    return QLatin1Char('.');
}

QString MailClient::folderLeaf(const QString &mailBox) const
{
    return mailBox.section(folderSeparator(), -1);
}

QString MailClient::folderParent(const QString &mailBox) const
{
    const QChar sep = folderSeparator();
    const int cut = mailBox.lastIndexOf(sep);
    return cut < 0 ? QString() : mailBox.left(cut);
}

QString MailClient::folderDisplayParent(const QString &mailBox) const
{
    // Read from the ancestors that really are mailboxes, exactly as pathRows()
    // draws the tree. An imported archive keeps its mail under the server
    // directory it came from ("mail.example.com/Inbox") without that directory
    // being a mailbox, so the string prefix is not the parent on screen — and
    // a drop has to mean what the user sees.
    const QChar sep = folderSeparator(); // the account's real delimiter, not a guess
    const QStringList boxes = m_folderModel.allMailBoxes();
    const QSet<QString> known(boxes.cbegin(), boxes.cend());
    const QStringList parts = mailBox.split(sep);
    QString parent;
    QString prefix;
    for (int i = 0; i + 1 < parts.size(); ++i) {
        prefix += (i == 0 ? QString() : QString(sep)) + parts.at(i);
        if (known.contains(prefix))
            parent = prefix;
    }
    return parent;
}

QString MailClient::folderDisplayLeaf(const QString &mailBox) const
{
    const QString parent = folderDisplayParent(mailBox);
    return parent.isEmpty() ? mailBox : mailBox.mid(parent.size() + 1);
}

QString MailClient::freeChildPath(const QString &parent, const QString &leaf) const
{
    const QChar sep = folderSeparator();
    const QStringList boxes = m_folderModel.allMailBoxes();
    const auto pathOf = [&](const QString &name) {
        return parent.isEmpty() ? name : parent + sep + name;
    };
    QString candidate = pathOf(leaf);
    for (int n = 2; boxes.contains(candidate); ++n)
        candidate = pathOf(QStringLiteral("%1 (%2)").arg(leaf).arg(n));
    return candidate;
}

QStringList MailClient::folderSubtree(const QString &mailBox) const
{
    const QString prefix = mailBox + folderSeparator();
    QStringList out;
    const QStringList boxes = m_folderModel.allMailBoxes();
    for (const QString &box : boxes) {
        if (box == mailBox || box.startsWith(prefix))
            out.append(box);
    }
    // Deepest first: a server may refuse to DELETE a mailbox that still has
    // children, and never the other way round.
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        return a.size() > b.size();
    });
    return out;
}

bool MailClient::folderProtected(const QString &mailBox) const
{
    if (mailBox.isEmpty())
        return true;
    // A local archive is plain storage: no folder has server-side meaning, so
    // even INBOX, Trash or Sent may be renamed, moved or deleted.
    if (m_local)
        return false;
    if (mailBox.compare(QLatin1String("INBOX"), Qt::CaseInsensitive) == 0)
        return true;
    // Drafts belongs here with the others: it is a RFC 6154 special-use
    // mailbox, and a server that requires one recreates it the moment it is
    // renamed away — which looks exactly like the move left a copy behind,
    // because the folder is now in both places.
    return mailBox == trashFolderName() || mailBox == junkFolderName()
        || mailBox == m_sentFolder || mailBox == m_draftsFolder
        || mailBox == m_allMailFolder;
}

bool MailClient::canMoveFolder(const QString &mailBox, const QString &newParent) const
{
    // A local archive edits its cache directly, so being offline is its
    // normal, fully-capable state.
    if ((!m_connected && !m_local) || mailBox.isEmpty() || mailBox == newParent)
        return false;
    if (folderProtected(mailBox))
        return false;
    const QChar sep = folderSeparator();
    if (folderDisplayParent(mailBox) == newParent)
        return false; // already sits there
    if (newParent.startsWith(mailBox + sep))
        return false; // a folder cannot become its own descendant
    const QStringList boxes = m_folderModel.allMailBoxes();
    if (!newParent.isEmpty() && !boxes.contains(newParent))
        return false;
    // Occupied destination: the server would reject the RENAME, and merging
    // two mailboxes is not something a drop should be able to mean.
    const QString leaf = folderDisplayLeaf(mailBox);
    return !boxes.contains(newParent.isEmpty() ? leaf : newParent + sep + leaf);
}

void MailClient::moveFolder(const QString &mailBox, const QString &newParent)
{
    if (!m_local && (!m_connected || !m_session)) {
        Q_EMIT errorOccurred(tr("Not connected — cannot move folders."));
        return;
    }
    if (!canMoveFolder(mailBox, newParent))
        return;
    const QChar sep = folderSeparator();
    const QString leaf = folderDisplayLeaf(mailBox);
    const QString dest = newParent.isEmpty() ? leaf : newParent + sep + leaf;
    renameFolderOnServer(mailBox, dest,
                         newParent.isEmpty()
                             ? tr("%1 moved to the top level").arg(leaf)
                             : tr("%1 moved into %2").arg(leaf, folderLeaf(newParent)));
}

QString MailClient::folderRenameBlockedReason(const QString &mailBox) const
{
    if (mailBox.isEmpty())
        return tr("No folder selected");
    if (m_local)
        return {}; // plain storage: no folder here means anything to a server
    if (!m_connected)
        return tr("Renaming needs a connection");
    // Named after the folder, so the menu says which one it is talking about.
    // RFC 3501 §6.3.5: renaming INBOX moves its messages out and leaves INBOX
    // in place, so it is not a rename at all. RFC 6154 special-use: a server
    // that requires one recreates it under the old name straight away.
    if (mailBox.compare(QLatin1String("INBOX"), Qt::CaseInsensitive) == 0)
        return tr("INBOX cannot be renamed (RFC 3501)");
    if (mailBox == m_sentFolder || mailBox == m_draftsFolder
        || mailBox == trashFolderName() || mailBox == junkFolderName()
        || mailBox == m_allMailFolder) {
        return tr("%1 cannot be renamed (RFC 6154)").arg(folderDisplayLeaf(mailBox));
    }
    return {};
}

void MailClient::renameFolder(const QString &mailBox, const QString &newName)
{
    const QString leaf = newName.trimmed();
    if (leaf.isEmpty() || !folderRenameBlockedReason(mailBox).isEmpty())
        return;
    const QChar sep = folderSeparator();
    if (leaf.contains(sep)) {
        // A separator in the name would reparent the folder, which is what
        // dragging it is for. Renaming only ever changes the last step.
        Q_EMIT errorOccurred(tr("A folder name cannot contain %1.").arg(sep));
        return;
    }
    if (leaf == folderDisplayLeaf(mailBox))
        return; // unchanged
    const QString parent = folderDisplayParent(mailBox);
    const QString dest = parent.isEmpty() ? leaf : parent + sep + leaf;
    if (m_folderModel.allMailBoxes().contains(dest)) {
        Q_EMIT errorOccurred(tr("A folder named %1 is already there.").arg(leaf));
        return;
    }
    if (!m_local && (!m_connected || !m_session)) {
        Q_EMIT errorOccurred(tr("Not connected — cannot rename folders."));
        return;
    }
    renameFolderOnServer(mailBox, dest, tr("Renamed to %1").arg(leaf));
}

bool MailClient::folderDeleteIsPermanent(const QString &mailBox) const
{
    const QString trash = trashFolderName();
    if (trash.isEmpty())
        return true; // nowhere to move it to
    return mailBox == trash || mailBox.startsWith(trash + folderSeparator());
}

void MailClient::deleteFolder(const QString &mailBox)
{
    if (!m_local && (!m_connected || !m_session)) {
        Q_EMIT errorOccurred(tr("Not connected — cannot delete folders."));
        return;
    }
    if (mailBox.isEmpty() || folderProtected(mailBox))
        return;

    const QString leaf = folderLeaf(mailBox);
    if (!folderDeleteIsPermanent(mailBox)) {
        renameFolderOnServer(mailBox, freeChildPath(trashFolderName(), leaf),
                             tr("%1 moved to trash").arg(leaf));
        return;
    }

    if (m_local) {
        // No server holds a copy: dropping the subtree from the folder list
        // and purging its cached mail IS the deletion. The purge itself runs
        // chunked on the worker; the tree and the open folder update now.
        const QStringList doomed = folderSubtree(mailBox);
        QStringList remaining = m_folderModel.allMailBoxes();
        for (const QString &box : doomed)
            remaining.removeAll(box);
        m_store.storeFolders(accountKey(), remaining);
        m_folderModel.setFolders(foldersFromPaths(remaining));
        purgeCachedFolders(doomed);
        if (doomed.contains(m_selectedFolder)) {
            // Even INBOX may be gone here — land on whatever still exists.
            if (remaining.contains(QStringLiteral("INBOX")))
                openFolder(QStringLiteral("INBOX"));
            else if (!remaining.isEmpty())
                openFolder(remaining.first());
            else {
                m_selectedFolder.clear();
                Q_EMIT selectedFolderChanged();
                m_messageModel.clear();
            }
        }
        setStatus(tr("%1 deleted").arg(leaf));
        return;
    }

    // Already in the trash: this really removes it. Subfolders go first —
    // deepest first, one job at a time, since a server may refuse to delete a
    // mailbox that still has children.
    auto remaining = std::make_shared<QStringList>(folderSubtree(mailBox));
    if (remaining->isEmpty())
        return;
    const auto deleted = std::make_shared<QStringList>();

    setBusy(true);
    // Chained rather than fired together: KIMAP runs one job at a time per
    // session anyway, and this way the first failure stops the rest.
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, remaining, deleted, step, leaf] {
        if (remaining->isEmpty()) {
            purgeCachedFolders(*deleted);
            // Whatever was open inside the deleted subtree is gone; land on
            // INBOX rather than on a mailbox the server no longer has.
            if (deleted->contains(m_selectedFolder))
                m_pendingFolder = QStringLiteral("INBOX");
            setStatus(tr("%1 deleted").arg(leaf));
            listFolders(); // clears busy when the new tree arrives
            return;
        }
        const QString box = remaining->takeFirst();
        auto *del = new KIMAP::DeleteJob(m_session);
        del->setMailBox(box);
        connect(del, &KJob::result, this, [this, box, deleted, step](KJob *job) {
            if (job->error()) {
                setBusy(false);
                setStatus(tr("Deleting folder failed"));
                Q_EMIT errorOccurred(job->errorString());
                if (!deleted->isEmpty()) {
                    purgeCachedFolders(*deleted); // whatever did go, goes from the cache too
                    setBusy(true);
                    listFolders();
                }
                return;
            }
            deleted->append(box);
            (*step)();
        });
        del->start();
    };
    (*step)();
}

void MailClient::renameFolderOnServer(const QString &from, const QString &to,
                                      const QString &doneStatus)
{
    setBusy(true);
    if (m_local) {
        // No server to ask — the cache re-key IS the rename, and it also
        // rewrites the stored folder list (see MailStore::renameFolderOn).
        // Unlike the connected path, the tree refresh and the selection
        // follow-up wait for the worker: reopening a folder whose rows are
        // still being re-keyed would show it half-empty.
        renameCachedFolder(from, to, [this, from, to, doneStatus] {
            const QString prefix = from + folderSeparator();
            if (m_selectedFolder == from || m_selectedFolder.startsWith(prefix)) {
                m_selectedFolder = to + m_selectedFolder.mid(from.size());
                Q_EMIT selectedFolderChanged();
            }
            // The re-key rewrites each path in place but leaves the stored row
            // order alone, and the tree is drawn from that order — a folder
            // moved under a parent further down the list would render as an
            // indented orphan. A connected account gets a freshly sorted list
            // back from the server's LIST; here nobody else will sort it.
            QStringList boxes = m_store.cachedFolders(accountKey());
            sortMailBoxPaths(&boxes, folderSeparator());
            m_store.storeFolders(accountKey(), boxes);
            m_folderModel.setFolders(foldersFromPaths(boxes));
            setBusy(false);
            if (!m_selectedFolder.isEmpty())
                openFolder(m_selectedFolder);
            setStatus(doneStatus);
        });
        return;
    }
    auto *rename = new KIMAP::RenameJob(m_session);
    rename->setSourceMailBox(from);
    rename->setDestinationMailBox(to);
    connect(rename, &KJob::result, this, [this, from, to, doneStatus](KJob *job) {
        if (job->error()) {
            setBusy(false);
            setStatus(tr("Moving folder failed"));
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        // The mail did not change, only the path it is filed under — re-key
        // the cache instead of making the user sync the folder again. It runs
        // in the background: a header sync that lands on the new path while it
        // is still going may lose a row or two to the re-key, which the next
        // refresh puts back. Blocking the UI for a subtree rewrite would be a
        // far worse trade.
        renameCachedFolder(from, to);
        // RENAME takes the subtree with it, so the open folder may be sitting
        // at a path that no longer exists. Follow it to the new one.
        const QString prefix = from + folderSeparator();
        if (m_selectedFolder == from || m_selectedFolder.startsWith(prefix))
            m_pendingFolder = to + m_selectedFolder.mid(from.size());
        setStatus(doneStatus);
        listFolders(); // clears busy when the new tree arrives
    });
    rename->start();
}

void MailClient::renameCachedFolder(const QString &from, const QString &to,
                                    std::function<void()> done)
{
    stopFolderOps(); // one maintenance worker at a time
    m_folderOpCancel.storeRelaxed(0);
    const QString account = accountKey();
    const QChar sep = folderSeparator();
    m_folderOpThread = QThread::create([account, from, to, sep] {
        QSqlDatabase db = MailStore::openWorkerConnection(QStringLiteral("mailstore-folderop"));
        if (db.isOpen()) {
            MailStore::renameFolderOn(db, account, from, to, sep);
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("mailstore-folderop"));
    });
    connect(m_folderOpThread, &QThread::finished, this,
            [this, done = std::move(done)] {
        m_folderOpThread->deleteLater();
        m_folderOpThread = nullptr;
        // Header counts and the missing-body estimate were keyed by the old path.
        invalidateMissingBodies();
        if (done)
            done();
    });
    // Below the UI's own work: a folder switch must never wait for this.
    m_folderOpThread->start(QThread::LowestPriority);
}

void MailClient::pollOtherAccounts()
{
    if (m_accountPollBusy)
        return; // the previous round is still going; skip this tick
    QSettings s = appSettings();
    const QList<QVariantMap> accounts = readAccountArray(s);
    auto queue = std::make_shared<QList<QVariantMap>>();
    for (int i = 0; i < accounts.size(); ++i) {
        if (i == m_currentAccount)
            continue; // it has a live session and IDLE of its own
        const QVariantMap &a = accounts.at(i);
        if (a.value(QStringLiteral("local"), false).toBool())
            continue; // an archive has no server to ask
        if (a.value(QStringLiteral("host")).toString().isEmpty()
            || a.value(QStringLiteral("user")).toString().isEmpty())
            continue;
        queue->append(a);
    }
    if (queue->isEmpty())
        return;

    m_accountPollBusy = true;
    // One at a time. Firing every account at once would open as many
    // simultaneous logins as there are accounts, which is exactly what servers
    // that count connections per IP object to.
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, queue, step] {
        if (queue->isEmpty()) {
            m_accountPollBusy = false;
            scheduleUnreadRecount(); // fold the new counts into the sidebar
            return;
        }
        const QVariantMap account = queue->takeFirst();
        pollAccount(account, [step] { (*step)(); });
    };
    (*step)();
}

void MailClient::pollAccount(const QVariantMap &account, const std::function<void()> &done)
{
    const QString host = account.value(QStringLiteral("host")).toString();
    const QString user = account.value(QStringLiteral("user")).toString();
    const int port = account.value(QStringLiteral("port"), 993).toInt();
    const int security = account.value(QStringLiteral("security"), int(SslTls)).toInt();
    const int authType = account.value(QStringLiteral("authType"), 0).toInt();
    QString key = account.value(QStringLiteral("cacheKey")).toString();
    if (key.isEmpty())
        key = user + QLatin1Char('@') + host;

    // Opens the connection once the secret is in hand. Shared by both auth
    // paths so the session setup is written once.
    auto connectWith = [this, host, port, security, user, authType, key, done]
                       (const QString &secret) {
        if (secret.isEmpty()) {
            done();
            return;
        }
        auto *session = new KIMAP::Session(host, quint16(port), this);
        auto *login = new KIMAP::LoginJob(session);
        login->setUserName(user);
        if (authType != 0) {
            login->setAuthenticationMode(KIMAP::LoginJob::XOAuth2);
        } else {
            login->setAuthenticationMode(KIMAP::LoginJob::Plain);
        }
        login->setPassword(secret);
        switch (security) {
        case StartTls:
            login->setEncryptionMode(KIMAP::LoginJob::STARTTLS);
            break;
        case None:
            login->setEncryptionMode(KIMAP::LoginJob::Unencrypted);
            break;
        default:
            login->setEncryptionMode(KIMAP::LoginJob::SSLorTLS);
            break;
        }
        connect(login, &KJob::result, this, [this, session, key, done](KJob *job) {
            if (job->error()) {
                // Unreachable or refused: not worth a message. This runs on a
                // timer in the background and the account the user is actually
                // reading is unaffected.
                session->close();
                session->deleteLater();
                done();
                return;
            }
            statusFoldersOn(session, key, [session, done] {
                auto *logout = new KIMAP::LogoutJob(session);
                connect(logout, &KJob::result, session, [session] {
                    session->close();
                    session->deleteLater();
                });
                logout->start();
                done();
            });
        });
        login->start();
    };

    // OAuth accounts keep a refresh token rather than a password; renew it
    // silently, exactly as the foreground connection does.
    if (authType != 0) {
        auto *read = new QKeychain::ReadPasswordJob(kWalletService, this);
        // Must match oauthWalletKey(): the token is stored per user@host, not
        // under the cache key, which for an imported account differs.
        read->setKey(QStringLiteral("oauth-refresh:") + user + QLatin1Char('@') + host);
        connect(read, &QKeychain::Job::finished, this,
                [this, read, account, connectWith, done] {
            if (read->error() || read->textData().isEmpty()) {
                done();
                return;
            }
            auto *oauth = new OAuthHelper(this);
            // authType is the provider id (see the foreground path).
            const auto provider = OAuthHelper::Provider(
                account.value(QStringLiteral("authType"), 0).toInt());
            connect(oauth, &OAuthHelper::tokensReady, this,
                    [oauth, connectWith](const QString &accessToken, const QString &,
                                         const QDateTime &) {
                oauth->deleteLater();
                connectWith(accessToken);
            });
            connect(oauth, &OAuthHelper::failed, this, [oauth, done](const QString &) {
                oauth->deleteLater();
                done();
            });
            oauth->refresh(provider,
                           account.value(QStringLiteral("clientId")).toString(),
                           account.value(QStringLiteral("clientSecret")).toString(),
                           read->textData());
        });
        read->start();
        return;
    }

    auto *read = new QKeychain::ReadPasswordJob(kWalletService, this);
    read->setKey(walletKeyFor(user, host));
    connect(read, &QKeychain::Job::finished, this, [read, connectWith, done] {
        if (read->error()) {
            done();
            return;
        }
        connectWith(read->textData());
    });
    read->start();
}

void MailClient::statusFoldersOn(KIMAP::Session *session, const QString &accountKey,
                                 const std::function<void()> &done)
{
    auto folders = std::make_shared<QStringList>(m_store.cachedFolders(accountKey));
    if (folders->isEmpty()) {
        // Never synced: INBOX is the one mailbox every server has.
        folders->append(QStringLiteral("INBOX"));
    }
    auto counts = std::make_shared<QHash<QString, int>>();
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, session, accountKey, folders, counts, step, done] {
        if (folders->isEmpty()) {
            // Replace wholesale: a folder that has dropped to zero unread must
            // lose its pill, which merging would never do.
            m_unreadByAccount.insert(accountKey, *counts);
            ++m_cachedFolderRevision;
            Q_EMIT cachedFoldersChanged();
            done();
            return;
        }
        const QString folder = folders->takeFirst();
        auto *status = new KIMAP::StatusJob(session);
        status->setMailBox(folder);
        // UNSEEN only: this is a mail check, not a sync. It needs no SELECT,
        // so it never disturbs the mailbox state of a client that is idling on
        // this account elsewhere.
        status->setDataItems({QByteArrayLiteral("UNSEEN")});
        connect(status, &KJob::result, this, [folder, counts, step](KJob *job) {
            if (!job->error()) {
                const auto items = static_cast<KIMAP::StatusJob *>(job)->status();
                for (const auto &item : items) {
                    if (item.first == "UNSEEN" && item.second > 0)
                        counts->insert(folder, int(item.second));
                }
            }
            (*step)();
        });
        status->start();
    };
    (*step)();
}

void MailClient::sweepOldSpam()
{
    // A local archive is an archive: imported mail is the only copy there is,
    // and quietly eating part of it is not something an import should do.
    qCDebug(logTrace, "spam sweep: days=%d local=%d swept=%d connected=%d",
            m_spamRetentionDays, int(m_local), int(m_spamSwept), int(m_connected));
    if (m_spamRetentionDays <= 0 || m_local || m_spamSwept || !m_connected)
        return;
    const QString junk = junkFolderName();
    if (junk.isEmpty()) {
        qCWarning(logTrace, "spam sweep: no spam folder among %lld mailboxes",
                  qint64(m_folderModel.allMailBoxes().size()));
        return;
    }
    m_spamSwept = true;
    qCDebug(logTrace, "spam sweep: folder=%s older than %d days",
            qUtf8Printable(junk), m_spamRetentionDays);

    // Its own connection, deliberately — not the sync session it used to
    // borrow. Two reasons, and the second is the serious one:
    //
    //  - the sync session is busy with the header backfill, so the sweep's
    //    SELECT queued behind an open-ended amount of work and in practice
    //    never got its turn;
    //  - worse, SEARCH/STORE/EXPUNGE act on whatever mailbox that connection
    //    currently has selected. The jobs are only created as each previous
    //    one finishes, so a backfill SELECT landing in between would have
    //    pointed the expunge at a different folder entirely. Deleting mail is
    //    not something to run on a connection somebody else can re-aim.
    const QString host = m_host;
    const int port = m_port;
    const int days = m_spamRetentionDays;
    const bool toTrash = !m_spamSkipTrash;
    const QString trash = trashFolderName();

    auto *session = new KIMAP::Session(host, quint16(port), this);
    auto finish = [session] {
        auto *logout = new KIMAP::LogoutJob(session);
        connect(logout, &KJob::result, session, [session] {
            session->close();
            session->deleteLater();
        });
        logout->start();
    };

    auto *login = new KIMAP::LoginJob(session);
    configureLogin(login);
    connect(login, &KJob::result, this,
            [this, session, junk, days, toTrash, trash, finish](KJob *job) {
        if (job->error()) {
            setStatus(tr("Spam cleanup: sign-in failed"));
            qCWarning(logTrace, "spam sweep: login failed: %s",
                      qUtf8Printable(job->errorString()));
            session->close();
            session->deleteLater();
            return;
        }
        auto *select = new KIMAP::SelectJob(session);
        select->setMailBox(junk);
        select->setOpenReadOnly(false); // \Deleted needs write access
        connect(select, &KJob::result, this,
                [this, session, junk, days, toTrash, trash, finish](KJob *sel) {
            if (sel->error()) {
                setStatus(tr("Spam cleanup: cannot open %1").arg(junk));
                qCWarning(logTrace, "spam sweep: SELECT %s failed: %s",
                          qUtf8Printable(junk), qUtf8Printable(sel->errorString()));
                finish();
                return;
            }
            // The dates mailo already holds, not an IMAP SEARCH.
            //
            // Both server-side criteria were tried and both matched nothing.
            // BEFORE is INTERNALDATE — when this mailbox received the message
            // — so a recently synced account stamps every message as arriving
            // now and none is ever old. SENTBEFORE should have worked and did
            // not, which leaves the criterion unexplained; the cache needs no
            // explanation, because it holds the very dates the list shows.
            //
            // So what gets deleted is exactly what you can see is old, and a
            // message with no usable date (the 1970 rows) counts as old rather
            // than being allowed to sit in spam forever by saying nothing.
            //
            // Bounded by what has been synced: mail not yet in the cache is
            // not touched, and the next pass picks it up.
            const qint64 cutoff =
                QDateTime::currentDateTimeUtc().addDays(-days).toSecsSinceEpoch();
            const QList<qint64> uids = m_store.uidsOlderThan(junk, cutoff);
            qCDebug(logTrace, "spam sweep: %lld cached messages older than %d days",
                    qint64(uids.size()), days);
            if (uids.isEmpty()) {
                setStatus(tr("Spam cleanup: nothing older than %1 days").arg(days));
                finish();
                return;
            }
            KIMAP::ImapSet set;
            for (qint64 uid : uids)
                set.add(uid);

            // Cache rows go only once the server has confirmed, so a failure
            // never leaves mail on the server that mailo has forgotten.
            auto settle = [this, junk, uids](const QString &crumb) {
                m_store.removeMessages(junk, uids);
                if (m_selectedFolder == junk)
                    m_messageModel.removeByUids(uids);
                scheduleUnreadRecount();
                setStatus(crumb);
            };

            if (toTrash && !trash.isEmpty()) {
                auto *move = new KIMAP::MoveJob(session);
                move->setUidBased(true);
                move->setSequenceSet(set);
                move->setMailBox(trash);
                connect(move, &KJob::result, this,
                        [this, uids, settle, finish](KJob *mjob) {
                    if (mjob->error()) {
                        setStatus(tr("Spam cleanup: move to trash failed"));
                        qCWarning(logTrace, "spam sweep: MOVE failed: %s",
                                  qUtf8Printable(mjob->errorString()));
                    } else {
                        settle(tr("Moved %1 from spam to trash")
                                   .arg(countNoun(uids.size(), "old message",
                                                  "old messages")));
                    }
                    finish();
                });
                move->start();
                return;
            }

            auto *store = new KIMAP::StoreJob(session);
            store->setUidBased(true);
            store->setSequenceSet(set);
            store->setMode(KIMAP::StoreJob::AppendFlags);
            store->setFlags({QByteArrayLiteral("\\Deleted")});
            connect(store, &KJob::result, this,
                    [this, session, uids, settle, finish](KJob *stjob) {
                if (stjob->error()) {
                    setStatus(tr("Spam cleanup: marking failed"));
                    qCWarning(logTrace, "spam sweep: STORE failed: %s",
                              qUtf8Printable(stjob->errorString()));
                    finish();
                    return;
                }
                auto *expunge = new KIMAP::ExpungeJob(session);
                connect(expunge, &KJob::result, this,
                        [this, uids, settle, finish](KJob *ejob) {
                    if (ejob->error()) {
                        setStatus(tr("Spam cleanup: delete failed"));
                        qCWarning(logTrace, "spam sweep: EXPUNGE failed: %s",
                                  qUtf8Printable(ejob->errorString()));
                    } else {
                        settle(tr("Deleted %1 from spam")
                                   .arg(countNoun(uids.size(), "old message",
                                                  "old messages")));
                    }
                    finish();
                });
                expunge->start();
            });
            store->start();
        });
        select->start();
    });
    login->start();
}

void MailClient::scheduleUnreadRecount()
{
    if (!m_unreadDebounce.isSingleShot()) {
        m_unreadDebounce.setSingleShot(true);
        // Long enough that reading a run of messages, or a sync landing a
        // batch of headers, collapses into one pass; short enough that the
        // pill still feels like it answers the click.
        m_unreadDebounce.setInterval(700);
        connect(&m_unreadDebounce, &QTimer::timeout, this,
                &MailClient::startUnreadRecount);
    }
    m_unreadDebounce.start();
}

void MailClient::startUnreadRecount()
{
    if (m_unreadThread) {
        m_unreadRecountQueued = true; // re-run once this one lands
        return;
    }
    // Every account, not just the open one: the pane shows the other accounts'
    // cached trees at the same time, and one connection answers for all of them.
    QStringList accounts;
    {
        QSettings s = appSettings();
        const int count = s.beginReadArray(QStringLiteral("accounts"));
        s.endArray();
        for (int i = 0; i < count; ++i) {
            QSettings a = appSettings();
            const QString key = storedAccountKeyAt(a, i);
            if (!key.isEmpty() && !accounts.contains(key))
                accounts.append(key);
        }
    }
    if (accounts.isEmpty())
        return;

    auto result = std::make_shared<QHash<QString, QHash<QString, int>>>();
    m_unreadThread = QThread::create([accounts, result] {
        QSqlDatabase db = MailStore::openWorkerConnection(QStringLiteral("mailstore-unread"));
        if (db.isOpen()) {
            for (const QString &account : accounts)
                result->insert(account, MailStore::unreadCountsOn(db, account));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("mailstore-unread"));
    });
    connect(m_unreadThread, &QThread::finished, this, [this, result] {
        m_unreadThread->deleteLater();
        m_unreadThread = nullptr;
        m_unreadByAccount = *result;
        m_folderModel.setUnreadCounts(m_unreadByAccount.value(accountKey()));
        ++m_cachedFolderRevision;
        Q_EMIT cachedFoldersChanged(); // repaints the other accounts' trees
        if (m_unreadRecountQueued) {
            m_unreadRecountQueued = false;
            scheduleUnreadRecount();
        }
    });
    // The sidebar is never worth a stutter in the list the user is reading.
    m_unreadThread->start(QThread::LowestPriority);
}

void MailClient::purgeCachedFolders(const QStringList &folders)
{
    if (folders.isEmpty())
        return;
    stopFolderOps();
    // Both purges open a connection under the same name, so they must not run
    // at once. The archive one is restartable and picks up where it stopped on
    // the next connect, so cancelling it here costs nothing.
    stopAllMailPurge();
    m_folderOpCancel.storeRelaxed(0);
    QStringList keys;
    keys.reserve(folders.size());
    for (const QString &folder : folders)
        keys.append(m_store.scopedKey(folder));
    m_folderOpThread = QThread::create([this, keys] {
        for (const QString &key : keys) {
            if (m_folderOpCancel.loadRelaxed())
                return;
            // Chunked with a yield between chunks, so the GUI thread never
            // queues behind this for a write lock (see MailStore::purgeFolder).
            MailStore::purgeFolder(key, m_folderOpCancel, {});
        }
    });
    connect(m_folderOpThread, &QThread::finished, this, [this] {
        m_folderOpThread->deleteLater();
        m_folderOpThread = nullptr;
        invalidateMissingBodies();
    });
    m_folderOpThread->start(QThread::LowestPriority);
}

void MailClient::stopFolderOps()
{
    if (!m_folderOpThread)
        return;
    m_folderOpCancel.storeRelaxed(1);
    m_folderOpThread->wait();
}

void MailClient::openFolder(const QString &mailBox)
{
    qCDebug(logTrace, "openFolder(%s)  selected=%s pending=%s",
            qUtf8Printable(mailBox), qUtf8Printable(m_selectedFolder),
            qUtf8Printable(m_pendingFolder));
    m_backfillTimer.stop();
    m_backfill = false;
    m_bodyBackfill = false;
    m_searchActive = false;
    // A deliberate folder change is a fresh start — un-pause any throttled
    // backfill and clear the backoff so this folder syncs at full pace.
    resetBackfillBackoff();
    // Queued prefetch uids belong to the folder that queued them.
    m_prefetchQueue.clear();
    // Whatever an account switch or a reconnect meant to reopen, this call
    // supersedes it. Leaving it set let listFolders() land a moment later and
    // yank the user back to the folder they had switched accounts with —
    // click a folder in account A, a non-INBOX folder in account B, then
    // INBOX, and INBOX opened and was immediately replaced by the second one.
    m_pendingFolder.clear();
    // Reopening the folder already on screen — which listFolders() does after
    // every reconnect — used to empty the list and refill it from cache. That
    // churn is what the view has to defend its selection against, and it buys
    // nothing: the rows are already the right ones and the network refresh
    // below merges into them either way. Only a real folder change rebuilds.
    const bool reopening = mailBox == m_selectedFolder && m_messageModel.rowCount() > 0;
    m_selectedFolder = mailBox;
    Q_EMIT selectedFolderChanged();
    m_folderReadWrite = false;

    QList<MessageListModel::Header> cached;
    if (!reopening) {
        m_messageModel.clear();
        // Show the cache instantly; the network refresh merges into it.
        cached = m_store.cachedHeaders(mailBox);
        updatePageAnchor(cached);
        if (!cached.isEmpty())
            m_messageModel.setHeaders(cached);
    }

    if (!m_connected || !m_session) {
        // Not cached.size(): that is one page (max 1000 rows), not the folder.
        // A local archive is not "offline" — the cache is the whole account.
        setStatus(m_local ? tr("%1 — %2 messages")
                                .arg(mailBox)
                                .arg(m_store.cachedHeaderCount(mailBox))
                          : tr("%1 — offline, %2 cached")
                                .arg(mailBox)
                                .arg(m_store.cachedHeaderCount(mailBox)));
        return;
    }
    setBusy(true);
    // Reopening always means refreshing: the rows are already on screen.
    if (reopening || !cached.isEmpty())
        setStatus(tr("%1 refreshing").arg(mailBox));
    else
        setStatus(tr("Opening %1").arg(mailBox));

    auto *select = new KIMAP::SelectJob(m_session);
    select->setMailBox(mailBox);
    select->setOpenReadOnly(true);
    // Full-cache numbers from SQL, not from the (row-limited) preview list —
    // the resume point must account for everything ever fetched.
    connect(select, &KJob::result, this,
            [this, mailBox, maxCachedUid = m_store.maxCachedUid(mailBox),
             cachedCount = m_store.cachedHeaderCount(mailBox)](KJob *job) mutable {
        auto *select = static_cast<KIMAP::SelectJob *>(job);
        // The user already clicked another folder while this SELECT was
        // queued — that folder's own flow owns the models and busy state now.
        if (mailBox != m_selectedFolder)
            return;
        if (job->error()) {
            setBusy(false);
            setStatus(tr("Could not open %1").arg(mailBox));
            Q_EMIT errorOccurred(job->errorString());
            return;
        }

        // UIDVALIDITY tracking: if the server regenerated the mailbox, every
        // cached uid is meaningless — drop the folder's cache and start over.
        const qint64 validity = select->uidValidity();
        const qint64 cachedValidity = m_store.uidValidity(mailBox);
        if (validity > 0 && cachedValidity > 0 && validity != cachedValidity) {
            qWarning() << "mailo: UIDVALIDITY changed for" << mailBox
                       << cachedValidity << "->" << validity << "- clearing cache";
            m_store.clearFolder(mailBox);
            invalidateMissingBodies();
            m_messageModel.clear();
            maxCachedUid = 0;
            cachedCount = 0;
        }
        if (validity > 0 && validity != cachedValidity)
            m_store.setUidValidity(mailBox, validity);

        const int count = select->messageCount();
        m_folderMessageCount = count;
        m_oldestFetchedSeq = 0;
        startIdle(); // (re)watch the newly opened folder for pushed mail
        if (count == 0) {
            setBusy(false);
            setStatus(tr("%1 is empty").arg(mailBox));
            return;
        }
        if (cachedCount > 0 && maxCachedUid > 0) {
            // Resume where we left off: fetch only what is newer than the
            // cache and continue the backfill below it.
            fetchNewerThanCache(maxCachedUid, cachedCount);
        } else {
            // Newest 100 messages by sequence number; older ones on demand.
            fetchHeaders(qMax(qint64(1), qint64(count) - 99), count, false);
        }
    });
    select->start();
}

void MailClient::fetchNewerThanCache(qint64 maxCachedUid, int cachedCount)
{
    const QString folder = m_selectedFolder;
    setStatus(tr("%1 — checking").arg(folder));

    auto *fetch = new KIMAP::FetchJob(m_session);
    KIMAP::ImapSet set;
    set.add(KIMAP::ImapInterval(maxCachedUid + 1)); // open end: "uid:*"
    fetch->setSequenceSet(set);
    fetch->setUidBased(true);
    KIMAP::FetchJob::FetchScope scope;
    scope.mode = KIMAP::FetchJob::FetchScope::FullHeaders;
    fetch->setScope(scope);

    auto headers = std::make_shared<QList<MessageListModel::Header>>();
    const QStringList authDomains = trustedAuthDomains();
    connect(fetch, &KIMAP::FetchJob::messagesAvailable, this,
            [this, headers, authDomains, maxCachedUid](const QMap<qint64, KIMAP::Message> &messages) {
                // "uid:*" always returns at least the mailbox's newest message,
                // even when its uid is below the requested range — hence the
                // minUid cutoff.
                appendScoredHeaders(*headers, messages, authDomains, maxCachedUid);
            });

    connect(fetch, &KJob::result, this, [this, headers, cachedCount, folder](KJob *job) {
        setBusy(false);
        if (job->error()) {
            setStatus(tr("Fetching headers failed"));
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        m_store.storeHeaders(folder, *headers);
        invalidateMissingBodies();
        scheduleUnreadRecount(); // freshly synced headers change the pills
        // Cache is updated above either way, but the visible list and the
        // backfill cursor belong to whatever folder is open NOW.
        if (folder != m_selectedFolder || m_searchActive)
            return;
        const auto merged = m_store.cachedHeaders(m_selectedFolder);
        updatePageAnchor(merged);
        m_messageModel.setHeaders(merged);
        Q_EMIT folderRefreshed();
        // Cached block + newly fetched mail occupy the top of the mailbox;
        // everything below is still-unfetched history for the backfill.
        m_oldestFetchedSeq = qMax(
            qint64(1), m_folderMessageCount - cachedCount - headers->size() + 1);
        if (m_oldestFetchedSeq > 1) {
            setStatus(openFolderSyncStatus(m_selectedFolder));
        } else {
            // rowCount is page-limited; report the folder's real size.
            setStatus(tr("%1 — %2 cached")
                          .arg(m_selectedFolder)
                          .arg(m_folderMessageCount > 0
                                   ? m_folderMessageCount
                                   : m_messageModel.rowCount()));
        }
        scheduleBackfill(); // more headers, or the body-caching phase
    });
    fetch->start();
}

void MailClient::scheduleBackfill(int delayMs)
{
    // The timer tick decides what still needs doing: older header windows
    // first, then missing bodies; it stops arming itself when both are done.
    m_backfillTimer.start(delayMs);
}

void MailClient::backoffBackfill()
{
    ++m_backfillAttempt;
    if (m_backfillAttempt > kBackoffMaxAttempts) {
        // Give up retrying for now — the server is persistently pushing back.
        // Sync resumes on the next (re)connect or when the user opens another
        // folder (both reset the attempt counter via resetBackfillBackoff).
        m_syncPaused = true;
        m_backfillTimer.stop();
        setStatus(tr("%1 — sync paused (server busy)")
                      .arg(m_selectedFolder.isEmpty() ? tr("Mail")
                                                       : m_selectedFolder));
        qWarning() << "mailo: backfill paused after" << kBackoffMaxAttempts
                   << "throttle/backoff attempts";
        return;
    }
    // Wait = min(2^n * base + full jitter, cap).
    const qint64 exp = qint64(kBackoffBaseMs) << m_backfillAttempt; // 2^n * base
    const int jitter = int(QRandomGenerator::global()->bounded(kBackoffJitterMs + 1));
    const int wait = int(qMin<qint64>(exp + jitter, kBackoffCapMs));
    scheduleBackfill(wait);
}

void MailClient::resetBackfillBackoff()
{
    m_backfillAttempt = 0;
    m_syncPaused = false;
}

bool MailClient::backfillBodies(const QString &folder)
{
    if (folder.isEmpty())
        return false;
    const auto missing = m_store.uidsWithoutBody(folder, 50);
    if (missing.isEmpty()) {
        if (folder == m_selectedFolder && m_bodyBackfill) {
            m_bodyBackfill = false;
            setStatus(tr("%1 — fully synced")
                          .arg(folder));
        }
        return false; // nothing left in this folder
    }
    if (folder == m_selectedFolder)
        m_bodyBackfill = true;
    // Compose with any header-sync progress so this doesn't clobber the
    // "N of M synced" figure while both phases are running.
    const QString composed = openFolderSyncStatus(folder);
    if (!composed.isEmpty()) {
        setStatus(composed);
    } else {
        const int remaining = missingBodiesIn(folder);
        setStatus(remaining == 1
                      ? tr("%1 — caching 1 body").arg(folder)
                      : tr("%1 — caching %2 bodies").arg(folder).arg(remaining));
    }
    for (qint64 uid : missing) {
        const auto item = qMakePair(folder, uid);
        if (!m_prefetchQueue.contains(item))
            m_prefetchQueue.append(item);
    }
    processPrefetchQueue();
    scheduleBackfill(); // next batch (or the "done" status) on a later tick
    return true;
}

void MailClient::continueFolderBackfill()
{
    if (!m_connected || !m_syncSession || !m_syncReady || m_folderBackfillPassDone)
        return; // needs the dedicated connection — never hijack the UI one
    if (m_backfillFolder.isEmpty()) {
        if (m_folderBackfillQueue.isEmpty())
            m_folderBackfillQueue = m_folderModel.allMailBoxes();
        while (!m_folderBackfillQueue.isEmpty()) {
            const QString next = m_folderBackfillQueue.takeFirst();
            if (next != m_selectedFolder) {
                m_backfillFolder = next;
                m_backfillOldestSeq = 0; // size unknown until the SELECT below
                break;
            }
        }
        if (m_backfillFolder.isEmpty()) {
            // Every folder visited; a new pass starts on the next (re)connect
            // or folder-list refresh.
            m_folderBackfillPassDone = true;
            setStatus(tr("All folders synced"));
            return;
        }
    }
    const QString folder = m_backfillFolder;
    if (m_backfillOldestSeq == 0) {
        // Fresh folder: learn its size (and UIDVALIDITY) on the sync session.
        auto *select = new KIMAP::SelectJob(m_syncSession);
        select->setMailBox(folder);
        select->setOpenReadOnly(true);
        connect(select, &KJob::result, this, [this, folder](KJob *job) {
            if (folder != m_backfillFolder || !m_syncSession)
                return; // pass was reshuffled meanwhile
            if (job->error()) {
                // Unselectable (\Noselect) or otherwise broken — skip it.
                m_backfillFolder.clear();
                scheduleBackfill(1000);
                return;
            }
            auto *select = static_cast<KIMAP::SelectJob *>(job);
            m_syncFolder = folder;
            const qint64 validity = select->uidValidity();
            const qint64 cachedValidity = m_store.uidValidity(folder);
            if (validity > 0 && cachedValidity > 0 && validity != cachedValidity) {
                qWarning() << "mailo: UIDVALIDITY changed for" << folder
                           << cachedValidity << "->" << validity << "- clearing cache";
                m_store.clearFolder(folder);
                invalidateMissingBodies();
            }
            if (validity > 0 && validity != cachedValidity)
                m_store.setUidValidity(folder, validity);
            const qint64 count = select->messageCount();
            // Everything already in the cache → straight to the body phase.
            m_backfillOldestSeq =
                (count <= 0 || m_store.cachedHeaderCount(folder) >= count)
                ? 1
                : count + 1;
            scheduleBackfill(50);
        });
        select->start();
        return;
    }
    if (m_backfillOldestSeq > 1) {
        // Next header window; cache-only — fetchHeadersOn never touches the
        // visible list for a folder that is not the open one.
        m_headerFetch = true;
        m_backfill = true;
        const qint64 to = m_backfillOldestSeq - 1;
        const qint64 from = qMax(qint64(1), to - 249);
        withSyncSession(folder, [this, folder, from, to](KIMAP::Session *session) {
            if (!session) {
                m_headerFetch = false;
                m_backfill = false;
                return;
            }
            fetchHeadersOn(session, folder, from, to, true, true);
        });
        return;
    }
    if (!backfillBodies(folder)) {
        // Headers and bodies complete — move on to the next folder.
        m_backfillFolder.clear();
        scheduleBackfill(50);
    }
}

void MailClient::updatePageAnchor(const QList<MessageListModel::Header> &page)
{
    if (page.isEmpty()) {
        m_pageDate = 0;
        m_pageUid = 0;
        return;
    }
    // cachedHeaders* returns date DESC — the last row is the oldest shown.
    m_pageDate = page.last().date.toSecsSinceEpoch();
    m_pageUid = page.last().uid;
}

void MailClient::loadMoreMessages()
{
    if (m_searchActive)
        return;
    // Older mail already cached on disk appears instantly, without touching
    // the network; the server is only asked below the end of the cache.
    if (m_pageUid > 0) {
        const auto older =
            m_store.cachedHeadersBefore(m_selectedFolder, m_pageDate, m_pageUid);
        if (!older.isEmpty()) {
            updatePageAnchor(older);
            m_messageModel.appendHeaders(older);
            return;
        }
    }
    fetchOlderFromServer();
}

bool MailClient::loadAllCachedMessages()
{
    if (m_searchActive || m_pageUid <= 0)
        return false;
    // One unlimited query rather than a loop of pages: the folder index
    // already orders these rows, so the cost is in handing them to the model,
    // and doing that once beats doing it eighty times. For a local archive
    // this is the whole folder, and so genuinely the oldest message; for a
    // server account it is as far back as the cache goes — anything older
    // still has to be backfilled before it can be jumped to.
    const auto rest = m_store.cachedHeadersBefore(m_selectedFolder, m_pageDate, m_pageUid, -1);
    if (rest.isEmpty())
        return false;
    updatePageAnchor(rest);
    m_messageModel.appendHeaders(rest);
    return true;
}

void MailClient::fetchOlderFromServer()
{
    if (!m_connected || !m_session || m_busy || m_headerFetch
        || m_oldestFetchedSeq <= 1) {
        m_backfill = false;
        return;
    }
    // Background backfill must not flip the busy state — busy is what makes
    // the UI feel blocked, and nothing user-facing is waiting on this.
    if (!m_backfill)
        setBusy(true);
    const qint64 to = m_oldestFetchedSeq - 1;
    // Fetch older history in modest windows with a pause between them
    // (scheduleBackfill below) so sustained backfill stays under server rate
    // limits instead of hammering the connection until it drops.
    fetchHeaders(qMax(qint64(1), to - (kHeaderWindow - 1)), to, true);
}

void MailClient::fetchHeaders(qint64 fromSeq, qint64 toSeq, bool append)
{
    const QString folder = m_selectedFolder;
    m_headerFetch = true;
    if (m_backfill) {
        // Background sync runs on its own connection so a folder click on the
        // main session never waits behind it.
        withSyncSession(folder,
                        [this, folder, fromSeq, toSeq, append](KIMAP::Session *session) {
                            if (!session) {
                                m_headerFetch = false;
                                m_backfill = false;
                                return;
                            }
                            fetchHeadersOn(session, folder, fromSeq, toSeq, append, true);
                        });
        return;
    }
    fetchHeadersOn(m_session, folder, fromSeq, toSeq, append, false);
}

void MailClient::fetchHeadersOn(KIMAP::Session *session, const QString &folder,
                                qint64 fromSeq, qint64 toSeq, bool append,
                                bool background)
{
    if (background) {
        // Show header-sync AND body-caching progress together, so the two
        // background phases don't overwrite each other's numbers.
        const QString composed = openFolderSyncStatus(folder);
        setStatus(!composed.isEmpty()
                      ? composed
                      : tr("%1 — fetching headers").arg(folder));
    } else {
        setStatus(tr("Fetching headers"));
    }

    auto *fetch = new KIMAP::FetchJob(session);
    fetch->setSequenceSet(KIMAP::ImapSet(fromSeq, toSeq));
    fetch->setUidBased(false);
    KIMAP::FetchJob::FetchScope scope;
    // FullHeaders (not Headers): we need Authentication-Results for the
    // SPF/DKIM/DMARC verdict, which the minimal header set doesn't include.
    scope.mode = KIMAP::FetchJob::FetchScope::FullHeaders;
    fetch->setScope(scope);

    auto headers = std::make_shared<QList<MessageListModel::Header>>();
    const QStringList authDomains = trustedAuthDomains();
    connect(fetch, &KIMAP::FetchJob::messagesAvailable, this,
            [this, headers, authDomains](const QMap<qint64, KIMAP::Message> &messages) {
                appendScoredHeaders(*headers, messages, authDomains);
            });

    connect(fetch, &KJob::result, this,
            [this, headers, fromSeq, append, folder, background](KJob *job) {
        m_headerFetch = false;
        if (background)
            m_backfill = false;
        else
            setBusy(false);
        if (job->error()) {
            if (!background) {
                setStatus(tr("Fetching headers failed"));
                Q_EMIT errorOccurred(job->errorString());
            } else {
                // Server pushback (throttling NO/BAD, dropped connection…):
                // retry with growing pauses instead of hammering it. If it is
                // specifically the concurrent-connection cap, also drop back to
                // fewer connections so retries don't hit the same wall.
                if (isTooManyConnections(job->errorString()))
                    shrinkBodyPool();
                backoffBackfill();
            }
            return;
        }
        resetBackfillBackoff();
        m_store.storeHeaders(folder, *headers);
        invalidateMissingBodies();
        scheduleUnreadRecount(); // freshly synced headers change the pills
        // A background pass over a non-open folder: advance that folder's
        // own cursor and keep chaining its windows.
        if (folder != m_selectedFolder && folder == m_backfillFolder) {
            if (m_backfillOldestSeq == 0 || fromSeq < m_backfillOldestSeq)
                m_backfillOldestSeq = fromSeq;
            scheduleBackfill(kHeaderPauseMs);
            return;
        }
        // The user may have moved on: results for a folder that is no longer
        // open go to the cache only — never into the visible list or the
        // current folder's backfill cursor.
        if (folder != m_selectedFolder || m_searchActive)
            return;
        // Only ever move the cursor DOWN: a top-window refresh (poll/IDLE)
        // must not make already-fetched history look unfetched again.
        if (m_oldestFetchedSeq == 0 || fromSeq < m_oldestFetchedSeq)
            m_oldestFetchedSeq = fromSeq;
        if (append) {
            const int added = m_messageModel.appendHeaders(*headers); // dedupes by uid
            if (added == 0 && m_oldestFetchedSeq > 1 && !background) {
                // This window was already on screen from the cache — keep
                // walking older windows until something new shows up,
                // otherwise the scroll appears "stuck" at the cached tail.
                loadMoreMessages();
                return;
            }
        } else {
            // Union of fresh fetch + everything cached, so previously
            // scrolled-in older messages stay visible across sessions.
            const auto merged = m_store.cachedHeaders(m_selectedFolder);
            updatePageAnchor(merged);
            m_messageModel.setHeaders(merged);
            Q_EMIT folderRefreshed();
        }
        if (m_oldestFetchedSeq > 1) {
            // More history on the server — keep syncing it while nothing else
            // is going on, showing header progress together with any body
            // caching so the two phases don't overwrite each other's numbers.
            setStatus(openFolderSyncStatus(m_selectedFolder));
        } else {
            // The visible model is page-limited (cachedHeaders caps at 1000
            // rows) — the status must report the folder's real size.
            const int total = m_folderMessageCount > 0
                ? int(m_folderMessageCount)
                : m_messageModel.rowCount();
            if (background)
                setStatus(tr("%1 — synced, %2 cached").arg(m_selectedFolder).arg(total));
            else
                setStatus(tr("%1 — %2 cached").arg(m_selectedFolder).arg(total));
        }
        // Pace the next window: a deliberate pause between windows keeps the
        // sustained fetch rate under server limits (see kHeaderPauseMs). The
        // longer idle pause is only for entering the body-caching phase.
        scheduleBackfill(m_oldestFetchedSeq > 1 ? kHeaderPauseMs : 500);
    });
    fetch->start();
}

void MailClient::searchMessages(const QString &query, int field)
{
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        clearSearch();
        return;
    }
    // While results are shown, end-of-list scrolling must not page unrelated
    // cached folder rows into the result list.
    m_searchActive = true;

    // /pattern/ → client-side regex filter over the already-loaded list
    if (trimmed.size() > 2 && trimmed.startsWith(QLatin1Char('/')) && trimmed.endsWith(QLatin1Char('/'))) {
        const QRegularExpression re(trimmed.mid(1, trimmed.size() - 2),
                                    QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid()) {
            Q_EMIT errorOccurred(tr("Invalid regular expression: %1").arg(re.errorString()));
            return;
        }
        m_messageModel.applyFilter(re);
        return;
    }

    // A local archive has no server to ask, but the whole archive is in the
    // cache — the local index pass IS the search.
    if (m_local && !m_selectedFolder.isEmpty()) {
        m_searchSeen.clear();
        m_searching = true;
        m_searchFound = 0;
        Q_EMIT searchingChanged();
        localKeywordFilter(trimmed, tr("Search results"), /*append=*/false, field == 0);
        return;
    }

    if (!m_connected || !m_session || m_selectedFolder.isEmpty()) {
        Q_EMIT errorOccurred(tr("Not connected."));
        return;
    }

    setBusy(true);
    // Progress lives in the list itself (Mail.searching drives an overlay
    // there), not in the status breadcrumb. The previous results are NOT
    // cleared here: the search field re-fires on every keystroke, and a clear
    // per letter flashes the list blank. New hits merge into what is showing,
    // and rows the new query does not confirm are pruned when it completes.
    m_searchSeen.clear();
    m_searching = true;
    m_searchFound = 0;
    Q_EMIT searchingChanged();

    // 0 = from + subject, the default: body search drags in every newsletter
    // that ever mentioned the word. "Everything" (1) is the opt-in.
    const bool headersOnly = field == 0;
    const KIMAP::Term term = headersOnly
        ? KIMAP::Term(KIMAP::Term::Or,
                      {KIMAP::Term(KIMAP::Term::From, trimmed),
                       KIMAP::Term(KIMAP::Term::Subject, trimmed)})
        : KIMAP::Term(KIMAP::Term::Text, trimmed);

    auto *search = new KIMAP::SearchJob(m_session);
    search->setUidBased(true);
    search->setTerm(term);
    connect(search, &KJob::result, this, [this, trimmed, headersOnly](KJob *job) {
        if (job->error()) {
            // Some servers reject SEARCH variants; fall back to local matching.
            qWarning() << "IMAP SEARCH failed:" << job->errorString();
            setBusy(false);
            localKeywordFilter(trimmed, tr("Server search failed (%1)").arg(job->errorString()),
                               /*append=*/false, headersOnly);
            return;
        }
        QList<qint64> uids = static_cast<KIMAP::SearchJob *>(job)->results();
        if (uids.isEmpty()) {
            setBusy(false);
            localKeywordFilter(trimmed, tr("No server matches"), /*append=*/false, headersOnly);
            return;
        }
        // Newest 200 hits are plenty for a result list.
        if (uids.size() > 200)
            uids = uids.mid(uids.size() - 200);
        // Merge in local partial-word hits — many servers (Gmail…) match
        // whole words only, so "hung" would otherwise miss "hungarian".
        fetchHeadersByUids(uids, trimmed, headersOnly);
    });
    search->start();
}

void MailClient::localKeywordFilter(const QString &keyword, const QString &reason, bool append,
                                    bool headersOnly)
{
    // The substring pass walks the folder's whole date index, which on a large
    // mailbox is far past a frame's worth of work — so it runs on a worker and
    // hands rows over in batches. The list fills in while it goes rather than
    // staying frozen and then appearing all at once.
    const quint64 seq = m_searchSeq.fetchAndAddOrdered(1) + 1;
    const QString scopedFolder = m_store.scopedKey(m_selectedFolder);
    const bool fts = m_store.ftsAvailable();
    const QString connection = QStringLiteral("mailstore-search-%1").arg(seq);

    // Shared with the worker: what it has delivered so far, so the finish
    // handler can tell "no matches" from "matches already on screen" without
    // asking the model (which may hold server hits too).
    auto delivered = std::make_shared<QAtomicInt>(0);
    if (!m_searching) {
        m_searching = true;
        m_searchFound = append ? m_messageModel.rowCount() : 0;
        Q_EMIT searchingChanged();
    }

    auto *thread = QThread::create([this, seq, scopedFolder, keyword, fts, connection, delivered,
                                    headersOnly] {
        QSqlDatabase db = MailStore::openWorkerConnection(connection);
        if (db.isOpen()) {
            MailStore::searchOn(
                db, scopedFolder, keyword, fts,
                [this, seq, delivered](const QList<MessageListModel::Header> &batch) -> bool {
                    if (m_searchSeq.loadAcquire() != seq)
                        return false; // nobody is waiting for this any more
                    delivered->fetchAndAddOrdered(batch.size());
                    // Queued, never blocking: both passes are capped at 200
                    // rows, so this is a handful of posts and there is nothing
                    // to throttle — while a worker blocking on the GUI thread
                    // would be a deadlock waiting for a shutdown to happen.
                    // Append-only: the list was cleared once when the search
                    // started, and every batch after that inserts sorted rows
                    // in place. Never a reset mid-search — a reset blanks the
                    // view for a frame and reads as flashing.
                    QMetaObject::invokeMethod(
                        this,
                        [this, seq, batch] {
                            if (m_searchSeq.loadAcquire() != seq)
                                return;
                            m_oldestFetchedSeq = 1; // results are not a page of the folder
                            m_messageModel.appendHeaders(batch);
                            for (const auto &h : batch)
                                m_searchSeen.insert(h.uid);
                            m_searchFound = int(m_searchSeen.size());
                            Q_EMIT searchingChanged();
                        },
                        Qt::QueuedConnection);
                    return m_searchSeq.loadAcquire() == seq;
                },
                headersOnly);
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    });
    connect(thread, &QThread::finished, this,
            [this, thread, seq, keyword, reason, delivered, append] {
                thread->deleteLater();
                if (m_searchSeq.loadAcquire() != seq)
                    return; // superseded; whoever replaced us owns the status line
                setBusy(false);
                m_searching = false;
                pruneSearchResults();
                m_searchFound = m_messageModel.rowCount();
                Q_EMIT searchingChanged();
                if (delivered->loadAcquire() > 0)
                    return;
                if (append)
                    return; // server hits stand on their own
                // Nothing in the index: fall back to filtering what is loaded.
                const QRegularExpression re(QRegularExpression::escape(keyword),
                                            QRegularExpression::CaseInsensitiveOption);
                m_messageModel.applyFilter(re);
            });
    // Below the UI's own work: typing the next letter must not wait for this.
    thread->start(QThread::LowPriority);
}

/// Drops rows the just-finished search did not deliver. Runs only at
/// completion: mid-search the old rows are still being confirmed one batch at
/// a time, and pruning early would re-introduce the per-keystroke blanking
/// this exists to avoid.
void MailClient::pruneSearchResults()
{
    if (!m_searchActive)
        return;
    QList<qint64> stale;
    const QList<qint64> uids = m_messageModel.allUids();
    for (qint64 uid : uids) {
        if (!m_searchSeen.contains(uid))
            stale.append(uid);
    }
    if (!stale.isEmpty())
        m_messageModel.removeByUids(stale);
}

void MailClient::clearSearch()
{
    m_searchActive = false;
    abandonLocalSearch(); // its rows would land in the folder we are restoring
    if (m_searching) {
        m_searching = false;
        Q_EMIT searchingChanged();
    }
    m_messageModel.applyFilter(QRegularExpression());
    if (!m_selectedFolder.isEmpty() && m_connected)
        openFolder(m_selectedFolder);
}

void MailClient::fetchHeadersByUids(const QList<qint64> &uids, const QString &localMergeKeyword,
                                    bool headersOnly)
{

    KIMAP::ImapSet set;
    for (qint64 uid : uids)
        set.add(uid);

    auto *fetch = new KIMAP::FetchJob(m_session);
    fetch->setSequenceSet(set);
    fetch->setUidBased(true);
    KIMAP::FetchJob::FetchScope scope;
    scope.mode = KIMAP::FetchJob::FetchScope::FullHeaders;
    fetch->setScope(scope);

    auto headers = std::make_shared<QList<MessageListModel::Header>>();
    const QStringList authDomains = trustedAuthDomains();
    connect(fetch, &KIMAP::FetchJob::messagesAvailable, this,
            [this, headers, authDomains](const QMap<qint64, KIMAP::Message> &messages) {
                appendScoredHeaders(*headers, messages, authDomains);
            });

    connect(fetch, &KJob::result, this,
            [this, headers, localMergeKeyword, headersOnly](KJob *job) {
        setBusy(false);
        if (job->error()) {
            setStatus(tr("Fetching results failed"));
            m_searching = false;
            Q_EMIT searchingChanged();
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        m_oldestFetchedSeq = 1; // disable load-more while showing results
        // Append, not set: a reset would blank the rows already showing.
        m_messageModel.appendHeaders(*headers);
        for (const auto &h : *headers)
            m_searchSeen.insert(h.uid);
        m_searchFound = int(m_searchSeen.size());
        // Local partial-word hits are topped up afterwards, on a worker: the
        // server's answer is already on screen and must not wait for ours.
        // Only that top-up ends the searching state; without one it ends here.
        if (!localMergeKeyword.isEmpty()) {
            localKeywordFilter(localMergeKeyword, tr("Search results"), /*append=*/true,
                               headersOnly);
        } else {
            m_searching = false;
            pruneSearchResults();
        }
        Q_EMIT searchingChanged();
    });
    fetch->start();
}

/// True for MIME parts that carry an iCalendar invite (.ics).
static bool partIsCalendar(KMime::Content *part)
{
    if (const auto *ct = std::as_const(*part).contentType()) {
        const QByteArray mime = ct->mimeType().toLower();
        if (mime == "text/calendar" || mime == "application/ics")
            return true;
    }
    QString name;
    if (const auto *cd = std::as_const(*part).contentDisposition())
        name = cd->filename();
    if (name.isEmpty()) {
        if (const auto *ct = std::as_const(*part).contentType())
            name = ct->name();
    }
    return name.toLower().endsWith(QLatin1String(".ics"));
}

void MailClient::refineAttachKind(const QString &folder, qint64 uid, KMime::Message *msg)
{
    // Refined (calendar icon in the list) only when every attachment is an
    // .ics; a mixed set keeps the head-derived generic flag.
    const auto parts = msg->attachments();
    if (uid < 0 || parts.isEmpty())
        return;
    for (KMime::Content *part : parts) {
        if (!partIsCalendar(part))
            return;
    }
    m_store.setAttachKind(folder, uid, MessageListModel::CalendarAttachment);
    if (folder == m_selectedFolder)
        m_messageModel.setAttachKind(uid, MessageListModel::CalendarAttachment);
}

/// Corrects the list's OpenPGP mark once the body is here. The head can only
/// show the outer content type: inline PGP is invisible to it, and so is a
/// message whose declared type promised a structure it does not have.
void MailClient::refineCrypto(const QString &folder, qint64 uid, KMime::Message *msg)
{
    if (uid < 0)
        return;
    const int kind = PgpMime::storedKind(PgpMime::classify(msg).kind);
    if (kind == PgpMime::storedKind(PgpMime::kindFromHead(msg->head())))
        return; // the head already had it right, which is the common case
    m_store.setCrypto(folder, uid, kind);
    if (folder == m_selectedFolder)
        m_messageModel.setCrypto(uid, kind);
}

void MailClient::collectInlineParts(MessageContext *ctx, KMime::Content *root)
{
    if (const auto *cid = std::as_const(*root).contentID(); cid && !cid->identifier().isEmpty()) {
        const auto *ct = std::as_const(*root).contentType();
        m_viewerHandler->setInlinePart(ctx->viewerContext(),
                                       QString::fromLatin1(cid->identifier()),
                                       ct ? ct->mimeType() : QByteArray(),
                                       root->decodedBody());
    }
    const auto children = root->contents();
    for (KMime::Content *child : children)
        collectInlineParts(ctx, child);
}

/// Strips the parts of a sender-supplied filename that can misrepresent what
/// the file is: directory components, C0/C1 control characters, and the Unicode
/// direction overrides that make "invoice<U+202E>cod.exe" render as
/// "invoiceexe.doc" in every label we put it in.
static QString sanitizeFileName(const QString &raw)
{
    QString out;
    out.reserve(raw.size());
    for (const QChar c : raw) {
        const char16_t u = c.unicode();
        const bool bidiControl = (u >= 0x202A && u <= 0x202E) // LRE…RLO, PDF
            || (u >= 0x2066 && u <= 0x2069)                   // LRI…PDI
            || u == 0x200E || u == 0x200F || u == 0x061C;     // LRM, RLM, ALM
        if (u < 0x20 || (u >= 0x7F && u <= 0x9F) || bidiControl)
            continue;
        out.append(c == QLatin1Char('/') || c == QLatin1Char('\\') ? QLatin1Char('_') : c);
    }
    out = out.trimmed();
    while (out.startsWith(QLatin1Char('.'))) // no hidden files, no "." or ".."
        out.remove(0, 1);
    return out.left(200); // stay under NAME_MAX once a suffix is appended
}

void MailClient::collectAttachments(MessageContext *ctx, KMime::Content *root)
{
    ctx->m_attachmentParts.clear();
    ctx->m_attachments.clear();
    const auto parts = root->attachments();
    for (KMime::Content *part : parts) {
        QString name;
        if (const auto *cd = std::as_const(*part).contentDisposition())
            name = cd->filename();
        if (name.isEmpty()) {
            if (const auto *ct = std::as_const(*part).contentType())
                name = ct->name();
        }
        // Sanitize once, here, so the list label, the confirmation dialog, the
        // risky-extension check and the on-disk name all agree on one string.
        name = sanitizeFileName(QFileInfo(name).fileName());
        if (name.isEmpty())
            name = tr("attachment %1").arg(ctx->m_attachmentParts.size() + 1);

        ctx->m_attachmentParts.append(part);
        ctx->m_attachments.append(QVariantMap{
            {QStringLiteral("name"), name},
            {QStringLiteral("sizeText"), QLocale().formattedDataSize(part->decodedBody().size())},
        });
    }
}

/// Escapes \a text for HTML with web links wrapped in anchors, so the
/// plain-text view gets clickable URLs. Detection runs on the raw text and
/// each piece is escaped separately — a match on already-escaped text would
/// trip over the &amp; entities inside query strings. Only ever produces
/// http(s) hrefs, and the viewer opens link clicks externally anyway (the
/// WebEngineView never navigates), so this adds no surface beyond the text.
static QString escapeAndLinkify(const QString &text)
{
    static const QRegularExpression urlRe(
        QStringLiteral("\\b(?:https?://|www\\.)[^\\s<>\"]+"),
        QRegularExpression::CaseInsensitiveOption);
    QString out;
    out.reserve(text.size() + text.size() / 8);
    qsizetype pos = 0;
    auto it = urlRe.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        QString url = m.captured();
        // Trailing punctuation belongs to the sentence, not the URL —
        // "see https://a.b/c." must not link the dot. Brackets only come off
        // while unbalanced, so Wikipedia-style "…/Foo_(bar)" paths survive.
        static const QString trailing = QStringLiteral(".,;:!?'\"");
        while (!url.isEmpty()) {
            const QChar last = url.back();
            if (trailing.contains(last)) {
                url.chop(1);
                continue;
            }
            if ((last == QLatin1Char(')')
                 && url.count(QLatin1Char('(')) < url.count(QLatin1Char(')')))
                || (last == QLatin1Char(']')
                    && url.count(QLatin1Char('[')) < url.count(QLatin1Char(']')))) {
                url.chop(1);
                continue;
            }
            break;
        }
        // "www." alone (or all-punctuation leftovers) is not a link.
        if (url.length() <= 4) {
            out += text.mid(pos, m.capturedEnd() - pos).toHtmlEscaped();
            pos = m.capturedEnd();
            continue;
        }
        out += text.mid(pos, m.capturedStart() - pos).toHtmlEscaped();
        const QString href = url.startsWith(QLatin1String("www."), Qt::CaseInsensitive)
            ? QStringLiteral("https://") + url
            : url;
        out += QStringLiteral("<a href=\"") + href.toHtmlEscaped() + QStringLiteral("\">")
            + url.toHtmlEscaped() + QStringLiteral("</a>");
        pos = m.capturedStart() + url.size();
    }
    out += text.mid(pos).toHtmlEscaped();
    return out;
}

static QByteArray preformattedPage(const QString &content, bool monospace, bool linkify = false)
{
    return QByteArrayLiteral("<html><head><meta charset=\"utf-8\"></head><body><pre style=\""
                             "white-space:pre-wrap;word-break:break-word;font-family:")
        + (monospace ? QByteArrayLiteral("monospace") : QByteArrayLiteral("sans-serif"))
        + QByteArrayLiteral(";\">")
        + (linkify ? escapeAndLinkify(content) : content.toHtmlEscaped()).toUtf8()
        + QByteArrayLiteral("</pre></body></html>");
}

QString MailClient::htmlViewUrl()
{
    return htmlViewUrlFor(m_reading);
}

QString MailClient::htmlViewUrlFor(MessageContext *ctx)
{
    if (!m_viewerHandler)
        return {};
    ctx->m_handler = m_viewerHandler;
    if (ctx->m_htmlBody.isEmpty())
        return textViewUrlFor(ctx);
    // Strip scripting hooks and embedded documents before anything else looks
    // at the markup. Backed by the CSP below — see sanitizeMessageHtml().
    QString html = sanitizeMessageHtml(ctx->m_htmlBody);
    // Point inline references at our scheme handler — but only actual
    // src/href attributes and CSS url() values, not arbitrary body text.
    static const QRegularExpression attrCidRe(
        QStringLiteral("((?:src|href|background)\\s*=\\s*[\"'])cid:"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression cssCidRe(
        QStringLiteral("(url\\(\\s*[\"']?)cid:"), QRegularExpression::CaseInsensitiveOption);
    const QString cidBase =
        QStringLiteral("\\1mailo:cid/") + QString::number(ctx->viewerContext())
        + QLatin1Char('/');
    html.replace(attrCidRe, cidBase);
    html.replace(cssCidRe, cidBase);
    return m_viewerHandler->setMessageHtml(
        ctx->viewerContext(),
        QByteArrayLiteral("<meta charset=\"utf-8\">")
            + messageCsp(ctx->remoteContentAllowed()) + html.toUtf8());
}

QString MailClient::textViewUrl()
{
    return textViewUrlFor(m_reading);
}

QString MailClient::textViewUrlFor(MessageContext *ctx)
{
    if (!m_viewerHandler)
        return {};
    ctx->m_handler = m_viewerHandler;
    QString text = ctx->m_textBody;
    if (text.isEmpty() && !ctx->m_htmlBody.isEmpty()) {
        // HTML-only message: show its stripped text — the junk folders'
        // text-only default must not degrade to an empty stub.
        text = QTextDocumentFragment::fromHtml(ctx->m_htmlBody.left(500000))
                   .toPlainText();
    }
    if (text.isEmpty())
        text = tr("(this message has no displayable text part)");
    // Monospace: plain-text mail (patches, tables, ASCII art — the Bugzilla
    // change tables are the classic case) is written for a fixed-width grid
    // and falls apart in a proportional font. Linkified so URLs are clickable
    // like in the HTML view; the source view stays verbatim.
    return m_viewerHandler->setMessageHtml(ctx->viewerContext(),
                                           preformattedPage(text, true, true));
}

QString MailClient::sourceViewUrl()
{
    return sourceViewUrlFor(m_reading);
}

QString MailClient::sourceViewUrlFor(MessageContext *ctx)
{
    if (!m_viewerHandler)
        return {};
    ctx->m_handler = m_viewerHandler;
    // Always the complete raw RFC-822 message — headers, MIME structure and
    // every part, verbatim. Showing just the HTML part here (as this once
    // did) left HTML mail with a "source" view that had no headers at all.
    return m_viewerHandler->setMessageHtml(
        ctx->viewerContext(), preformattedPage(QString::fromUtf8(ctx->m_raw), true));
}


QString MailClient::attachmentNameFor(const MessageContext *ctx, int index) const
{
    // Basename only — a hostile filename must not traverse directories — and
    // sanitized again so this never depends on collectAttachments() having run.
    const QString name = sanitizeFileName(
        QFileInfo(ctx->m_attachments.at(index).toMap().value(QStringLiteral("name")).toString())
            .fileName());
    return name.isEmpty() ? QStringLiteral("attachment") : name;
}

bool MailClient::writeAttachmentFor(const MessageContext *ctx, int index, const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        Q_EMIT errorOccurred(tr("Could not write %1: %2").arg(path, file.errorString()));
        return false;
    }
    file.write(ctx->m_attachmentParts.at(index)->decodedBody());
    return true;
}

void MailClient::saveAttachment(int index, const QUrl &fileUrl)
{
    saveAttachmentFor(m_reading, index, fileUrl);
}

void MailClient::saveAttachmentFor(MessageContext *ctx, int index, const QUrl &fileUrl)
{
    if (index < 0 || index >= ctx->m_attachmentParts.size())
        return;
    if (writeAttachmentFor(ctx, index, fileUrl.toLocalFile()))
        setStatus(tr("Saved %1").arg(QFileInfo(fileUrl.toLocalFile()).fileName()));
}

bool MailClient::attachmentRisky(int index) const
{
    return attachmentRiskyFor(m_reading, index);
}

bool MailClient::attachmentRiskyFor(const MessageContext *ctx, int index) const
{
    if (index < 0 || index >= ctx->m_attachmentParts.size())
        return false;
    const QString name =
        ctx->m_attachments.at(index).toMap().value(QStringLiteral("name")).toString().toLower();
    static const QStringList riskyExtensions = {
        // Shells and interpreters
        QStringLiteral(".sh"),        QStringLiteral(".bash"),    QStringLiteral(".zsh"),
        QStringLiteral(".ksh"),       QStringLiteral(".csh"),     QStringLiteral(".fish"),
        QStringLiteral(".py"),        QStringLiteral(".pyc"),     QStringLiteral(".pyo"),
        QStringLiteral(".pl"),        QStringLiteral(".rb"),      QStringLiteral(".lua"),
        QStringLiteral(".php"),       QStringLiteral(".tcl"),     QStringLiteral(".awk"),
        // Native executables and libraries
        QStringLiteral(".run"),       QStringLiteral(".bin"),     QStringLiteral(".elf"),
        QStringLiteral(".so"),        QStringLiteral(".out"),     QStringLiteral(".exe"),
        QStringLiteral(".dll"),       QStringLiteral(".scr"),     QStringLiteral(".com"),
        QStringLiteral(".pif"),       QStringLiteral(".cpl"),     QStringLiteral(".msc"),
        // Packages and installers — opening these hands off to a package tool
        QStringLiteral(".appimage"),  QStringLiteral(".flatpakref"),
        QStringLiteral(".flatpakrepo"), QStringLiteral(".snap"),  QStringLiteral(".deb"),
        QStringLiteral(".rpm"),       QStringLiteral(".msi"),     QStringLiteral(".msix"),
        QStringLiteral(".appx"),      QStringLiteral(".pkg"),     QStringLiteral(".dmg"),
        // Windows scripting hosts
        QStringLiteral(".bat"),       QStringLiteral(".cmd"),     QStringLiteral(".ps1"),
        QStringLiteral(".psm1"),      QStringLiteral(".vbs"),     QStringLiteral(".vbe"),
        QStringLiteral(".js"),        QStringLiteral(".jse"),     QStringLiteral(".wsf"),
        QStringLiteral(".wsh"),       QStringLiteral(".hta"),     QStringLiteral(".reg"),
        // Launchers and shortcuts — these run something else
        QStringLiteral(".desktop"),   QStringLiteral(".lnk"),     QStringLiteral(".url"),
        QStringLiteral(".appref-ms"), QStringLiteral(".jar"),     QStringLiteral(".jnlp"),
        // Mountable images: opening one exposes whatever is inside it
        QStringLiteral(".iso"),       QStringLiteral(".img"),     QStringLiteral(".vhd"),
        QStringLiteral(".vhdx"),      QStringLiteral(".udf")};
    for (const QString &ext : riskyExtensions) {
        if (name.endsWith(ext))
            return true;
    }
    // No extension at all: nothing tells the desktop what this is, so the
    // handler is decided by content sniffing. Treat it as needing confirmation.
    if (!name.contains(QLatin1Char('.')))
        return true;
    // Also honor what the sender *declared* — a lie either way is suspicious.
    const QByteArray mime = std::as_const(*ctx->m_attachmentParts.at(index)).contentType()
        ? std::as_const(*ctx->m_attachmentParts.at(index)).contentType()->mimeType().toLower()
        : QByteArray();
    return mime.contains("executable") || mime.contains("x-sharedlib")
        || mime.contains("x-desktop") || mime.contains("shellscript")
        || mime.contains("x-msdownload") || mime.contains("java-archive");
}

void MailClient::openAttachment(int index)
{
    openAttachmentFor(m_reading, index);
}

void MailClient::openAttachmentFor(MessageContext *ctx, int index)
{
    if (index < 0 || index >= ctx->m_attachmentParts.size())
        return;
    // A fixed path under /tmp is reachable by every other user on the machine:
    // they can pre-create the directory, plant a symlink at a plausible file
    // name so our write lands somewhere else, or swap the contents between the
    // write and the open. QTemporaryDir gives us a 0700 directory with an
    // unpredictable name, which closes all three. Process-lifetime static:
    // cleaned up on exit, and files must outlive this call so the handler
    // application can still read them.
    static QTemporaryDir tempDir(QDir::tempPath() + QStringLiteral("/mailo-attachments-XXXXXX"));
    if (!tempDir.isValid()) {
        Q_EMIT errorOccurred(tr("Could not create a private temporary directory: %1")
                                 .arg(tempDir.errorString()));
        return;
    }
    const QString path = tempDir.filePath(attachmentNameFor(ctx, index));
    if (!writeAttachmentFor(ctx, index, path))
        return;
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        setStatus(tr("Opened %1").arg(attachmentNameFor(ctx, index)));
    else
        Q_EMIT errorOccurred(tr("No application could open %1.")
                                 .arg(attachmentNameFor(ctx, index)));
}

void MailClient::saveAttachmentToDownloads(int index)
{
    saveAttachmentToDownloadsFor(m_reading, index);
}

void MailClient::saveAttachmentToDownloadsFor(MessageContext *ctx, int index)
{
    if (index < 0 || index >= ctx->m_attachmentParts.size())
        return;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir().mkpath(dir);

    const QFileInfo info(attachmentNameFor(ctx, index));
    QString candidate = dir + QLatin1Char('/') + info.fileName();
    for (int i = 1; QFile::exists(candidate); ++i) {
        const QString suffix = info.completeSuffix().isEmpty()
            ? QString()
            : QLatin1Char('.') + info.completeSuffix();
        candidate = QStringLiteral("%1/%2 (%3)%4").arg(dir, info.baseName()).arg(i).arg(suffix);
    }
    if (writeAttachmentFor(ctx, index, candidate))
        setStatus(tr("Saved to %1").arg(candidate));
}

void MailClient::markMessageColor(const QVariantList &rows, int color)
{
    if (color < 0 || color > 5)
        return;
    for (const QVariant &v : rows) {
        const int row = v.toInt();
        const qint64 uid = m_messageModel.uidAt(row);
        if (uid < 0)
            continue;
        const int newColor = m_messageModel.colorLabelAt(row) == color ? 0 : color;
        m_messageModel.setColorLabel(uid, newColor);
        m_store.setColorLabel(m_selectedFolder, uid, newColor);
    }
}

void MailClient::filterByColor(int color)
{
    m_messageModel.setColorFilter(color);
    if (color <= 0)
        return;
    // The loaded page holds only the newest rows — pull every cached mark of
    // this color from disk so older marks show as well (indexed query).
    m_messageModel.appendHeaders(m_store.headersByColor(m_selectedFolder, color));
}

void MailClient::markMessagesUnread(const QVariantList &rows)
{
    QList<qint64> uids;
    for (const QVariant &v : rows) {
        const int row = v.toInt();
        const qint64 uid = m_messageModel.uidAt(row);
        if (uid < 0 || !m_messageModel.seenAt(row))
            continue; // already unread: nothing to undo
        m_messageModel.markUnseen(row);
        m_store.setUnseen(m_selectedFolder, uid);
        uids.append(uid);
    }
    if (uids.isEmpty())
        return;
    scheduleUnreadRecount();

    // Nothing here reopens the message. Marking the open one unread leaves it
    // on screen and leaves it unread — the ordinary rule then applies again,
    // and it is only marked read the next time it is actually opened.
    if (!m_connected || !m_session)
        return;
    // Clear \Seen server-side so other clients and the next header sync agree,
    // exactly as markMessageRead() sets it. Best effort, same as there.
    KIMAP::ImapSet set;
    for (qint64 uid : std::as_const(uids))
        set.add(uid);
    auto *store = new KIMAP::StoreJob(m_session);
    store->setUidBased(true);
    store->setSequenceSet(set);
    store->setMode(KIMAP::StoreJob::RemoveFlags);
    store->setFlags({QByteArrayLiteral("\\Seen")});
    connect(store, &KJob::result, this, [this](KJob *job) {
        if (job->error())
            setStatus(tr("Marking unread failed on the server"));
    });
    store->start();
}

void MailClient::markMessageRead(int row)
{
    const qint64 uid = m_messageModel.uidAt(row);
    if (uid < 0)
        return;
    const bool alreadySeen = m_messageModel.seenAt(row);
    m_messageModel.markSeen(row);
    m_store.setSeen(m_selectedFolder, uid);
    if (!alreadySeen)
        scheduleUnreadRecount();
    if (alreadySeen || !m_connected || !m_session)
        return;

    // Push \Seen to the server so other clients (and our next header sync)
    // agree. Best effort: the cache row above already carries the state, and
    // KIMAP fetches with BODY.PEEK, so without this STORE the server would
    // never learn the message was read.
    const QString folder = m_selectedFolder;
    const auto storeSeen = [this, folder, uid]() {
        if (folder != m_selectedFolder || !m_session)
            return;
        auto *store = new KIMAP::StoreJob(m_session);
        store->setUidBased(true);
        store->setSequenceSet(KIMAP::ImapSet(uid));
        store->setMode(KIMAP::StoreJob::AppendFlags);
        store->setFlags({QByteArrayLiteral("\\Seen")});
        connect(store, &KJob::result, this, [](KJob *job) {
            if (job->error())
                qWarning() << "mailo: storing \\Seen failed:" << job->errorString();
        });
        store->start();
    };
    if (m_folderReadWrite) {
        storeSeen();
        return;
    }
    // The browsing SELECT is read-only (EXAMINE); STORE needs read-write.
    auto *select = new KIMAP::SelectJob(m_session);
    select->setMailBox(folder);
    connect(select, &KJob::result, this, [this, folder, storeSeen](KJob *job) {
        if (job->error() || folder != m_selectedFolder)
            return;
        m_folderReadWrite = true;
        storeSeen();
    });
    select->start();
}

void MailClient::fetchMessage(int row)
{
    const qint64 uid = m_messageModel.uidAt(row);
    if (uid < 0) {
        m_detachPending = false;
        return;
    }
    // Remembered so draftData() can name the message it came from.
    m_reading->m_uid = uid;

    // Previously read message → serve from cache, no network needed.
    const QByteArray cachedRaw = m_store.cachedBody(m_selectedFolder, uid);
    if (!cachedRaw.isEmpty()) {
        auto msg = std::make_shared<KMime::Message>();
        msg->setContent(KMime::CRLFtoLF(cachedRaw));
        // The cached form is a stub: attachment payloads live in the file
        // store. Put them back before anything looks at the message. If a
        // payload has gone missing the cache entry is incomplete, so fall
        // through and re-fetch rather than showing an empty attachment.
        msg->parse();
        if (!restoreAttachments(msg.get(), m_store.partsFor(m_selectedFolder, uid))
            && m_connected && m_session) {
            m_store.removeBodyOnly(m_selectedFolder, uid);
        } else {
        m_presentingFromCache = true;
        presentMessage(msg);
        m_presentingFromCache = false;
        refineAttachKind(m_selectedFolder, uid, msg.get());
        markMessageRead(row);
        // No status crumb for opening a cached message — it's silent, the
        // message simply appears.
        // Read-ahead: sequential reading should never wait on the network.
        prefetchMessage(row + 1);
        prefetchMessage(row + 2);
        return;
        }
    }

    if (!m_connected || !m_session) {
        m_detachPending = false;
        setStatus(tr("Not cached — connect to load"));
        return;
    }

    setBusy(true);
    // No "loading…" crumb — the busy spinner already shows activity; only a
    // failure is worth a status.

    auto *fetch = new KIMAP::FetchJob(m_session);
    fetch->setSequenceSet(KIMAP::ImapSet(uid));
    fetch->setUidBased(true);
    KIMAP::FetchJob::FetchScope scope;
    // Full = headers + body. Content alone omits the top-level RFC-2822
    // headers, which carry the multipart boundary — KMime then can't split
    // the parts and every multipart (HTML) mail comes out empty.
    scope.mode = KIMAP::FetchJob::FetchScope::Full;
    fetch->setScope(scope);

    // Servers may split one message's FETCH attributes across several
    // untagged responses, which KIMAP surfaces as multiple messagesAvailable
    // emissions. Accumulate and keep the delivery that actually has content;
    // process only once the job is done.
    auto found = std::make_shared<std::shared_ptr<KMime::Message>>();
    connect(fetch, &KIMAP::FetchJob::messagesAvailable, this,
            [found](const QMap<qint64, KIMAP::Message> &messages) {
                for (const KIMAP::Message &m : messages) {
                    if (!m.message)
                        continue;
                    const bool hasContent = !m.message->body().isEmpty()
                        || !m.message->contents().isEmpty();
                    if (!*found || hasContent)
                        *found = m.message;
                }
            });

    connect(fetch, &KJob::result, this, [this, row, found](KJob *job) {
        setBusy(false);
        if (job->error()) {
            m_detachPending = false;
            setStatus(tr("Message load failed"));
            Q_EMIT errorOccurred(job->errorString());
            return;
        }
        if (!*found) {
            m_detachPending = false;
            setStatus(tr("Message load failed"));
            return;
        }
        presentMessage(*found);
        // Index text: prefer the plain part, else strip the HTML.
        const QString indexText = !m_reading->m_textBody.isEmpty()
            ? m_reading->m_textBody
            : QTextDocumentFragment::fromHtml(m_reading->m_htmlBody).toPlainText();
        m_store.storeBody(m_selectedFolder, m_messageModel.uidAt(row), m_reading->m_raw,
                          indexText);
        refineAttachKind(m_selectedFolder, m_messageModel.uidAt(row), found->get());
        setStatus({});
        markMessageRead(row);
        // Read-ahead: sequential reading should never wait on the network.
        prefetchMessage(row + 1);
        prefetchMessage(row + 2);
    });
    fetch->start();
}

void MailClient::openExternalUrl(const QUrl &url)
{
    const QString scheme = url.scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")
        && scheme != QLatin1String("mailto")) {
        Q_EMIT errorOccurred(tr("Refusing to open %1 link externally.").arg(scheme));
        return;
    }
    if (QDesktopServices::openUrl(url))
        setStatus(tr("Opened %1 in browser").arg(url.host()));
    else
        Q_EMIT errorOccurred(tr("Could not open %1.").arg(url.toString()));
}

void MailClient::setRemoteContentAllowed(bool allow)
{
    m_reading->setRemoteContentAllowed(allow);
}

void MailClient::rememberRemoteContent(const QString &senderAddress, bool allow)
{
    // User toggle: remember the choice for this sender.
    qCDebug(logTrace) << "remote content toggle" << allow << "for sender" << senderAddress;
    m_store.setRemoteContentAllowedFor(senderAddress, allow);
}

static QString indexTextFor(KMime::Message *msg)
{
    // Encrypted mail is not full-text searchable, and this is the one line
    // that makes that true. The index is a plaintext SQLite table; putting a
    // decrypted body in it would undo the encryption for anyone who reads the
    // file, permanently and invisibly (doc/openpgp.md §4). Subject and headers
    // travel in the clear on the wire and stay indexed by the header path.
    //
    // Note this runs on the ciphertext even for a message the reader has open
    // and decrypted: the cache and the viewer never share a tree.
    if (PgpMime::classify(msg).isEncrypted())
        return {};
    KMime::Content *textPart = msg->mainBodyPart("text/plain");
    if (!textPart)
        textPart = findPartByType(msg, "text/plain");
    if (textPart)
        return textPart->decodedText();
    KMime::Content *htmlPart = msg->mainBodyPart("text/html");
    if (!htmlPart)
        htmlPart = findPartByType(msg, "text/html");
    if (htmlPart)
        return QTextDocumentFragment::fromHtml(htmlPart->decodedText().left(100000))
            .toPlainText();
    return {};
}

/// Extracts the searchable text of a batch of cached bodies on a worker thread
/// and writes it back to the index. A full MIME parse plus an HTML-to-text
/// conversion of up to 100 KB is far past the 20 ms budget, and open() seeds
/// the queue with every cached body — on a large cache that was hours of
/// GUI-thread CPU. None of it touches the UI, so none of it belongs there.
void MailClient::reindexPendingBodies()
{
    if (m_reindexThread || m_reclaiming)
        return; // a batch is already in flight
    // 10, not more: reading the batch still pulls raw blobs on this thread, so
    // the read itself has to stay inside the frame budget. The win is in the
    // parsing and the single commit, not in a bigger read.
    const auto batch = m_store.pendingBodyIndex(10);
    if (batch.isEmpty()) {
        m_reindexTimer.stop();
        return;
    }
    m_reindexThread = QThread::create([this, batch] {
        QList<std::tuple<QString, qint64, QString>> done;
        done.reserve(batch.size());
        for (const auto &pending : batch) {
            KMime::Message msg;
            msg.setContent(KMime::CRLFtoLF(pending.raw));
            msg.parse();
            done.append({pending.scopedFolder, pending.uid, indexTextFor(&msg)});
        }
        // The writes go back to the GUI thread's connection: they are small
        // (no blobs) and keeping one writer avoids lock contention entirely.
        QMetaObject::invokeMethod(this, [this, done] {
            m_store.finishBodyIndexBatch(done);
        }, Qt::QueuedConnection);
    });
    connect(m_reindexThread, &QThread::finished, this, [this] {
        m_reindexThread->deleteLater();
        m_reindexThread = nullptr;
    });
    m_reindexThread->setPriority(QThread::LowestPriority);
    m_reindexThread->start();
}

void MailClient::prefetchMessage(int row)
{
    const qint64 uid = m_messageModel.uidAt(row);
    if (uid < 0 || !m_connected || !m_session)
        return;
    const auto item = qMakePair(m_selectedFolder, uid);
    if (m_prefetchQueue.contains(item))
        return;
    if (!m_store.cachedBody(m_selectedFolder, uid).isEmpty())
        return;
    // Newest request first; keep the queue tiny — this is opportunistic.
    // (Trimmed background-backfill entries are re-derived on a later tick.)
    m_prefetchQueue.prepend(item);
    while (m_prefetchQueue.size() > 4)
        m_prefetchQueue.removeLast();
    processPrefetchQueue();
}

// Same-folder run of up to 50 queued uids, removed from the queue. One FETCH
// per batch: the server streams the bodies back to back instead of paying a
// full round trip per message.
static KIMAP::ImapSet takeBodyBatch(QList<QPair<QString, qint64>> &queue,
                                    QString *folderOut)
{
    const QString folder = queue.first().first;
    KIMAP::ImapSet set;
    int taken = 0;
    for (int i = 0; i < queue.size() && taken < 50;) {
        if (queue.at(i).first != folder) {
            ++i;
            continue;
        }
        set.add(KIMAP::ImapSet::Id(queue.at(i).second));
        queue.removeAt(i);
        ++taken;
    }
    *folderOut = folder;
    return set;
}

bool MailClient::bodyFetchActive() const
{
    if (m_prefetching)
        return true;
    for (const auto &conn : m_bodyPool) {
        if (conn->busy)
            return true;
    }
    return false;
}

void MailClient::shrinkBodyPool()
{
    // Stop growing the pool, and shed one idle connection so the concurrent
    // count actually drops. A busy one is left to finish and reused; the cap
    // flag keeps us from re-adding.
    m_bodyPoolBroken = true;
    for (int i = 0; i < m_bodyPool.size(); ++i) {
        if (!m_bodyPool.at(i)->busy) {
            auto conn = m_bodyPool.takeAt(i);
            if (conn->session)
                conn->session->deleteLater();
            break;
        }
    }
    qWarning() << "mailo: reduced body-fetch pool to" << m_bodyPool.size()
               << "after connection-cap refusal";
}

void MailClient::ensureBodyPool()
{
    // Extra connections only once the server has proven it grants us a
    // second one at all (the sync session), and never after a refusal.
    if (!m_connected || !m_syncReady || m_bodyPoolBroken)
        return;
    // Keep the total concurrent connection count low (main + IDLE + sync +
    // these) — well under the ~15 servers like Gmail cap, and near the 2–3
    // recommended to avoid throttling.
    while (m_bodyPool.size() < 2) {
        auto conn = std::make_shared<BodyConn>();
        conn->session = new KIMAP::Session(m_host, quint16(m_port), this);
        m_bodyPool.append(conn);
        const auto drop = [this, conn] {
            if (conn->session)
                conn->session->deleteLater();
            m_bodyPool.removeAll(conn);
        };
        connect(conn->session, &KIMAP::Session::connectionFailed, this, drop);
        connect(conn->session, &KIMAP::Session::connectionLost, this, drop);
        auto *login = new KIMAP::LoginJob(conn->session);
        configureLogin(login);
        connect(login, &KJob::result, this, [this, conn, drop](KJob *job) {
            if (job->error() || !conn->session) {
                qWarning() << "mailo: body-pool login failed:" << job->errorString();
                // The server likely caps concurrent connections — settle for
                // what we have until the next (re)connect.
                m_bodyPoolBroken = true;
                drop();
                return;
            }
            conn->ready = true;
            processPrefetchQueue(); // put the fresh connection to work
        });
        login->start();
    }
}

void MailClient::dispatchBodyBatch(const std::shared_ptr<BodyConn> &conn)
{
    QString folder;
    const KIMAP::ImapSet set = takeBodyBatch(m_prefetchQueue, &folder);
    conn->busy = true;
    const auto release = [conn] { conn->busy = false; };
    if (conn->folder == folder) {
        startBodyFetchJob(conn->session.data(), folder, set, release);
        return;
    }
    auto *select = new KIMAP::SelectJob(conn->session);
    select->setMailBox(folder);
    select->setOpenReadOnly(true);
    connect(select, &KJob::result, this,
            [this, conn, folder, set, release](KJob *job) {
                if (job->error() || !conn->session) {
                    // Dropped uids reappear via uidsWithoutBody on a later tick.
                    release();
                    return;
                }
                conn->folder = folder;
                startBodyFetchJob(conn->session.data(), folder, set, release);
            });
    select->start();
}

void MailClient::processPrefetchQueue()
{
    if (m_prefetchQueue.isEmpty() || !m_connected || !m_session)
        return;
    ensureBodyPool();
    // Preferred path: one in-flight batch per pool connection, in parallel.
    bool poolReady = false;
    for (const auto &conn : m_bodyPool) {
        if (!conn->session || !conn->ready)
            continue;
        poolReady = true;
        if (m_prefetchQueue.isEmpty())
            break;
        if (!conn->busy)
            dispatchBodyBatch(conn);
    }
    if (poolReady || m_prefetching || m_prefetchQueue.isEmpty())
        return;
    // Fallback while the pool is still logging in (or was refused): a single
    // batch on the shared sync connection, so bodies flow either way.
    QString folder;
    const KIMAP::ImapSet set = takeBodyBatch(m_prefetchQueue, &folder);
    m_prefetching = true;
    withSyncSession(folder, [this, folder, set](KIMAP::Session *session) {
        if (!session) {
            m_prefetching = false;
            return;
        }
        startBodyFetchJob(session, folder, set, [this] { m_prefetching = false; });
    });
}

void MailClient::startBodyFetchJob(KIMAP::Session *session, const QString &folder,
                                   const KIMAP::ImapSet &set,
                                   const std::function<void()> &release)
{
    auto *fetch = new KIMAP::FetchJob(session);
    fetch->setSequenceSet(set);
    fetch->setUidBased(true);
    KIMAP::FetchJob::FetchScope scope;
    scope.mode = KIMAP::FetchJob::FetchScope::Full;
    fetch->setScope(scope);

    // Store each body the moment its delivery streams in: memory stays
    // flat (roughly one body at a time) however big the batch is, and
    // the parse/index work spreads across the socket events. `found`
    // only stashes content-less deliveries — a message's attributes may
    // arrive split across several emissions.
    auto found =
        std::make_shared<QHash<qint64, std::shared_ptr<KMime::Message>>>();
    auto stored = std::make_shared<QSet<qint64>>();
    connect(fetch, &KIMAP::FetchJob::messagesAvailable, this,
            [this, folder, found, stored](
                const QMap<qint64, KIMAP::Message> &messages) {
                for (const KIMAP::Message &m : messages) {
                    if (!m.message || m.uid <= 0 || stored->contains(m.uid))
                        continue;
                    if (!m.message->body().isEmpty()
                        || !m.message->contents().isEmpty()) {
                        stored->insert(m.uid);
                        found->remove(m.uid);
                        storeFetchedBody(folder, m.uid, m.message);
                    } else if (!found->contains(m.uid)) {
                        (*found)[m.uid] = m.message;
                    }
                }
            });
    connect(fetch, &KJob::result, this, [this, found, folder, release](KJob *job) {
        if (job->error()) {
            release();
            // Server pushback: pause the backfill loop instead of
            // immediately re-requesting the same bodies. On the concurrent-
            // connection cap, also shed a pool connection.
            if (isTooManyConnections(job->errorString()))
                shrinkBodyPool();
            backoffBackfill();
            return;
        }
        resetBackfillBackoff();
        // Leftovers whose deliveries never showed content (rare): store
        // what we got, one per event-loop pass to keep the GUI fluid.
        auto drain = std::make_shared<std::function<void()>>();
        *drain = [this, found, folder, release, drain]() {
            if (!found->isEmpty()) {
                const qint64 uid = found->cbegin().key();
                storeFetchedBody(folder, uid, found->take(uid));
                if (!found->isEmpty()) {
                    QTimer::singleShot(0, this, *drain);
                    return;
                }
            }
            release();
            processPrefetchQueue();
            // Body backfill: pace the next batch so bulk body downloads stay
            // under server bandwidth limits (bodies are far heavier than
            // headers), rather than requesting the next batch immediately.
            if (m_prefetchQueue.isEmpty() && !bodyFetchActive())
                scheduleBackfill(kBodyPauseMs);
        };
        (*drain)();
    });
    fetch->start();
}

void MailClient::storeFetchedBody(const QString &folder, qint64 uid,
                                  const std::shared_ptr<KMime::Message> &message)
{
    KMime::Message *msg = message.get();
    if (msg->contents().isEmpty())
        msg->parse();
    const QByteArray raw = msg->encodedContent();
    const qint64 cap = qint64(m_maxBodyMB) * 1024 * 1024;
    if (cap > 0 && raw.size() > cap) {
        // A single mail with a big attachment can outweigh thousands of normal
        // ones. Remember the refusal (so the backfill drops it) and move on —
        // opening the message still fetches it on demand.
        m_store.skipBody(folder, uid, raw.size());
        refineAttachKind(folder, uid, msg);
        refineCrypto(folder, uid, msg);
        noteBodyStored(folder);
        return;
    }
    queueBodyWrite({m_store.scopedKey(folder), uid, raw, indexTextFor(msg)});
    noteBodyStored(folder);
    refineAttachKind(folder, uid, msg);
    refineCrypto(folder, uid, msg);
    if (!m_sentFolder.isEmpty() && folder == m_sentFolder)
        harvestRecipients(msg);
}

void MailClient::startDkimVerification(MessageContext *ctx)
{
    ctx->m_dkimStatus.clear();
    ctx->m_dkimDetail.clear();
    ctx->m_arcStatus.clear();
    ctx->m_arcSealer.clear();
    ctx->m_arcDetail.clear();
    ctx->m_dkimTrusted = false;
    ctx->m_dkimChecking = false;
    ctx->m_dkimAttempt = 0;
    ctx->m_dkimFromCache = false;
    // No verification for imported archive mail: the DNS keys its signatures
    // were made against are long gone, so every check would report a failure
    // that says nothing about the message. Nor when the user turned sender
    // authentication off — that must emit no DNS query at all, not merely
    // hide the answer.
    if (m_local || !m_authVerification || !m_dkimVerifier || ctx->m_raw.isEmpty()) {
        Q_EMIT ctx->dkimChanged();
        return;
    }

    // Verified once, not once per open. Re-checking would cost a DNS query
    // every time the message is opened, and for a message whose attachments
    // have since been lifted out of the body there is no byte-exact copy left
    // to re-check against — the verdict from when there was one is the honest
    // answer, not the mismatch the reassembled stub would produce.
    if (ctx->m_uid >= 0 && !ctx->m_folder.isEmpty()) {
        const MailStore::AuthVerdict v = m_store.authVerdict(ctx->m_folder, ctx->m_uid);
        if (!v.isEmpty()) {
            ctx->m_dkimStatus = v.dkimStatus;
            ctx->m_dkimDetail = v.dkimDetail;
            ctx->m_dkimTrusted = v.dkimTrusted;
            ctx->m_arcStatus = v.arcStatus;
            ctx->m_arcSealer = v.arcSealer;
            ctx->m_arcDetail = v.arcDetail;
            qCDebug(logTrace) << "dkim: uid" << ctx->m_uid << "verdict from cache"
                              << ctx->m_dkimStatus;
            Q_EMIT ctx->dkimChanged();
            return;
        }
    }
    submitDkimVerification(ctx);
}

void MailClient::submitDkimVerification(MessageContext *ctx)
{
    if (!m_authVerification || !m_dkimVerifier || ctx->m_raw.isEmpty())
        return;

    // The signature covers the octets as they travelled. KMime holds the
    // message in LF form, and LFtoCRLF restores the wire form byte for byte
    // as long as nothing re-assembled the MIME tree — which is why the raw
    // bytes are kept rather than rebuilt from the parsed parts.
    const QByteArray wire = KMime::LFtoCRLF(ctx->m_raw);

    // Alignment is judged against the From: header's domain.
    QString fromDomain;
    if (ctx->m_message) {
        if (const auto *from = std::as_const(*ctx->m_message).from();
            from && !from->mailboxes().isEmpty()) {
            const QString addr = QString::fromLatin1(from->mailboxes().first().address());
            fromDomain = addr.section(QLatin1Char('@'), 1).toLower();
        }
    }

    // Which path produced these bytes is the single most useful fact when a
    // body hash fails: a message straight off the wire and the same message
    // rebuilt from the cache are not the same byte sequence.
    qCDebug(logTrace) << "dkim: verifying uid" << ctx->m_uid
                      << "source" << (m_presentingFromCache ? "cache" : "network")
                      << "bytes" << wire.size();

    // Remembered per submission, not read back from m_presentingFromCache when
    // the verdict lands: the user may have opened something else by then, and
    // how a mismatch must be reported depends on where *these* bytes came from.
    ctx->m_dkimFromCache = m_presentingFromCache;

    const quint64 requestId = ++m_dkimNextRequest;
    m_dkimPending.insert(requestId, ctx);
    ctx->m_dkimChecking = true;
    Q_EMIT ctx->dkimChanged();

    QMetaObject::invokeMethod(m_dkimVerifier, "verify", Qt::QueuedConnection,
                              Q_ARG(quint64, requestId), Q_ARG(QByteArray, wire),
                              Q_ARG(QString, fromDomain));
}

bool MailClient::scheduleDkimRetry(MessageContext *ctx)
{
    static constexpr int delaysMs[] = {15000, 60000, 180000};
    static constexpr int maxAttempts = int(std::size(delaysMs));
    if (ctx->m_dkimAttempt >= maxAttempts)
        return false;
    const int delay = delaysMs[ctx->m_dkimAttempt];
    ++ctx->m_dkimAttempt;

    // Pin the message this retry belongs to. The user may open something else
    // in the meantime, and a verdict for the old message must never be
    // attached to the new one.
    const QPointer<MessageContext> guard(ctx);
    const qint64 uid = ctx->m_uid;
    QTimer::singleShot(delay, this, [this, guard, uid] {
        if (!guard || !guard->m_hasMessage || guard->m_uid != uid)
            return;
        submitDkimVerification(guard);
    });
    return true;
}

bool MailClient::healCachedBody(MessageContext *ctx, HealReason reason)
{
    if (!m_connected || !m_session || ctx->m_uid < 0 || ctx->m_folder.isEmpty())
        return false;

    const QString key = ctx->m_folder + QLatin1Char('\n') + QString::number(ctx->m_uid);
    if (m_dkimHealed.contains(key))
        return false; // already refetched once; the mismatch is the message's

    // A message whose attachments were lifted into the file store is cached as
    // a stub and put back together on open, and that reassembly re-encodes the
    // MIME parts — so the refetched octets must not be written back, or the
    // next open would mismatch again. The verdict is what gets kept instead.
    const bool externalized = !m_store.partsFor(ctx->m_folder, ctx->m_uid).isEmpty();

    const QString folder = ctx->m_folder;
    const qint64 uid = ctx->m_uid;
    qCDebug(logTrace) << "dkim: body mismatch from cache, refetching uid" << uid
                      << "in" << folder;

    auto *fetch = new KIMAP::FetchJob(m_session);
    fetch->setSequenceSet(KIMAP::ImapSet(uid));
    fetch->setUidBased(true);
    KIMAP::FetchJob::FetchScope scope;
    scope.mode = KIMAP::FetchJob::FetchScope::Full;
    fetch->setScope(scope);

    // Same accumulate-then-decide shape as openMessage(): a server may split
    // one message's attributes across several untagged responses.
    auto found = std::make_shared<std::shared_ptr<KMime::Message>>();
    connect(fetch, &KIMAP::FetchJob::messagesAvailable, this,
            [found](const QMap<qint64, KIMAP::Message> &messages) {
                for (const KIMAP::Message &m : messages) {
                    if (!m.message)
                        continue;
                    const bool hasContent =
                        !m.message->body().isEmpty() || !m.message->contents().isEmpty();
                    if (!*found || hasContent)
                        *found = m.message;
                }
            });

    const QPointer<MessageContext> guard(ctx);
    const QString healKey = key;
    connect(fetch, &KJob::result, this,
            [this, guard, found, folder, uid, healKey, externalized, reason](KJob *job) {
        if (!guard || !guard->m_hasMessage || guard->m_uid != uid)
            return; // the user moved on; a verdict now would land on the wrong message
        if (job->error() || !*found) {
            // Could not get a second opinion, so do not offer one. Not marked
            // healed either: a refetch that never happened must not stop the
            // next open from trying again.
            if (reason == HealReason::OpenPgpSignature) {
                guard->m_signatureStatus = QStringLiteral("unverified");
                guard->m_cryptoChecking = false;
                Q_EMIT guard->cryptoChanged();
            } else {
                guard->m_dkimStatus = QStringLiteral("unverified");
                guard->m_dkimChecking = false;
                Q_EMIT guard->dkimChanged();
            }
            return;
        }
        // Marked only now, on a refetch that actually delivered — an aborted
        // one would otherwise burn the single attempt this message gets.
        m_dkimHealed.insert(healKey);
        KMime::Message *msg = found->get();
        if (msg->contents().isEmpty())
            msg->parse();
        // Judge the signature against what the server actually holds. The
        // parsed message on screen is left alone: the rendered body is
        // unchanged, and re-presenting it would flicker for no gain.
        guard->m_raw = msg->encodedContent();
        if (!externalized) {
            // Heal the cache too, so this message is right from now on.
            m_store.storeBody(folder, uid, guard->m_raw, indexTextFor(msg));
        }
        if (reason == HealReason::OpenPgpSignature) {
            // The signature has to be re-checked against the tree these bytes
            // parse into, not the one on screen: the octets are sliced out of
            // the message the boundary belongs to.
            guard->m_message = *found;
            guard->m_pgpFromCache = false;
            if (!startPgpVerification(guard, msg)) {
                guard->m_signatureStatus = QStringLiteral("unverified");
                guard->m_cryptoChecking = false;
                Q_EMIT guard->cryptoChanged();
            }
            return;
        }
        // submitDkimVerification records these as network-origin, which is
        // what lets a second mismatch be reported as a real failure — and what
        // makes the resulting verdict worth storing.
        submitDkimVerification(guard);
    });
    fetch->start();

    // The badge stays in its "checking" state across the refetch — the check
    // has not finished, it has been handed a better copy to work from.
    if (reason == HealReason::OpenPgpSignature) {
        ctx->m_cryptoChecking = true;
        Q_EMIT ctx->cryptoChanged();
    } else {
        ctx->m_dkimChecking = true;
        Q_EMIT ctx->dkimChanged();
    }
    return true;
}

void MailClient::applyDkimResult(quint64 requestId, const DkimResult &result)
{
    const QPointer<MessageContext> ctx = m_dkimPending.take(requestId);
    if (!ctx)
        return; // the window closed while the check was in flight

    bool retrying = false;
    switch (result.status) {
    case DkimResult::None:
        // No signature at all. Plenty of legitimate mail is unsigned, so this
        // stays distinct from a failure and shows nothing.
        ctx->m_dkimStatus = QStringLiteral("none");
        break;
    case DkimResult::Pass:
        ctx->m_dkimStatus = QStringLiteral("pass");
        break;
    case DkimResult::Fail:
        // The one case that earns an accusation: we fetched the key the
        // signature names and the signature does not match it.
        ctx->m_dkimStatus = QStringLiteral("fail");
        break;
    case DkimResult::PermError:
        // A signature we cannot evaluate — most often "no public key published
        // for <selector>", which simply means the signer has rotated the key
        // out of DNS since the message arrived. That is the normal fate of any
        // archived mail, and it is why a receiving server's dkim=pass from
        // months ago can sit next to our own inability to check it today.
        // Also covers a revoked or unusable key and a malformed signature; the
        // tooltip carries which. Not an accusation either way: we did not
        // establish that the signature is bad, only that we cannot tell.
        ctx->m_dkimStatus = QStringLiteral("permerror");
        break;
    case DkimResult::TempError:
        ctx->m_dkimStatus = QStringLiteral("temperror");
        retrying = scheduleDkimRetry(ctx);
        break;
    case DkimResult::BodyMismatch:
        // The body hash did not match. What that means depends entirely on
        // whether the bytes we hashed are the bytes that arrived.
        if (ctx->m_dkimFromCache) {
            // They came out of the offline cache, which for bodies written by
            // older builds is not the original octets. Refetch and re-verify
            // before saying anything; healCachedBody() leaves the check
            // running when it takes over.
            if (healCachedBody(ctx, HealReason::DkimBodyHash))
                return;
            // Nothing to heal with (offline, or already tried) — say only what
            // we know, which is that we could not verify it.
            ctx->m_dkimStatus = QStringLiteral("unverified");
        } else {
            // These bytes came straight off the wire, and the fetch path is
            // known to preserve them (doc/roadmap.md), so the body really is
            // not the one that was signed. That is emphatically NOT a failed
            // signature: it is the everyday outcome of a mailing list or a
            // forwarder appending a footer or rewriting the subject, and it is
            // the exact situation ARC exists to describe. Calling it "invalid"
            // would cry wolf on most list mail — the same mistake the SPF/DKIM
            // column already makes (doc/roadmap.md). Its own status, so the
            // viewer can say what happened instead of accusing.
            ctx->m_dkimStatus = QStringLiteral("modified");
        }
        break;
    case DkimResult::Unsupported:
        // An obsolete algorithm is not a broken signature — it is one we
        // declined to give an opinion on. Saying "invalid" would be a claim we
        // did not earn.
        ctx->m_dkimStatus = QStringLiteral("unsupported");
        break;
    }
    qCDebug(logTrace) << "dkim: uid" << ctx->m_uid << "verdict" << ctx->m_dkimStatus
                      << "d=" << result.domain << "aligned" << result.aligned
                      << "-" << result.detail;
    ctx->m_dkimDetail = result.detail;

    // ARC is only run when DKIM could not settle the question, so an empty
    // status here means "not asked", not "no chain".
    switch (result.arc.status) {
    case ArcResult::None:
        ctx->m_arcStatus = QStringLiteral("none");
        break;
    case ArcResult::Pass:
        ctx->m_arcStatus = QStringLiteral("pass");
        break;
    case ArcResult::SealsOnly:
        ctx->m_arcStatus = QStringLiteral("sealsonly");
        break;
    case ArcResult::Fail:
        ctx->m_arcStatus = QStringLiteral("fail");
        break;
    case ArcResult::TempError:
    case ArcResult::PermError:
        // Nothing was established either way; the reason is in the tooltip.
        ctx->m_arcStatus = QStringLiteral("error");
        break;
    }
    ctx->m_arcSealer = result.arc.sealer;
    ctx->m_arcDetail = result.arc.detail;
    if (result.arc.status != ArcResult::None) {
        qCDebug(logTrace) << "arc: uid" << ctx->m_uid << "status" << ctx->m_arcStatus << "sealer"
                          << result.arc.sealer << "hops" << result.arc.sets;
    }
    ctx->m_dkimTrusted = result.trustworthy();
    // Still "checking" while a retry is pending — we have not given up yet.
    ctx->m_dkimChecking = retrying;
    Q_EMIT ctx->dkimChanged();

    // Record it, so the message is never re-verified — but only a verdict that
    // is both settled and earned. "temperror" is unsettled by definition, and
    // "unverified" says our copy was doubtful, which is not a fact about the
    // message. A verdict read off cached bytes is only trusted when it passed:
    // corruption produces a mismatch, never a valid signature, so a pass
    // cannot be an artefact of a stale copy the way a failure can.
    const bool settled = !retrying && ctx->m_dkimStatus != QLatin1String("temperror")
        && ctx->m_dkimStatus != QLatin1String("unverified")
        && !ctx->m_dkimStatus.isEmpty();
    // "permerror" is recorded even off cached bytes: "no key published" is a
    // fact about DNS, not about our copy of the message, and it is not an
    // accusation — which is what this rule exists to guard against. Without it
    // every open of an archived message whose key has rotated away would spend
    // another DNS query to reach the same answer.
    const bool earned = !ctx->m_dkimFromCache || ctx->m_dkimStatus == QLatin1String("pass")
        || ctx->m_dkimStatus == QLatin1String("permerror");
    if (settled && earned && ctx->m_uid >= 0 && !ctx->m_folder.isEmpty()) {
        m_store.storeAuthVerdict(ctx->m_folder, ctx->m_uid,
                                 {ctx->m_dkimStatus, ctx->m_dkimDetail, ctx->m_dkimTrusted,
                                  ctx->m_arcStatus, ctx->m_arcSealer, ctx->m_arcDetail});
    }
}

void MailClient::setPgpEngine(PgpEngine *engine)
{
    m_pgp = engine;
    if (!engine)
        return;
    connect(engine, &PgpEngine::decryptFinished, this, &MailClient::applyDecryption);
    connect(engine, &PgpEngine::verifyFinished, this, &MailClient::applyVerification);
    connect(engine, &PgpEngine::decryptRecipient, this,
            [this](quint64 jobId, const QString &keyId) {
                const auto it = m_pendingDecrypt.constFind(jobId);
                if (it == m_pendingDecrypt.cend())
                    return;
                // gpg names the encryption *subkey* here. Resolved to the
                // primary fingerprint, because that is the identity everything
                // else (key manager, keyInfo) speaks; the raw subkey ID kept
                // as the fallback when the keyring cannot resolve it.
                const QString primary = m_pgp->primaryFingerprintFor(keyId);
                for (const QPointer<MessageContext> &ctx : it.value()) {
                    if (ctx)
                        ctx->m_decryptionKeyId = primary.isEmpty() ? keyId : primary;
                }
            });
}

bool MailClient::startPgpVerification(MessageContext *ctx, KMime::Message *root)
{
    if (!m_pgp || !m_pgp->available() || !root)
        return false;
    const PgpMime::Structure s = PgpMime::classify(root);
    if (s.kind != PgpMime::Kind::Signed)
        return false;

    // The signed part is sliced out of the bytes the tree was parsed from,
    // never rebuilt from the tree itself: rebuilding can refold headers, and a
    // signature checked against rebuilt octets says nothing when it fails.
    //
    // Both trees have such bytes. For the message as it arrived that is m_raw;
    // for a decrypted one it is the plaintext gpg handed back, which is just as
    // original — it is what the sender signed before encrypting it. Getting
    // this wrong is not harmless: verifying a signed-and-encrypted message
    // against rebuilt octets, and calling them exact, reports every good
    // signature as "modified after signing".
    const bool fromArrived = root == ctx->m_message.get();
    const QByteArray raw = fromArrived ? ctx->m_raw
        : (root == ctx->m_decrypted.get() ? ctx->m_decryptedRaw : QByteArray());
    // Remembered per submission, like m_dkimFromCache: by the time the verdict
    // lands the user may have opened something else, and whether a mismatch
    // means anything depends on where *these* bytes came from. Only the arrived
    // copy can be stale — a decryption happened just now, in this session.
    ctx->m_pgpFromCache = m_presentingFromCache && fromArrived;
    bool exact = false;
    const QByteArray octets = raw.isEmpty() ? PgpMime::signedOctets(s)
                                            : PgpMime::signedOctets(raw, s, &exact);
    ctx->m_pgpOctetsExact = exact;

    const quint64 job = m_pgp->verifyDetached(octets, PgpMime::signature(s));
    if (!job)
        return false;
    ctx->m_verifyJob = job;
    m_pendingVerify[job].append(ctx);
    return true;
}

/// Records one signature verdict on the message it belongs to.
///
/// Reached two ways: a detached RFC 3156 signature we asked about, and a
/// signature gpg found inside a message it was decrypting — the latter arrives
/// under the decrypt job's own token, which is why both maps are consulted.
void MailClient::applyVerification(quint64 jobId, const PgpSignatureInfo &signature)
{
    QList<QPointer<MessageContext>> waiting = m_pendingVerify.take(jobId);
    // A signature inside an encrypted message: the contexts waiting on that
    // decryption are the ones this verdict is about.
    const auto pending = m_pendingDecrypt.constFind(jobId);
    if (pending != m_pendingDecrypt.cend())
        waiting += pending.value();
    if (waiting.isEmpty())
        return;

    for (const QPointer<MessageContext> &ptr : std::as_const(waiting)) {
        MessageContext *ctx = ptr;
        if (!ctx || !ctx->m_hasMessage)
            continue;

        ctx->m_verifyJob = 0;
        switch (signature.status) {
        case PgpSignatureInfo::Valid:
            ctx->m_signatureStatus = QStringLiteral("valid");
            break;
        case PgpSignatureInfo::NotVerified:
            // The same question DKIM asks of a body-hash mismatch, for the same
            // reason (doc/roadmap.md): are these the bytes that were signed?
            if (ctx->m_pgpFromCache) {
                // They came out of the cache, so no. Either the body predates
                // the fetch path that preserves octets, or it was reassembled
                // from externalised attachments — which is also the case where
                // the octets could not be sliced exactly, and so the case a
                // refetch helps most. Refetch once and ask again before saying
                // anything; healing leaves the check running when it takes
                // over.
                if (healCachedBody(ctx, HealReason::OpenPgpSignature))
                    continue;
                // Offline, or already refetched once. Say only what we know.
                ctx->m_signatureStatus = QStringLiteral("unverified");
            } else if (!ctx->m_pgpOctetsExact) {
                // Off the wire, but the signed part could not be sliced out of
                // it — a malformed boundary, or a tree gpg produced itself.
                // Nothing about the message follows from a mismatch here.
                ctx->m_signatureStatus = QStringLiteral("unverified");
            } else {
                // Exact octets, straight off the wire. The part that was
                // signed is genuinely not the part that arrived — which is the
                // everyday result of a mailing list rewriting a message, not
                // an accusation. Its own status, so the badge can say what
                // happened rather than imply forgery.
                ctx->m_signatureStatus = QStringLiteral("modified");
            }
            break;
        case PgpSignatureInfo::UnknownKey:
            ctx->m_signatureStatus = QStringLiteral("unknownKey");
            break;
        case PgpSignatureInfo::Expired:
            ctx->m_signatureStatus = QStringLiteral("expired");
            break;
        case PgpSignatureInfo::Revoked:
            ctx->m_signatureStatus = QStringLiteral("revoked");
            break;
        case PgpSignatureInfo::Error:
            ctx->m_signatureStatus = QStringLiteral("error");
            break;
        case PgpSignatureInfo::None:
            ctx->m_signatureStatus.clear();
            break;
        }
        ctx->m_signerName = signature.signerName;
        ctx->m_signerEmail = signature.signerEmail;
        ctx->m_signerFingerprint = signature.fingerprint;
        // Alignment, not just validity: a good signature from a key that has
        // nothing to do with the From address is precisely what a forger's own
        // key produces. Only the two together earn a name on the badge.
        ctx->m_signerTrusted = signature.status == PgpSignatureInfo::Valid
            && !signature.signerEmail.isEmpty()
            && signature.signerEmail.compare(ctx->m_senderAddress, Qt::CaseInsensitive) == 0;
        if (!signature.detail.isEmpty())
            ctx->m_cryptoDetail = signature.detail;
        if (ctx->m_signerTrusted) {
            // Nothing else here says the sender is who they claim, so say it
            // once, plainly, where the reader is already looking.
            ctx->m_cryptoDetail =
                tr("Signed by %1, whose key matches the From address.")
                    .arg(signature.signerEmail);
        } else if (signature.status == PgpSignatureInfo::Valid) {
            ctx->m_cryptoDetail =
                tr("Good signature, but the signing key belongs to %1 rather "
                   "than the sender. That is what a forged message signed with "
                   "the forger's own key looks like.")
                    .arg(signature.signerEmail.isEmpty() ? tr("an unknown address")
                                                         : signature.signerEmail);
        }

        // "signed and encrypted" is a stronger statement than either alone, so
        // it gets its own state rather than overwriting one with the other.
        if (ctx->m_cryptoState == QLatin1String("encrypted"))
            ctx->m_cryptoState = QStringLiteral("signedEncrypted");
        else if (ctx->m_cryptoState.isEmpty()
                 || ctx->m_cryptoState == QLatin1String("signed"))
            ctx->m_cryptoState = QStringLiteral("signed");
        ctx->m_cryptoChecking = ctx->m_decryptJob != 0;

        // The list's mark can be refined now: the head only shows the outer
        // type, so an encrypted message that turns out to be signed as well
        // only becomes "both" here.
        //
        // The model only, never the store. That a message is *also* signed was
        // learned by decrypting it, and nothing learned that way goes into the
        // plaintext cache — not the body, not the index, and not a metadata
        // bit about what was inside (doc/openpgp.md §4). It is a mark on
        // screen for as long as the message is open, and it is gone on the
        // next start, which is the correct trade.
        if (ctx->m_uid >= 0 && ctx->m_cryptoState == QLatin1String("signedEncrypted")
            && ctx->m_folder == m_selectedFolder) {
            m_messageModel.setCrypto(ctx->m_uid, PgpMime::StoredBoth);
        }
        Q_EMIT ctx->cryptoChanged();
    }
}

/// Takes the result of one decrypt job and, if the message it was for is still
/// open, re-renders that message from the decrypted tree.
///
/// The plaintext stops here: it goes into the context's own m_decrypted and
/// nowhere else. Nothing on this path writes to the cache, the search index or
/// the attachment store — those all read m_raw and m_message, which are still
/// the ciphertext exactly as it arrived (doc/openpgp.md §4).
void MailClient::applyDecryption(quint64 jobId, const QByteArray &plainText,
                                 const QString &error, bool noSecretKey)
{
    const QList<QPointer<MessageContext>> waiting = m_pendingDecrypt.take(jobId);
    if (waiting.isEmpty())
        return;

    // Parsed once and shared: the reading pane and any window detached from it
    // are showing the same message, and a second parse would be a second copy
    // of the plaintext in memory.
    std::shared_ptr<KMime::Message> inner;
    if (error.isEmpty() && !plainText.isEmpty()) {
        inner = std::make_shared<KMime::Message>();
        // Inline PGP encrypts the sender's text, not a MIME entity, so what
        // comes back is a paragraph rather than a message. Parsed as one it
        // yields an entity whose first line was taken for a header and whose
        // body is empty — a good decryption that displays nothing. Give it the
        // one header it needs instead.
        if (PgpMime::looksLikeMimeEntity(plainText)) {
            inner->setContent(KMime::CRLFtoLF(plainText));
        } else {
            inner->setContent(QByteArrayLiteral("Content-Type: text/plain; "
                                                "charset=\"utf-8\"\n\n")
                              + KMime::CRLFtoLF(plainText));
        }
        inner->parse();
    }

    for (const QPointer<MessageContext> &ptr : waiting) {
        MessageContext *ctx = ptr;
        // The context is gone (a detached window closed), or the reader moved
        // on and this answer is for a message no longer on screen.
        if (!ctx || ctx->m_decryptJob != jobId)
            continue;

        ctx->m_decryptJob = 0;
        ctx->m_cryptoChecking = false;

        if (!inner) {
            ctx->m_cryptoState = QStringLiteral("failed");
            // "No key of yours" is the ordinary case — mail encrypted to an
            // address on another of the user's devices, or to a key they have
            // since retired. It is not a fault to report as a broken message.
            ctx->m_cryptoDetail = noSecretKey
                ? tr("This message is encrypted to a key you do not have.")
                : (error.isEmpty() ? tr("This message could not be decrypted.") : error);
            applyBodyParts(ctx, ctx->m_message.get(), ctx->m_junk);
        } else {
            ctx->m_decrypted = inner;
            // Kept so a signature inside can be checked against these octets
            // rather than against a re-serialisation of the tree they parse to.
            ctx->m_decryptedRaw = plainText;
            // From here until the message is closed there is plaintext in this
            // process, and it must not reach a core file.
            ctx->markPlaintextHeld(true);
            ctx->m_cryptoState = QStringLiteral("encrypted");
            // Which of the reader's keys opened it belongs here, in the
            // tooltip, and not in the badge's click target — that one is about
            // the sender. decryptRecipient() lands during the decryption, so
            // the id is already known by the time the job reports back.
            ctx->m_cryptoDetail = ctx->m_decryptionKeyId.isEmpty()
                ? tr("Decrypted with one of your OpenPGP keys. "
                     "The subject line was never encrypted.")
                : tr("Decrypted with your key %1. "
                     "The subject line was never encrypted.")
                      .arg(ctx->m_decryptionKeyId);
            applyBodyParts(ctx, inner.get(), ctx->m_junk);

            // Until it was decrypted there was no way to know whether this
            // message carried attachments — the head only says
            // multipart/encrypted, and the parts are inside the ciphertext. Now
            // there is, so the list can show the paperclip beside the lock.
            //
            // The model only, never the store: "this encrypted message has
            // attachments" is a fact derived from plaintext, and the cache is
            // the one place §4 keeps plaintext-derived data out of. It costs a
            // paperclip that reappears per session instead of persisting, which
            // is the right side of that trade.
            if (ctx->m_uid >= 0 && ctx->m_folder == m_selectedFolder
                && !inner->attachments().isEmpty()) {
                m_messageModel.setAttachKind(ctx->m_uid,
                                             MessageListModel::GenericAttachment);
            }

            // The usual shape of signed-and-encrypted mail is a multipart/signed
            // *inside* the ciphertext, which only becomes visible now. (A
            // message signed in the same OpenPGP operation instead reports its
            // signature through the decrypt job's own token — see
            // applyVerification.)
            if (startPgpVerification(ctx, inner.get()))
                ctx->m_cryptoChecking = true;
        }
        Q_EMIT ctx->messageChanged();
        Q_EMIT ctx->cryptoChanged();
    }
}

/// Notices a public key attached to a message and offers it to the reader —
/// the fourth of the key sources in doc/openpgp.md §7, and the only one that
/// arrives unasked.
///
/// Nothing is imported here. A key attached to a message is a claim about an
/// identity made by whoever sent the message, which is exactly the claim a
/// forger would make; the reader gets a button, not a fait accompli.
void MailClient::findAttachedKey(MessageContext *ctx, KMime::Content *root)
{
    const auto parts = root->attachments();
    for (KMime::Content *part : parts) {
        const auto *ct = std::as_const(*part).contentType();
        const QByteArray mime = ct ? ct->mimeType().toLower() : QByteArray();
        QString name;
        if (const auto *cd = std::as_const(*part).contentDisposition())
            name = cd->filename();
        if (name.isEmpty() && ct)
            name = ct->name();

        // The declared type, or an .asc/.gpg file whose contents say so. Many
        // clients attach keys as application/octet-stream.
        const bool declared = mime == "application/pgp-keys";
        const bool looksLikeKey = name.endsWith(QLatin1String(".asc"), Qt::CaseInsensitive)
            || name.endsWith(QLatin1String(".gpg"), Qt::CaseInsensitive)
            || name.endsWith(QLatin1String(".pgp"), Qt::CaseInsensitive);
        if (!declared && !looksLikeKey)
            continue;

        const QByteArray body = part->decodedBody();
        // A signature file also ends in .asc, and importing one does nothing;
        // only a key block is offered.
        if (!body.contains("BEGIN PGP PUBLIC KEY BLOCK") && !declared)
            continue;
        // Keys are small. Anything this size is not one, and refusing it here
        // keeps a hostile attachment out of gpg entirely.
        if (body.isEmpty() || body.size() > 1024 * 1024)
            continue;

        ctx->m_attachedKeyData = body;
        ctx->m_attachedKeyName = name.isEmpty() ? tr("public key") : name;
        return;
    }
}

/// Fills in everything the viewer renders from one MIME tree: the body parts,
/// the instant preview, the inline images and the attachment list.
///
/// Split out of presentMessage() because it runs twice for an encrypted
/// message — once over the ciphertext (which has nothing to show), and again
/// over the decrypted tree when gpg answers. \a root is the tree to read;
/// ctx->m_message stays the message as it arrived either way.
void MailClient::applyBodyParts(MessageContext *ctx, KMime::Message *root, bool junk)
{
    const bool encrypted = ctx->m_cryptoState == QLatin1String("encrypted")
        || ctx->m_cryptoState == QLatin1String("decrypting")
        || ctx->m_cryptoState == QLatin1String("failed");
    // Nothing readable yet: the parts are ciphertext, and searching them for a
    // text/html would only find nothing and warn about it.
    const bool opaque = encrypted && !ctx->m_decrypted;

    KMime::Content *htmlPart = nullptr;
    KMime::Content *textPart = nullptr;
    if (!opaque) {
        htmlPart = root->mainBodyPart("text/html");
        if (!htmlPart)
            htmlPart = findPartByType(root, "text/html");
        textPart = root->mainBodyPart("text/plain");
        if (!textPart)
            textPart = findPartByType(root, "text/plain");
        if (!textPart && !htmlPart)
            textPart = root->textContent();
    }

    ctx->m_htmlBody = htmlPart ? htmlPart->decodedText() : QString();
    ctx->m_textBody = textPart ? textPart->decodedText() : QString();

    if (opaque) {
        // The viewer needs something to show while gpg works, or in place of a
        // message it could not open. This is the only body it gets — the
        // ciphertext is not put on screen as text.
        ctx->m_textBody = ctx->m_cryptoDetail.isEmpty()
            ? tr("Encrypted message")
            : ctx->m_cryptoDetail;
    } else if (ctx->m_htmlBody.isEmpty() && ctx->m_textBody.isEmpty()) {
        const auto *ct = std::as_const(*root).contentType();
        qWarning() << "mailo: no displayable part found. content-type:"
                   << (ct ? ct->mimeType() : QByteArrayLiteral("(none)"))
                   << "children:" << root->contents().size()
                   << "raw size:" << ctx->m_raw.size();
    }

    // Plain-text stand-in shown while Chromium renders the HTML view. Never
    // decrypted text: this string also reaches the message list, and the whole
    // point of §4 is that no plaintext leaves the viewer.
    if (encrypted) {
        m_textPreview = tr("Encrypted message");
    } else if (!ctx->m_textBody.isEmpty()) {
        m_textPreview = ctx->m_textBody;
    } else {
        // Cap the input: stripping hundreds of KB of HTML would defeat the
        // purpose of an *instant* preview.
        m_textPreview =
            QTextDocumentFragment::fromHtml(ctx->m_htmlBody.left(100000)).toPlainText();
    }
    Q_EMIT textPreviewChanged();

    if (m_viewerHandler) {
        m_viewerHandler->clearInlineParts(ctx->viewerContext());
        if (!opaque)
            collectInlineParts(ctx, root);
    }
    ctx->m_attachedKeyName.clear();
    ctx->m_attachedKeyData.clear();
    if (opaque) {
        ctx->m_attachmentParts.clear();
        ctx->m_attachments.clear();
    } else {
        // For a decrypted message these parts live in ctx->m_decrypted, in
        // memory. They reach the disk only when the user saves one by hand —
        // the attachment store never sees them (doc/openpgp.md §4).
        collectAttachments(ctx, root);
        findAttachedKey(ctx, root);
    }

    ctx->m_bodyUrl = (htmlPart && !junk) ? htmlViewUrlFor(ctx) : textViewUrlFor(ctx);
}

void MailClient::presentMessage(const std::shared_ptr<KMime::Message> &message)
{
    KMime::Message *msg = message.get();
    // KIMAP delivers the message already parsed. Calling parse() again on a
    // parsed multipart message DESTROYS it: the body was consumed into the
    // child parts, so a re-parse from the now-empty body drops every part
    // and leaves a headers-only shell. Parse only if it hasn't happened yet.
    if (msg->contents().isEmpty())
        msg->parse();

    MessageContext *ctx = m_reading;
    ctx->m_handler = m_viewerHandler;

    // What shape of OpenPGP this is, before anything else looks at the tree:
    // both the remote-content decision below and the part search further down
    // depend on the answer.
    const PgpMime::Structure pgp = PgpMime::classify(msg);

    // Privacy default: remote content blocked, unless the user previously
    // chose "load remote content" for this exact sender address. Must run
    // AFTER the parse guard — cache-served messages have no headers before it.
    ctx->m_senderAddress.clear();
    if (const auto *from = std::as_const(*msg).from(); from && !from->mailboxes().isEmpty())
        ctx->m_senderAddress =
            QString::fromLatin1(from->mailboxes().first().address()).toLower();
    const bool remembered = m_store.remoteContentAllowedFor(ctx->m_senderAddress);
    qCDebug(logTrace) << "presenting message from" << ctx->m_senderAddress
                      << "remembered remote-content" << remembered;
    // Junk gets hostile-content handling: no remembered remote-content
    // allowance either — the user can still toggle it per view.
    const bool junk = isJunkFolder(m_selectedFolder);
    // Nor does an encrypted message inherit one. A remote request made from
    // decrypted mail can carry the plaintext to whoever chose the URL, and a
    // per-sender allowance granted for their ordinary mail was never a
    // decision about that (doc/openpgp.md §5). The toggle still works; it just
    // has to be a fresh, deliberate choice, and the viewer says why.
    ctx->applyRemoteAllowed(remembered && !junk && !pgp.isEncrypted());
    ctx->m_junk = junk;
    ctx->m_folder = m_selectedFolder;
    // A message in the Sent folder means its To/Cc were once our recipients —
    // feed them to the compose autocompletion.
    if (!m_sentFolder.isEmpty() && m_selectedFolder == m_sentFolder)
        harvestRecipients(msg);
    ctx->m_message = message; // keeps all parts alive
    ctx->m_raw = msg->encodedContent();
    ctx->m_decrypted.reset();
    ctx->m_decryptJob = 0;
    ctx->m_cryptoState.clear();
    ctx->m_cryptoDetail.clear();
    ctx->m_cryptoChecking = false;

    // For an encrypted message the parts worth showing are not in this tree at
    // all, so this has to settle before the part search below.
    if (pgp.kind == PgpMime::Kind::Partial) {
        ctx->m_cryptoState = QStringLiteral("partial");
        ctx->m_cryptoDetail =
            tr("Part of this message is encrypted. Mailo does not decrypt a "
               "fragment of a message: shown together with the parts that were "
               "never encrypted, there would be no way to tell which was which.");
    } else if (pgp.isEncrypted()) {
        // Remote content is not inherited for these: see the applyRemoteAllowed
        // call above and doc/openpgp.md §5.
        ctx->m_cryptoState = QStringLiteral("failed"); // until a job starts
        if (!m_pgp || !m_pgp->available()) {
            ctx->m_cryptoDetail = m_pgp ? m_pgp->unavailableReason()
                                        : tr("OpenPGP support is not available.");
        } else if (const quint64 job = m_pgp->decrypt(PgpMime::ciphertext(pgp))) {
            ctx->m_cryptoState = QStringLiteral("decrypting");
            ctx->m_decryptJob = job;
            ctx->m_cryptoChecking = true;
            ctx->m_cryptoDetail = tr("Decrypting…");
            m_pendingDecrypt[job].append(ctx);
        } else {
            ctx->m_cryptoDetail = tr("This message could not be decrypted.");
        }
    } else if (pgp.kind == PgpMime::Kind::Signed) {
        // Nothing is hidden here — the message reads either way — so it is
        // presented at once and the verdict lands on the badge when it lands.
        ctx->m_cryptoState = QStringLiteral("signed");
        if (startPgpVerification(ctx, msg)) {
            ctx->m_cryptoChecking = true;
            ctx->m_cryptoDetail = tr("Checking the signature…");
        } else {
            ctx->m_signatureStatus = QStringLiteral("error");
            ctx->m_cryptoDetail = m_pgp && !m_pgp->available()
                ? m_pgp->unavailableReason()
                : tr("The signature could not be checked.");
        }
    }

    // Body, preview, inline parts and attachments — and the view URL, which
    // applyBodyParts picks (junk folders open as plain text, always).
    applyBodyParts(ctx, msg, junk);
    const QString bodyUrl = ctx->m_bodyUrl;

    const KMime::Message *cmsg = msg;
    const QString subject = cmsg->subject() ? cmsg->subject()->asUnicodeString() : QString();
    const QString from = cmsg->from() ? cmsg->from()->asUnicodeString() : QString();
    const QString to = cmsg->to() ? cmsg->to()->asUnicodeString() : QString();
    const QString cc = cmsg->cc() ? cmsg->cc()->asUnicodeString() : QString();
    QString date;
    if (cmsg->date()) {
        const QDateTime local = cmsg->date()->dateTime().toLocalTime();
        date = local.date() == QDate::currentDate()
            ? local.toString(QStringLiteral("hh:mm"))
            : local.toString(m_dateFormat + QStringLiteral(" hh:mm"));
    }
    // Imported archive mail gets no authentication verdicts at all: there is
    // no receiving server whose Authentication-Results could be trusted, and
    // years-old DKIM keys are long rotated — every check would just fail.
    const QString authInfo =
        m_local ? QString() : trustedAuthResults(cmsg, trustedAuthDomains());

    ctx->m_subject = subject;
    ctx->m_from = from;
    ctx->m_to = to;
    ctx->m_cc = cc;
    ctx->m_date = date;
    ctx->m_authInfo = authInfo;
    ctx->m_bodyUrl = bodyUrl;
    ctx->m_hasMessage = true;
    Q_EMIT ctx->messageChanged();
    Q_EMIT ctx->cryptoChanged();
    Q_EMIT messageLoaded(subject, from, to, cc, date, bodyUrl, authInfo);

    // Verify the signature ourselves. This is the one place it is started:
    // opening a message, not listing or prefetching one.
    startDkimVerification(ctx);

    // A double-click asked for this message in its own window; now that it is
    // presentable, hand a standalone copy to QML. The uid guard drops stale
    // requests — e.g. the user moved on to another message before the fetch
    // for the double-clicked one came back.
    if (m_detachPending) {
        m_detachPending = false;
        if (ctx->m_uid == m_detachUid)
            Q_EMIT messageWindowReady(detachReading());
    }
}

MessageContext *MailClient::detachReading()
{
    MessageContext *src = m_reading;
    auto *ctx = new MessageContext(this);
    ctx->m_handler = m_viewerHandler;
    ctx->m_message = src->m_message; // shared — parts stay alive for both
    ctx->m_attachmentParts = src->m_attachmentParts;
    ctx->m_attachments = src->m_attachments;
    ctx->m_htmlBody = src->m_htmlBody;
    ctx->m_textBody = src->m_textBody;
    ctx->m_raw = src->m_raw;
    ctx->m_uid = src->m_uid;
    // What makes this message *this* message: a uid is only unique within a
    // folder, and a folder name only within an account.
    ctx->m_sourceKey = accountKey() + QLatin1Char('\n') + m_selectedFolder
        + QLatin1Char('\n') + QString::number(src->m_uid);
    ctx->m_senderAddress = src->m_senderAddress;
    ctx->m_junk = src->m_junk;
    ctx->m_folder = src->m_folder;
    ctx->m_remoteAllowed = src->m_remoteAllowed;
    ctx->m_subject = src->m_subject;
    ctx->m_from = src->m_from;
    ctx->m_to = src->m_to;
    ctx->m_cc = src->m_cc;
    ctx->m_date = src->m_date;
    ctx->m_authInfo = src->m_authInfo;
    ctx->m_hasMessage = src->m_hasMessage;
    // Whatever we already know about the signature, so the window opens with
    // the badge the reading pane was showing rather than a blank space.
    ctx->m_dkimStatus = src->m_dkimStatus;
    ctx->m_dkimDetail = src->m_dkimDetail;
    ctx->m_dkimTrusted = src->m_dkimTrusted;
    ctx->m_arcStatus = src->m_arcStatus;
    ctx->m_arcSealer = src->m_arcSealer;
    ctx->m_arcDetail = src->m_arcDetail;
    ctx->m_dkimFromCache = src->m_dkimFromCache;
    // The decrypted tree is shared, not decrypted again: a second decryption
    // would mean a second passphrase prompt for a message already open.
    ctx->m_decrypted = src->m_decrypted;
    ctx->m_decryptedRaw = src->m_decryptedRaw;
    if (ctx->m_decrypted)
        ctx->markPlaintextHeld(true);
    ctx->m_decryptionKeyId = src->m_decryptionKeyId;
    ctx->m_cryptoState = src->m_cryptoState;
    ctx->m_cryptoDetail = src->m_cryptoDetail;
    ctx->m_cryptoChecking = src->m_cryptoChecking;
    ctx->m_decryptJob = src->m_decryptJob;
    ctx->m_signatureStatus = src->m_signatureStatus;
    ctx->m_signerName = src->m_signerName;
    ctx->m_signerEmail = src->m_signerEmail;
    ctx->m_signerFingerprint = src->m_signerFingerprint;
    ctx->m_signerTrusted = src->m_signerTrusted;
    ctx->m_verifyJob = src->m_verifyJob;
    ctx->m_pgpOctetsExact = src->m_pgpOctetsExact;
    ctx->m_pgpFromCache = src->m_pgpFromCache;
    // Same reasoning as the decrypt job below: one verification, two contexts.
    if (ctx->m_verifyJob)
        m_pendingVerify[ctx->m_verifyJob].append(ctx);
    // Own scheme-handler slot: the window keeps its body and inline images
    // however the reading pane moves on.
    if (m_viewerHandler && ctx->m_message)
        collectInlineParts(ctx, ctx->m_decrypted ? ctx->m_decrypted.get()
                                                 : ctx->m_message.get());
    // No HTML part to show — an encrypted message still being decrypted, among
    // others — means the HTML view would open blank.
    ctx->m_bodyUrl = (ctx->m_junk || ctx->m_htmlBody.isEmpty()) ? textViewUrlFor(ctx)
                                                                : htmlViewUrlFor(ctx);
    // A window detached mid-decryption is addressed by the same job id as the
    // pane it came from, so it joins that job's list rather than starting one
    // of its own (unlike DKIM below — re-running gpg would prompt again).
    if (ctx->m_decryptJob)
        m_pendingDecrypt[ctx->m_decryptJob].append(ctx);
    // A detach almost always happens while the reading pane's check is still in
    // flight — the verdict is addressed to that context by request id, so this
    // copy would sit at "checking" forever and show nothing but the server's
    // say-so. Run its own instead: once a verdict has been recorded this is a
    // primary-key lookup, and while one is still being established the second
    // verification reuses the worker's DNS cache, so it costs no query.
    if (src->m_dkimChecking)
        startDkimVerification(ctx);
    return ctx;
}

void MailClient::openMessageInWindow(int row)
{
    const qint64 uid = m_messageModel.uidAt(row);
    if (uid < 0)
        return;
    // The usual case: the first click of the double-click already loaded the
    // message into the reading pane — copy it straight into a window.
    if (m_reading->m_hasMessage && m_reading->m_uid == uid) {
        Q_EMIT messageWindowReady(detachReading());
        return;
    }
    // Still loading (or the click never fetched): ask for the message and
    // open the window when it arrives — see presentMessage().
    m_detachPending = true;
    m_detachUid = uid;
    fetchMessage(row);
}

/// Copies the FTS index into one whose tokenizer folds diacritics, so "ave"
/// finds "ávé". A tokenizer cannot be changed in place, and the copy is as big
/// as the mail behind it, so it runs in slices on a worker with the cursor
/// persisted after every slice: quitting resumes rather than restarts. The
/// window refuses to close while it runs, because the swap at the end is what
/// makes the work count.
void MailClient::startIndexRebuild()
{
    if (m_indexThread || m_reclaiming || !m_store.ftsNeedsRebuild())
        return;
    m_indexCancel.storeRelaxed(0);
    m_indexRebuildActive.storeRelaxed(1);
    m_indexRebuilding = true;
    m_indexPercent = 0;
    Q_EMIT indexRebuildChanged();

    m_indexThread = QThread::create([this] {
        QSqlDatabase db = MailStore::openWorkerConnection(QStringLiteral("mailstore-ftsdia"));
        if (!db.isOpen())
            return;
        bool ok = MailStore::beginFtsRebuild(db);
        const qint64 total = qMax<qint64>(1, MailStore::indexedMessageCount(db));
        qint64 cursor = MailStore::ftsRebuildCursor(db);
        qint64 copied = 0;
        while (ok && !m_indexCancel.loadRelaxed()) {
            const int n = MailStore::copyFtsChunk(db, &cursor, 500);
            if (n < 0) {
                ok = false;
                break;
            }
            if (n == 0) {
                ok = MailStore::finishFtsRebuild(db);
                break;
            }
            copied += n;
            const int percent = int(qMin<qint64>(99, copied * 100 / total));
            QMetaObject::invokeMethod(this, [this, percent] {
                if (percent == m_indexPercent)
                    return;
                m_indexPercent = percent;
                setStatus(tr("Rebuilding the search index — %1%").arg(percent));
                Q_EMIT indexRebuildChanged();
            }, Qt::QueuedConnection);
            // Yield the write lock between slices, exactly as the attachment
            // migration does: the user's own writes must never queue behind us.
            QThread::msleep(25);
        }
        const bool finished = ok && !m_indexCancel.loadRelaxed();
        db.close();
        QSqlDatabase::removeDatabase(QStringLiteral("mailstore-ftsdia"));
        m_indexRebuildActive.storeRelaxed(0);
        QMetaObject::invokeMethod(this, [this, finished] {
            m_indexRebuilding = false;
            m_indexPercent = finished ? 100 : 0;
            Q_EMIT indexRebuildChanged();
            if (finished) {
                // The store's own connection still points at the table that was
                // just swapped out; it reopens with the new one on next start.
                setStatus(tr("Search index rebuilt — accents are ignored from the next start"));
            }
            if (m_quitAfterIndex)
                Q_EMIT closeRequested();
        }, Qt::QueuedConnection);
    });
    connect(m_indexThread, &QThread::finished, this, [this] {
        m_indexThread->deleteLater();
        m_indexThread = nullptr;
    });
    m_indexThread->start(QThread::LowestPriority);
}
