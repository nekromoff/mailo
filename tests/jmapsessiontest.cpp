// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * Checks JmapSession (doc/JMAP_ROADMAP.md phase 1) against recorded session
 * objects — RFC 8620 §2 in full: relative endpoint URLs, URI-template
 * expansion, primary-account selection with and without `primaryAccounts`, the
 * core capability limits, and the two authentication schemes.
 *
 * Entirely offline, and deliberately so: discovery is one GET whose answer is
 * a JSON document, and every decision worth testing is made on those bytes.
 *
 * Hand-written fixtures alone were not enough, though, and the way they failed
 * is the reason `tests/data/cyrus-jmap-session.json` exists: a fixture written
 * from the same misreading as the parser agrees with it. `maxSizeRequest` was
 * spelled `maxSizeRequestObject` in both, so the limit silently read zero and
 * every check passed. Only a *recorded* session object — that one, served by
 * cyrus-jmap-tester (see the roadmap for the container) — carries the server's
 * own key names and catches it.
 *
 * Two diagnostic modes, neither part of the pass/fail run:
 *   jmapsessiontest <session.json> [url-it-was-served-from]
 *   jmapsessiontest --live <host> --bearer <token>
 *   jmapsessiontest --live <host> --basic <user> <password>
 * The second is the only way to exercise discover() itself — the redirect, the
 * Authorization header, the HTTP-to-MailBackend::Error mapping — and is meant
 * for the test container, not for a real account.
 */

#include "../src/jmapsession.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QTextStream>
#include <QUrl>

static int failures = 0;

/// Plain stdout rather than qInfo: a diagnostic tool has to print its findings
/// whatever the ambient QT_LOGGING_RULES say, and the default rules drop
/// qInfo() on the floor.
static QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}

static void check(bool ok, const QString &what)
{
    out() << (ok ? QStringLiteral("  ok   ") : QStringLiteral("  FAIL ")) << what << Qt::endl;
    if (!ok)
        ++failures;
}

static void checkEqual(const QString &actual, const QString &expected, const QString &what)
{
    check(actual == expected, what);
    if (actual != expected)
        out() << "         expected: " << expected << Qt::endl
              << "         actual:   " << actual << Qt::endl;
}

// --- Recorded session objects ----------------------------------------------

/// A hosted server of the Fastmail shape: absolute URLs, both mail and
/// submission, primaryAccounts stated outright.
static const char *kHostedSession = R"json({
  "capabilities": {
    "urn:ietf:params:jmap:core": {
      "maxSizeUpload": 250000000,
      "maxConcurrentUpload": 10,
      "maxSizeRequest": 10000000,
      "maxCallsInRequest": 64,
      "maxObjectsInGet": 500,
      "maxObjectsInSet": 500,
      "maxConcurrentRequests": 10,
      "collationAlgorithms": ["i;ascii-numeric", "i;unicode-casemap"]
    },
    "urn:ietf:params:jmap:mail": {},
    "urn:ietf:params:jmap:submission": { "maxDelayedSend": 44236800 }
  },
  "accounts": {
    "u1a2b3c4": {
      "name": "user@example.com",
      "isPersonal": true,
      "isReadOnly": false,
      "accountCapabilities": {
        "urn:ietf:params:jmap:mail": { "maxMailboxesPerEmail": 1000 },
        "urn:ietf:params:jmap:submission": {}
      }
    }
  },
  "primaryAccounts": {
    "urn:ietf:params:jmap:mail": "u1a2b3c4",
    "urn:ietf:params:jmap:submission": "u1a2b3c4"
  },
  "username": "user@example.com",
  "apiUrl": "https://api.example.com/jmap/api/",
  "downloadUrl": "https://api.example.com/jmap/download/{accountId}/{blobId}/{name}?type={type}",
  "uploadUrl": "https://api.example.com/jmap/upload/{accountId}/",
  "eventSourceUrl": "https://api.example.com/jmap/event/?types={types}&closeafter={closeafter}&ping={ping}",
  "state": "cyrus-0;p-5;vfs-0"
})json";

/// The self-hosted shape: every endpoint relative to the session resource, and
/// no primaryAccounts at all — both of which RFC 8620 permits and Cyrus does.
static const char *kRelativeSession = R"json({
  "capabilities": {
    "urn:ietf:params:jmap:core": { "maxCallsInRequest": 16 },
    "urn:ietf:params:jmap:mail": {}
  },
  "accounts": {
    "shared-archive": {
      "name": "Archive",
      "isPersonal": false,
      "isReadOnly": true,
      "accountCapabilities": { "urn:ietf:params:jmap:mail": {} }
    },
    "cassandane": {
      "name": "cassandane",
      "isPersonal": true,
      "isReadOnly": false,
      "accountCapabilities": { "urn:ietf:params:jmap:mail": {} }
    }
  },
  "primaryAccounts": {},
  "username": "cassandane",
  "apiUrl": "/jmap/",
  "downloadUrl": "/jmap/download/{accountId}/{blobId}/{name}?accept={type}",
  "uploadUrl": "/jmap/upload/{accountId}/",
  "eventSourceUrl": "/jmap/event/{types}/{closeafter}/{ping}",
  "state": "0"
})json";

/// A server whose primaryAccounts names an account it does not list — the
/// answer has to come from the accounts themselves, not from a dangling id.
static const char *kDanglingPrimarySession = R"json({
  "capabilities": { "urn:ietf:params:jmap:mail": {} },
  "accounts": {
    "real-account": {
      "name": "real",
      "isPersonal": true,
      "accountCapabilities": { "urn:ietf:params:jmap:mail": {} }
    }
  },
  "primaryAccounts": { "urn:ietf:params:jmap:mail": "vanished-account" },
  "apiUrl": "https://example.net/jmap/"
})json";

/// Authenticated, but to something that is not mail — a calendar-only server.
static const char *kNoMailSession = R"json({
  "capabilities": { "urn:ietf:params:jmap:calendars": {} },
  "accounts": {
    "cal1": {
      "name": "calendar",
      "isPersonal": true,
      "accountCapabilities": { "urn:ietf:params:jmap:calendars": {} }
    }
  },
  "primaryAccounts": {},
  "apiUrl": "https://example.net/jmap/"
})json";

/// Reads \a path, or returns an empty array and counts a failure.
static QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        check(false, QStringLiteral("cannot read %1: %2").arg(path, file.errorString()));
        return {};
    }
    return file.readAll();
}

/// Prints what JmapSession made of a session object. Not a pass/fail check —
/// the diagnostic half of this tool, for looking at a server the recordings do
/// not cover.
static void printSession(const JmapSession &session, const QUrl &from)
{
    out() << "served from      " << from.toString() << Qt::endl
          << "username         " << session.username() << Qt::endl
          << "state            " << session.state() << Qt::endl
          << "apiUrl           " << session.apiUrl().toString() << Qt::endl
          << "uploadUrl        "
          << session.uploadUrl(session.mailAccountId()).toString() << Qt::endl
          << "eventSourceUrl   "
          << session.eventSourceUrl({QStringLiteral("Email")}, 30).toString() << Qt::endl
          << "mail account     " << session.mailAccountId() << Qt::endl
          << "submits from     "
          << (session.submissionAccountId().isEmpty() ? QStringLiteral("(nothing)")
                                                      : session.submissionAccountId())
          << Qt::endl;

    const JmapSession::Limits limits = session.limits();
    out() << "limits           maxCallsInRequest=" << limits.maxCallsInRequest
          << " maxObjectsInGet=" << limits.maxObjectsInGet
          << " maxObjectsInSet=" << limits.maxObjectsInSet
          << " maxSizeRequest=" << limits.maxSizeRequest
          << " maxSizeUpload=" << limits.maxSizeUpload << Qt::endl;

    const auto accounts = session.accounts();
    for (const JmapSession::Account &account : accounts) {
        out() << "account          " << account.id << "  \"" << account.name << "\""
              << (account.isPersonal ? QStringLiteral(" personal") : QString())
              << (account.isReadOnly ? QStringLiteral(" read-only") : QString())
              << "  " << account.capabilities.size() << " capabilities" << Qt::endl;
    }
}

/// Ingests a capture from disk.
static int describe(const QString &path, const QUrl &from)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        out() << "cannot read " << path << ": " << file.errorString() << Qt::endl;
        return 1;
    }
    JmapSession session;
    QString error;
    if (!session.ingest(file.readAll(), from, &error)) {
        out() << "rejected: " << error << Qt::endl;
        return 1;
    }
    printSession(session, from);
    return 0;
}

/// Runs the real discovery — the one part of JmapSession no recording can
/// cover, since what it exercises is the GET: the .well-known redirect, the
/// Authorization header, and the mapping of an HTTP answer onto
/// MailBackend::Error. Points at the cyrus-jmap-tester container in practice;
/// never at anyone's real account, which is why it takes its credentials on
/// the command line and stores nothing.
static int discoverLive(const QString &host, const QString &token, const QString &user,
                        const QString &password)
{
    MailBackend::Credentials credentials;
    credentials.host = host;
    credentials.accessToken = token;
    credentials.user = user;
    credentials.password = password;

    JmapSession session;
    int result = 1;
    QEventLoop loop;
    QObject::connect(&session, &JmapSession::ready, &loop, [&] {
        printSession(session, JmapSession::wellKnownUrl(credentials));
        result = 0;
        loop.quit();
    });
    QObject::connect(&session, &JmapSession::failed, &loop,
                     [&](MailBackend::Error error, const QString &message) {
                         out() << "discovery failed (" << static_cast<int>(error) << "): "
                               << message << Qt::endl;
                         loop.quit();
                     });

    out() << "GET " << JmapSession::wellKnownUrl(credentials).toString() << Qt::endl;
    session.discover(credentials);
    loop.exec();
    return result;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QStringList args = app.arguments();
    if (args.size() > 2 && args.at(1) == QLatin1String("--live")) {
        // --live <host> --bearer <token> | --live <host> --basic <user> <password>
        const QString host = args.at(2);
        const QString scheme = args.value(3);
        if (scheme == QLatin1String("--bearer"))
            return discoverLive(host, args.value(4), {}, {});
        if (scheme == QLatin1String("--basic"))
            return discoverLive(host, {}, args.value(4), args.value(5));
        return discoverLive(host, {}, {}, {});
    }
    if (args.size() > 1) {
        return describe(args.at(1),
                        args.size() > 2
                            ? QUrl(args.at(2))
                            : QUrl(QStringLiteral("https://localhost/.well-known/jmap")));
    }

    const QUrl hostedFrom(QStringLiteral("https://example.com/.well-known/jmap"));
    const QUrl relativeFrom(QStringLiteral("https://localhost:8080/.well-known/jmap"));

    out() << "a hosted session object" << Qt::endl;
    {
        JmapSession session;
        QString error;
        check(session.ingest(QByteArray(kHostedSession), hostedFrom, &error),
              QStringLiteral("a well-formed session object is accepted"));
        check(error.isEmpty(), QStringLiteral("acceptance reports no error"));
        check(session.isValid(), QStringLiteral("the session is valid once ingested"));
        checkEqual(session.apiUrl().toString(),
                   QStringLiteral("https://api.example.com/jmap/api/"),
                   QStringLiteral("apiUrl is taken verbatim when absolute"));
        checkEqual(session.username(), QStringLiteral("user@example.com"),
                   QStringLiteral("the server's own username is kept"));
        checkEqual(session.state(), QStringLiteral("cyrus-0;p-5;vfs-0"),
                   QStringLiteral("the session state string is kept"));
        checkEqual(session.mailAccountId(), QStringLiteral("u1a2b3c4"),
                   QStringLiteral("primaryAccounts names the mail account"));
        checkEqual(session.submissionAccountId(), QStringLiteral("u1a2b3c4"),
                   QStringLiteral("primaryAccounts names the submission account"));
        check(session.accounts().size() == 1, QStringLiteral("one account is listed"));
        check(session.hasCapability(JmapSession::mailCapability()),
              QStringLiteral("the mail capability is advertised at session level"));
        check(session.hasCapability(JmapSession::submissionCapability()),
              QStringLiteral("the submission capability is advertised"));
        check(!session.hasCapability(QStringLiteral("urn:ietf:params:jmap:calendars")),
              QStringLiteral("a capability the server never named is absent"));
        check(session.accountHasCapability(QStringLiteral("u1a2b3c4"),
                                           JmapSession::mailCapability()),
              QStringLiteral("the account itself offers mail"));
        check(!session.accountHasCapability(QStringLiteral("nonesuch"),
                                            JmapSession::mailCapability()),
              QStringLiteral("an unknown account offers nothing"));

        const JmapSession::Limits limits = session.limits();
        check(limits.maxCallsInRequest == 64,
              QStringLiteral("maxCallsInRequest is read from the core capability"));
        check(limits.maxObjectsInGet == 500,
              QStringLiteral("maxObjectsInGet is read from the core capability"));
        check(limits.maxSizeUpload == 250000000,
              QStringLiteral("maxSizeUpload survives being a JSON double"));
        // RFC 8620 §2 names this maxSizeRequest. Spelling it
        // "maxSizeRequestObject" reads every bit as plausibly and silently
        // yields zero — which is why the recorded Cyrus session below, with
        // the server's own key names, is in this test at all.
        check(limits.maxSizeRequest == 10000000,
              QStringLiteral("maxSizeRequest is read under its RFC 8620 name"));

        out() << "  URI templates" << Qt::endl;
        checkEqual(session.downloadUrl(QStringLiteral("u1a2b3c4"),
                                       QStringLiteral("G12ab"),
                                       QStringLiteral("text/plain"),
                                       QStringLiteral("notes.txt"))
                       .toString(QUrl::FullyEncoded),
                   QStringLiteral("https://api.example.com/jmap/download/u1a2b3c4/G12ab/"
                                  "notes.txt?type=text%2Fplain"),
                   QStringLiteral("downloadUrl expands all four variables"));
        checkEqual(session.downloadUrl(QStringLiteral("u1a2b3c4"),
                                       QStringLiteral("G1/2+3"),
                                       QStringLiteral("application/pdf"),
                                       QStringLiteral("quarterly report.pdf"))
                       .toString(QUrl::FullyEncoded),
                   QStringLiteral("https://api.example.com/jmap/download/u1a2b3c4/G1%2F2%2B3/"
                                  "quarterly%20report.pdf?type=application%2Fpdf"),
                   QStringLiteral("template values are percent-encoded, separators included"));
        checkEqual(session.uploadUrl(QStringLiteral("u1a2b3c4")).toString(),
                   QStringLiteral("https://api.example.com/jmap/upload/u1a2b3c4/"),
                   QStringLiteral("uploadUrl expands the account id"));
        checkEqual(session.eventSourceUrl({QStringLiteral("Email"),
                                           QStringLiteral("Mailbox")}, 30)
                       .toString(QUrl::FullyEncoded),
                   QStringLiteral("https://api.example.com/jmap/event/?types=Email%2CMailbox"
                                  "&closeafter=no&ping=30"),
                   QStringLiteral("eventSourceUrl expands types, closeafter and ping"));
        // `*` and `,` are both sub-delims, so RFC 6570 level-1 expansion
        // percent-encodes them and a conformant server decodes them back. The
        // encoder is uniform on purpose: exempting the wildcard would be a
        // guess about one server's parser dressed up as a rule.
        checkEqual(session.eventSourceUrl({}, 0).toString(QUrl::FullyEncoded),
                   QStringLiteral("https://api.example.com/jmap/event/?types=%2A"
                                  "&closeafter=no&ping=0"),
                   QStringLiteral("an empty type list subscribes to everything"));
    }

    out() << "relative URLs and an absent primaryAccounts" << Qt::endl;
    {
        JmapSession session;
        QString error;
        check(session.ingest(QByteArray(kRelativeSession), relativeFrom, &error),
              QStringLiteral("a session object with relative URLs is accepted"));
        checkEqual(session.apiUrl().toString(),
                   QStringLiteral("https://localhost:8080/jmap/"),
                   QStringLiteral("a relative apiUrl resolves against the session URL"));
        checkEqual(session.uploadUrl(QStringLiteral("cassandane")).toString(),
                   QStringLiteral("https://localhost:8080/jmap/upload/cassandane/"),
                   QStringLiteral("a relative uploadUrl resolves and still expands"));
        checkEqual(session.eventSourceUrl({QStringLiteral("Email")}, 0)
                       .toString(QUrl::FullyEncoded),
                   QStringLiteral("https://localhost:8080/jmap/event/Email/no/0"),
                   QStringLiteral("template variables in path segments expand too"));
        checkEqual(session.mailAccountId(), QStringLiteral("cassandane"),
                   QStringLiteral("with no primaryAccounts, the personal account wins"));
        check(session.submissionAccountId().isEmpty(),
              QStringLiteral("a server without submission names no submission account"));
        check(session.accounts().size() == 2,
              QStringLiteral("both accounts are listed"));
        check(session.limits().maxCallsInRequest == 16,
              QStringLiteral("the stated call limit is read"));
        check(session.limits().maxObjectsInGet == 0,
              QStringLiteral("a limit the server omits reads as zero, not a guess"));
    }

    out() << "a primaryAccounts entry that names nothing" << Qt::endl;
    {
        JmapSession session;
        QString error;
        check(session.ingest(QByteArray(kDanglingPrimarySession), hostedFrom, &error),
              QStringLiteral("a dangling primary account is not itself fatal"));
        checkEqual(session.mailAccountId(), QStringLiteral("real-account"),
                   QStringLiteral("the account list overrides an id that does not exist"));
    }

    out() << "session objects that cannot be used" << Qt::endl;
    {
        JmapSession session;
        QString error;
        check(!session.ingest(QByteArray(kNoMailSession), hostedFrom, &error),
              QStringLiteral("a server with no mail account is rejected"));
        check(!error.isEmpty(), QStringLiteral("the rejection says why"));
        check(!session.isValid(), QStringLiteral("a rejected session is not valid"));

        error.clear();
        check(!session.ingest(QByteArrayLiteral("{ this is not json"), hostedFrom, &error),
              QStringLiteral("malformed JSON is rejected"));
        check(!error.isEmpty(), QStringLiteral("the parse failure says why"));

        error.clear();
        check(!session.ingest(QByteArrayLiteral("[]"), hostedFrom, &error),
              QStringLiteral("JSON that is not an object is rejected"));

        error.clear();
        check(!session.ingest(QByteArrayLiteral("{\"accounts\":{}}"), hostedFrom, &error),
              QStringLiteral("a session object with no apiUrl is rejected"));

        error.clear();
        check(!session.ingest(QByteArray(), hostedFrom, &error),
              QStringLiteral("an empty body is rejected"));
    }

    out() << "a failed refresh leaves the working session alone" << Qt::endl;
    {
        JmapSession session;
        QString error;
        check(session.ingest(QByteArray(kHostedSession), hostedFrom, &error),
              QStringLiteral("the session starts out good"));
        check(!session.ingest(QByteArray(kNoMailSession), hostedFrom, &error),
              QStringLiteral("a later unusable answer is refused"));
        check(session.isValid(),
              QStringLiteral("the session is still valid after the refusal"));
        checkEqual(session.apiUrl().toString(),
                   QStringLiteral("https://api.example.com/jmap/api/"),
                   QStringLiteral("the working endpoints are untouched"));
        checkEqual(session.mailAccountId(), QStringLiteral("u1a2b3c4"),
                   QStringLiteral("the mail account id is untouched"));

        session.clear();
        check(!session.isValid(), QStringLiteral("clear() invalidates the session"));
        check(session.apiUrl().isEmpty(), QStringLiteral("clear() drops the endpoints"));
        check(session.mailAccountId().isEmpty(),
              QStringLiteral("clear() drops the account id"));
    }

    out() << "a session object recorded from cyrus-jmap-tester" << Qt::endl;
    {
        // Served from /jmap, not /.well-known/jmap: Cyrus 301-redirects, and
        // every URL in the document is relative to where it finally landed.
        const QUrl servedFrom(QStringLiteral("http://localhost:18080/jmap"));
        JmapSession session;
        QString error;
        const QByteArray recorded = readFile(
            QStringLiteral(JMAP_TEST_DATA_DIR "/cyrus-jmap-session.json"));
        check(session.ingest(recorded, servedFrom, &error),
              QStringLiteral("a real Cyrus session object is accepted"));
        checkEqual(error, QString(), QStringLiteral("acceptance reports no error"));
        checkEqual(session.username(), QStringLiteral("cassandane"),
                   QStringLiteral("the username is read"));
        checkEqual(session.apiUrl().toString(),
                   QStringLiteral("http://localhost:18080/jmap/"),
                   QStringLiteral("Cyrus's relative apiUrl resolves past the redirect"));
        checkEqual(session.mailAccountId(), QStringLiteral("cassandane"),
                   QStringLiteral("the mail account is found"));
        checkEqual(session.submissionAccountId(), QStringLiteral("cassandane"),
                   QStringLiteral("the submission account is found"));

        // The check the hand-written fixtures could not make: these are the
        // server's key names, not ours.
        const JmapSession::Limits limits = session.limits();
        check(limits.maxCallsInRequest == 50,
              QStringLiteral("maxCallsInRequest is read from a real server"));
        check(limits.maxObjectsInGet == 4096,
              QStringLiteral("maxObjectsInGet is read from a real server"));
        check(limits.maxObjectsInSet == 4096,
              QStringLiteral("maxObjectsInSet is read from a real server"));
        check(limits.maxSizeRequest == 10485760,
              QStringLiteral("maxSizeRequest is read from a real server"));
        check(limits.maxSizeUpload == 1073741824,
              QStringLiteral("maxSizeUpload is read from a real server"));
        check(limits.maxConcurrentUpload == 5,
              QStringLiteral("maxConcurrentUpload is read from a real server"));
        check(limits.maxConcurrentRequests == 5,
              QStringLiteral("maxConcurrentRequests is read from a real server"));

        check(session.hasCapability(JmapSession::coreCapability()),
              QStringLiteral("the core capability is advertised"));
        check(session.hasCapability(JmapSession::mailCapability()),
              QStringLiteral("the mail capability is advertised"));
        check(session.accountHasCapability(QStringLiteral("cassandane"),
                                           JmapSession::submissionCapability()),
              QStringLiteral("the account may submit"));

        checkEqual(session.uploadUrl(QStringLiteral("cassandane")).toString(),
                   QStringLiteral("http://localhost:18080/jmap/upload/cassandane/"),
                   QStringLiteral("Cyrus's uploadUrl template resolves and expands"));
        checkEqual(session.downloadUrl(QStringLiteral("cassandane"),
                                       QStringLiteral("G1a2b3c"),
                                       QStringLiteral("text/plain"),
                                       QStringLiteral("part.txt"))
                       .toString(QUrl::FullyEncoded),
                   QStringLiteral("http://localhost:18080/jmap/download/cassandane/G1a2b3c/"
                                  "part.txt?accept=text%2Fplain"),
                   QStringLiteral("Cyrus spells the type variable `accept`, and it expands"));
        checkEqual(session.eventSourceUrl({QStringLiteral("Email")}, 30)
                       .toString(QUrl::FullyEncoded),
                   QStringLiteral("http://localhost:18080/jmap/eventsource/?types=Email"
                                  "&closeafter=no&ping=30"),
                   QStringLiteral("Cyrus's eventSourceUrl template expands"));
    }

    out() << "authentication" << Qt::endl;
    {
        MailBackend::Credentials basic;
        basic.user = QStringLiteral("user@example.com");
        basic.password = QStringLiteral("secret");
        checkEqual(QString::fromLatin1(JmapSession::authorizationHeader(basic)),
                   QStringLiteral("Basic dXNlckBleGFtcGxlLmNvbTpzZWNyZXQ="),
                   QStringLiteral("a password becomes a Basic header"));

        MailBackend::Credentials bearer = basic;
        bearer.accessToken = QStringLiteral("ya29.a0Af-token");
        bearer.authType = 1;
        checkEqual(QString::fromLatin1(JmapSession::authorizationHeader(bearer)),
                   QStringLiteral("Bearer ya29.a0Af-token"),
                   QStringLiteral("an OAuth token becomes a Bearer header and wins"));

        check(JmapSession::authorizationHeader({}).isEmpty(),
              QStringLiteral("credentials with nothing in them produce no header"));
    }

    out() << "where discovery looks" << Qt::endl;
    {
        MailBackend::Credentials addressOnly;
        addressOnly.user = QStringLiteral("user@example.com");
        checkEqual(JmapSession::wellKnownUrl(addressOnly).toString(),
                   QStringLiteral("https://example.com/.well-known/jmap"),
                   QStringLiteral("an address alone gives a session URL"));

        MailBackend::Credentials withHost = addressOnly;
        withHost.host = QStringLiteral("jmap.example.net");
        checkEqual(JmapSession::wellKnownUrl(withHost).toString(),
                   QStringLiteral("https://jmap.example.net/.well-known/jmap"),
                   QStringLiteral("a stated host overrides the address domain"));

        MailBackend::Credentials local = addressOnly;
        local.host = QStringLiteral("http://localhost:8080");
        checkEqual(JmapSession::wellKnownUrl(local).toString(),
                   QStringLiteral("http://localhost:8080/.well-known/jmap"),
                   QStringLiteral("a scheme and port in the host are respected"));

        MailBackend::Credentials explicitPath = addressOnly;
        explicitPath.host = QStringLiteral("http://localhost:8080/jmap/session");
        checkEqual(JmapSession::wellKnownUrl(explicitPath).toString(),
                   QStringLiteral("http://localhost:8080/jmap/session"),
                   QStringLiteral("a host with a path names the session resource outright"));

        check(!JmapSession::wellKnownUrl({}).isValid(),
              QStringLiteral("nothing to go on gives no URL rather than a guess"));

        MailBackend::Credentials nameOnly;
        nameOnly.user = QStringLiteral("bare-login-name");
        check(!JmapSession::wellKnownUrl(nameOnly).isValid(),
              QStringLiteral("a login name that is not an address gives no URL"));
    }

    if (failures) {
        out() << failures << " check(s) failed" << Qt::endl;
        return 1;
    }
    out() << "all checks passed" << Qt::endl;
    return 0;
}
