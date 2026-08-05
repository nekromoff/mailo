// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * Checks JmapRequest (doc/JMAP_ROADMAP.md phase 1): the shape of a method-call
 * batch, back-references, the server's call limit, and how a response document
 * — including the error responses JMAP delivers *inside* a successful POST —
 * is read back.
 *
 * Offline by default, for the reason given in jmapsessiontest.cpp. The live
 * mode is not a substitute: it is how a recording gets made in the first place.
 *   jmaprequesttest --live <host> --bearer <token>
 * asks the container for its mailboxes, which is Phase 1's `Mailbox/get` step
 * run for real.
 */

#include "../src/jmaprequest.h"
#include "../src/jmapsession.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>

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

/// A session standing in for a discovered one, with \a maxCalls as the
/// server's stated batch limit (0 = the server stated none).
static bool makeSession(JmapSession &session, int maxCalls)
{
    const QString json = QStringLiteral(R"json({
      "capabilities": {
        "urn:ietf:params:jmap:core": { "maxCallsInRequest": %1 },
        "urn:ietf:params:jmap:mail": {}
      },
      "accounts": {
        "acct1": {
          "name": "test",
          "isPersonal": true,
          "accountCapabilities": { "urn:ietf:params:jmap:mail": {} }
        }
      },
      "primaryAccounts": { "urn:ietf:params:jmap:mail": "acct1" },
      "apiUrl": "https://example.com/jmap/"
    })json")
                             .arg(maxCalls);
    QString error;
    return session.ingest(json.toUtf8(),
                          QUrl(QStringLiteral("https://example.com/.well-known/jmap")),
                          &error);
}

/// A response document of the shape Cyrus returns for the Phase 1 header page:
/// a query answered by a get that read the query's output.
static const char *kPagedResponse = R"json({
  "methodResponses": [
    ["Email/query", {
      "accountId": "acct1",
      "queryState": "qs-1",
      "canCalculateChanges": true,
      "position": 0,
      "total": 2,
      "ids": ["Ma1", "Mb2"]
    }, "c0"],
    ["Email/get", {
      "accountId": "acct1",
      "state": "es-7",
      "list": [
        { "id": "Ma1", "blobId": "Ga1", "size": 4096 },
        { "id": "Mb2", "blobId": "Gb2", "size": 8192 }
      ],
      "notFound": []
    }, "c1"]
  ],
  "sessionState": "0"
})json";

/// A batch in which one call failed and the other did not — the case that
/// makes "the POST succeeded" and "the request worked" different questions.
static const char *kPartialErrorResponse = R"json({
  "methodResponses": [
    ["Mailbox/get", { "accountId": "acct1", "state": "ms-3", "list": [] }, "c0"],
    ["error", { "type": "invalidArguments", "description": "no such filter" }, "c1"]
  ],
  "sessionState": "0"
})json";

static int runChecks()
{
    out() << "building a batch" << Qt::endl;
    {
        JmapSession session;
        check(makeSession(session, 0), QStringLiteral("the stand-in session is valid"));
        JmapRequest request(&session);

        checkEqual(request.addCall(QStringLiteral("Mailbox/get"),
                                   QJsonObject{{QStringLiteral("accountId"),
                                                QStringLiteral("acct1")}}),
                   QStringLiteral("c0"), QStringLiteral("the first call is c0"));
        checkEqual(request.addCall(QStringLiteral("Email/query"), {}),
                   QStringLiteral("c1"), QStringLiteral("call ids increment"));
        check(request.callCount() == 2, QStringLiteral("both calls are in the batch"));

        const QJsonObject body = request.requestObject();
        const QJsonArray using_ = body.value(QLatin1String("using")).toArray();
        check(using_.contains(QJsonValue(JmapSession::coreCapability())),
              QStringLiteral("the core capability is declared without being asked for"));
        check(using_.contains(QJsonValue(JmapSession::mailCapability())),
              QStringLiteral("the mail capability is declared without being asked for"));
        check(!using_.contains(QJsonValue(JmapSession::submissionCapability())),
              QStringLiteral("submission is not declared until a caller needs it"));

        request.useCapability(JmapSession::submissionCapability());
        request.useCapability(JmapSession::submissionCapability());
        const QJsonArray afterUse =
            request.requestObject().value(QLatin1String("using")).toArray();
        check(afterUse.contains(QJsonValue(JmapSession::submissionCapability())),
              QStringLiteral("a declared capability appears"));
        check(afterUse.size() == 3,
              QStringLiteral("declaring the same capability twice adds it once"));

        const QJsonArray calls = body.value(QLatin1String("methodCalls")).toArray();
        check(calls.size() == 2, QStringLiteral("methodCalls carries both calls"));
        const QJsonArray first = calls.at(0).toArray();
        check(first.size() == 3,
              QStringLiteral("a method call is a three-element array"));
        checkEqual(first.at(0).toString(), QStringLiteral("Mailbox/get"),
                   QStringLiteral("the method name comes first"));
        checkEqual(first.at(1).toObject().value(QLatin1String("accountId")).toString(),
                   QStringLiteral("acct1"),
                   QStringLiteral("the arguments come second"));
        checkEqual(first.at(2).toString(), QStringLiteral("c0"),
                   QStringLiteral("the call id comes third"));
    }

    out() << "back-references" << Qt::endl;
    {
        JmapSession session;
        makeSession(session, 0);
        JmapRequest request(&session);

        const QString queryId = request.addCall(
            QStringLiteral("Email/query"),
            QJsonObject{{QStringLiteral("accountId"), QStringLiteral("acct1")},
                        {QStringLiteral("position"), 0},
                        {QStringLiteral("limit"), 50}});
        request.addCall(
            QStringLiteral("Email/get"),
            QJsonObject{{QStringLiteral("accountId"), QStringLiteral("acct1")},
                        {QStringLiteral("#ids"),
                         JmapRequest::resultReference(queryId,
                                                      QStringLiteral("Email/query"),
                                                      QStringLiteral("/ids"))}});

        const QJsonObject get = request.requestObject()
                                    .value(QLatin1String("methodCalls"))
                                    .toArray()
                                    .at(1)
                                    .toArray()
                                    .at(1)
                                    .toObject();
        check(get.contains(QLatin1String("#ids")),
              QStringLiteral("a back-referenced argument is named with a leading #"));
        check(!get.contains(QLatin1String("ids")),
              QStringLiteral("and does not also appear under its plain name"));
        const QJsonObject reference = get.value(QLatin1String("#ids")).toObject();
        checkEqual(reference.value(QLatin1String("resultOf")).toString(),
                   QStringLiteral("c0"),
                   QStringLiteral("the reference names the call it reads"));
        checkEqual(reference.value(QLatin1String("name")).toString(),
                   QStringLiteral("Email/query"),
                   QStringLiteral("the reference names that call's method"));
        checkEqual(reference.value(QLatin1String("path")).toString(),
                   QStringLiteral("/ids"),
                   QStringLiteral("the reference carries the JSON pointer"));
    }

    out() << "the server's call limit" << Qt::endl;
    {
        JmapSession session;
        makeSession(session, 2);
        JmapRequest request(&session);
        check(!request.isFull(), QStringLiteral("an empty batch is not full"));
        check(!request.addCall(QStringLiteral("Mailbox/get"), {}).isEmpty(),
              QStringLiteral("the first call is accepted"));
        check(!request.addCall(QStringLiteral("Mailbox/get"), {}).isEmpty(),
              QStringLiteral("the second call is accepted"));
        check(request.isFull(), QStringLiteral("the batch is full at the stated limit"));
        check(request.addCall(QStringLiteral("Mailbox/get"), {}).isEmpty(),
              QStringLiteral("a call past the limit is refused rather than sent"));
        check(request.callCount() == 2,
              QStringLiteral("the refused call is not in the batch"));

        JmapSession unlimited;
        makeSession(unlimited, 0);
        JmapRequest open(&unlimited);
        for (int i = 0; i < 200; ++i)
            open.addCall(QStringLiteral("Mailbox/get"), {});
        check(!open.isFull(),
              QStringLiteral("a server that states no limit imposes none"));
        check(open.callCount() == 200,
              QStringLiteral("and every call is kept"));
    }

    out() << "reading a response" << Qt::endl;
    {
        QList<JmapRequest::Response> responses;
        QString sessionState;
        QString error;
        check(JmapRequest::parseResponse(QByteArray(kPagedResponse), responses,
                                         sessionState, &error),
              QStringLiteral("a well-formed response is parsed"));
        check(responses.size() == 2, QStringLiteral("both responses are returned"));
        checkEqual(sessionState, QStringLiteral("0"),
                   QStringLiteral("the sessionState is handed back for staleness checks"));

        checkEqual(responses.at(0).method, QStringLiteral("Email/query"),
                   QStringLiteral("the first response names its method"));
        checkEqual(responses.at(0).callId, QStringLiteral("c0"),
                   QStringLiteral("the first response carries its call id"));
        check(!responses.at(0).isError(),
              QStringLiteral("a successful response is not an error"));
        check(responses.at(0).arguments.value(QLatin1String("ids")).toArray().size() == 2,
              QStringLiteral("the query's ids survive"));
        check(responses.at(1).arguments.value(QLatin1String("list")).toArray().size() == 2,
              QStringLiteral("the get's list survives"));
        check(JmapRequest::firstError(responses).method.isEmpty(),
              QStringLiteral("a batch with no errors has no first error"));
    }

    out() << "an error inside a successful POST" << Qt::endl;
    {
        QList<JmapRequest::Response> responses;
        QString sessionState;
        QString error;
        check(JmapRequest::parseResponse(QByteArray(kPartialErrorResponse), responses,
                                         sessionState, &error),
              QStringLiteral("a batch containing an error still parses"));
        check(responses.size() == 2, QStringLiteral("both responses are returned"));
        check(!responses.at(0).isError(),
              QStringLiteral("the call that worked is not an error"));
        check(responses.at(1).isError(),
              QStringLiteral("the call that failed is an error"));
        checkEqual(responses.at(1).errorType(), QStringLiteral("invalidArguments"),
                   QStringLiteral("the error type is readable"));
        check(responses.at(0).errorType().isEmpty(),
              QStringLiteral("a non-error response has no error type"));
        checkEqual(JmapRequest::firstError(responses).callId, QStringLiteral("c1"),
                   QStringLiteral("firstError finds the failed call"));
    }

    out() << "responses that cannot be used" << Qt::endl;
    {
        QList<JmapRequest::Response> responses;
        QString sessionState;
        QString error;
        check(!JmapRequest::parseResponse(QByteArrayLiteral("{ not json"), responses,
                                          sessionState, &error),
              QStringLiteral("malformed JSON is rejected"));
        check(!error.isEmpty(), QStringLiteral("the parse failure says why"));
        check(!JmapRequest::parseResponse(QByteArrayLiteral("[]"), responses, sessionState,
                                          &error),
              QStringLiteral("JSON that is not an object is rejected"));
        check(!JmapRequest::parseResponse(QByteArrayLiteral("{\"sessionState\":\"0\"}"),
                                          responses, sessionState, &error),
              QStringLiteral("a response with no methodResponses is rejected"));
        check(!JmapRequest::parseResponse(
                  QByteArrayLiteral("{\"methodResponses\":[[\"Mailbox/get\",{}]]}"),
                  responses, sessionState, &error),
              QStringLiteral("a method response that is not a triple is rejected"));
    }

    out() << "a batch recorded from cyrus-jmap-tester" << Qt::endl;
    {
        // The same three calls the live mode issues, answered by the real
        // server. Here to catch what a hand-written fixture cannot: JMAP
        // property names we only think we know. (jmapsessiontest.cpp says how
        // that lesson was learned.)
        QFile file(QStringLiteral(JMAP_TEST_DATA_DIR "/cyrus-jmap-page.json"));
        if (!file.open(QIODevice::ReadOnly)) {
            check(false, QStringLiteral("cannot read the recorded page: %1")
                             .arg(file.errorString()));
        } else {
            QList<JmapRequest::Response> responses;
            QString sessionState;
            QString error;
            check(JmapRequest::parseResponse(file.readAll(), responses, sessionState,
                                             &error),
                  QStringLiteral("a real Cyrus batch response is parsed"));
            check(responses.size() == 3,
                  QStringLiteral("all three method responses come back"));
            check(JmapRequest::firstError(responses).method.isEmpty(),
                  QStringLiteral("no call in the recorded batch failed"));

            checkEqual(responses.at(0).method, QStringLiteral("Mailbox/get"),
                       QStringLiteral("c0 answered Mailbox/get"));
            const QJsonArray mailboxes =
                responses.at(0).arguments.value(QLatin1String("list")).toArray();
            check(mailboxes.size() == 1, QStringLiteral("one mailbox is listed"));
            // The whole point of Phase 1's folder path: the server states the
            // role, so trashFolderName() stops guessing from the name.
            checkEqual(mailboxes.at(0).toObject().value(QLatin1String("role")).toString(),
                       QStringLiteral("inbox"),
                       QStringLiteral("the mailbox states its role rather than only a name"));

            checkEqual(responses.at(1).method, QStringLiteral("Email/query"),
                       QStringLiteral("c1 answered Email/query"));
            const QJsonArray ids =
                responses.at(1).arguments.value(QLatin1String("ids")).toArray();
            check(ids.size() == 2, QStringLiteral("the query found both messages"));
            check(responses.at(1).arguments.value(QLatin1String("total")).toInt() == 2,
                  QStringLiteral("the query reports the folder total"));
            check(responses.at(1)
                      .arguments.value(QLatin1String("canCalculateChanges"))
                      .toBool(),
                  QStringLiteral("the server can calculate changes — what Phase 3 needs"));

            // The back-reference is the claim under test: c2 was sent with no
            // ids of its own, only a pointer at c1's output.
            checkEqual(responses.at(2).method, QStringLiteral("Email/get"),
                       QStringLiteral("c2 answered Email/get"));
            const QJsonArray list =
                responses.at(2).arguments.value(QLatin1String("list")).toArray();
            check(list.size() == 2,
                  QStringLiteral("the back-reference fed the query's ids into the get"));
            check(responses.at(2).arguments.value(QLatin1String("notFound")).toArray().isEmpty(),
                  QStringLiteral("and none of them went missing"));

            const QJsonObject email = list.at(0).toObject();
            check(email.value(QLatin1String("id")).toString().startsWith(QLatin1Char('M')),
                  QStringLiteral("an email carries its id"));
            check(!email.value(QLatin1String("blobId")).toString().isEmpty(),
                  QStringLiteral("and a blobId, which is how the body is fetched"));
            check(!email.value(QLatin1String("threadId")).toString().isEmpty(),
                  QStringLiteral("and a threadId"));
            check(email.value(QLatin1String("size")).toInt() > 0,
                  QStringLiteral("and a size"));
            check(!email.value(QLatin1String("receivedAt")).toString().isEmpty(),
                  QStringLiteral("and a receivedAt"));
            check(email.value(QLatin1String("mailboxIds")).toObject().size() == 1,
                  QStringLiteral("mailboxIds is an object of id->true, not a list"));
            check(email.value(QLatin1String("keywords")).isObject(),
                  QStringLiteral("keywords is an object, and empty means unread"));
        }
    }

    out() << "error types" << Qt::endl;
    {
        check(JmapRequest::errorForType(QStringLiteral("rateLimit"))
                  == MailBackend::Error::Throttled,
              QStringLiteral("rateLimit means back off"));
        check(JmapRequest::errorForType(QStringLiteral("serverUnavailable"))
                  == MailBackend::Error::Throttled,
              QStringLiteral("serverUnavailable means back off"));
        check(JmapRequest::errorForType(QStringLiteral("forbidden"))
                  == MailBackend::Error::Auth,
              QStringLiteral("forbidden means re-authenticate"));
        check(JmapRequest::errorForType(QStringLiteral("accountNotFound"))
                  == MailBackend::Error::NotFound,
              QStringLiteral("accountNotFound means gone"));
        check(JmapRequest::errorForType(QStringLiteral("invalidArguments"))
                  == MailBackend::Error::Protocol,
              QStringLiteral("invalidArguments is the server refusing the request"));
        check(JmapRequest::errorForType(QStringLiteral("somethingNewInAnRFC"))
                  == MailBackend::Error::Protocol,
              QStringLiteral("an unrecognised type is reported as Protocol, not guessed at"));
    }

    out() << "sending nothing" << Qt::endl;
    {
        JmapSession session;
        makeSession(session, 0);
        JmapRequest request(&session);
        MailBackend::Error reported = MailBackend::Error::None;
        bool called = false;
        request.send([&](MailBackend::Error error, const QList<JmapRequest::Response> &,
                         const QString &) {
            reported = error;
            called = true;
        });
        check(called, QStringLiteral("an empty batch answers without touching the network"));
        check(reported == MailBackend::Error::Protocol,
              QStringLiteral("and reports it as a protocol error"));

        JmapSession invalid;
        JmapRequest overNothing(&invalid);
        overNothing.addCall(QStringLiteral("Mailbox/get"), {});
        called = false;
        overNothing.send([&](MailBackend::Error error, const QList<JmapRequest::Response> &,
                             const QString &) {
            reported = error;
            called = true;
        });
        check(called, QStringLiteral("a batch with no session answers immediately"));
        check(reported == MailBackend::Error::Auth,
              QStringLiteral("and reports there is no session to send over"));
    }

    if (failures) {
        out() << failures << " check(s) failed" << Qt::endl;
        return 1;
    }
    out() << "all checks passed" << Qt::endl;
    return 0;
}

/// Discovers against a real server and asks it for its mailboxes — Phase 1's
/// `Mailbox/get`, run end to end. Meant for the test container.
static int runLive(const QString &host, const QString &token)
{
    MailBackend::Credentials credentials;
    credentials.host = host;
    credentials.accessToken = token;

    JmapSession session;
    int result = 1;
    QEventLoop loop;

    QObject::connect(&session, &JmapSession::failed, &loop,
                     [&](MailBackend::Error error, const QString &message) {
                         out() << "discovery failed (" << static_cast<int>(error)
                               << "): " << message << Qt::endl;
                         loop.quit();
                     });
    QObject::connect(&session, &JmapSession::ready, &loop, [&] {
        auto *request = new JmapRequest(&session);
        const QString account = session.mailAccountId();
        request->addCall(QStringLiteral("Mailbox/get"),
                         QJsonObject{{QStringLiteral("accountId"), account},
                                     {QStringLiteral("ids"), QJsonValue::Null}});
        // The Phase 1 header page, as it will actually be issued: a windowed
        // query and the get that reads its output, in one round trip.
        QJsonObject sort;
        sort.insert(QStringLiteral("property"), QStringLiteral("receivedAt"));
        sort.insert(QStringLiteral("isAscending"), false);
        const QString queryId = request->addCall(
            QStringLiteral("Email/query"),
            QJsonObject{{QStringLiteral("accountId"), account},
                        {QStringLiteral("sort"), QJsonArray{sort}},
                        {QStringLiteral("position"), 0},
                        {QStringLiteral("limit"), 10}});
        request->addCall(
            QStringLiteral("Email/get"),
            QJsonObject{{QStringLiteral("accountId"), account},
                        {QStringLiteral("#ids"),
                         JmapRequest::resultReference(queryId,
                                                      QStringLiteral("Email/query"),
                                                      QStringLiteral("/ids"))},
                        {QStringLiteral("properties"),
                         QJsonArray{QStringLiteral("id"), QStringLiteral("blobId"),
                                    QStringLiteral("threadId"),
                                    QStringLiteral("mailboxIds"),
                                    QStringLiteral("keywords"),
                                    QStringLiteral("receivedAt"), QStringLiteral("size"),
                                    QStringLiteral("subject"), QStringLiteral("from")}}});

        out() << "POST " << session.apiUrl().toString() << Qt::endl
              << QString::fromUtf8(QJsonDocument(request->requestObject())
                                       .toJson(QJsonDocument::Compact))
              << Qt::endl;

        request->send([&, request](MailBackend::Error error,
                                   const QList<JmapRequest::Response> &responses,
                                   const QString &message) {
            request->deleteLater();
            if (error != MailBackend::Error::None) {
                out() << "request failed (" << static_cast<int>(error)
                      << "): " << message << Qt::endl;
                loop.quit();
                return;
            }
            for (const JmapRequest::Response &response : responses) {
                out() << "--- " << response.callId << "  " << response.method << Qt::endl;
                if (response.isError()) {
                    out() << "    error type: " << response.errorType() << Qt::endl;
                    continue;
                }
                if (response.method == QLatin1String("Mailbox/get")) {
                    const QJsonArray list =
                        response.arguments.value(QLatin1String("list")).toArray();
                    out() << "    state=" << response.arguments.value(QLatin1String("state"))
                                                 .toString()
                          << "  " << list.size() << " mailbox(es)" << Qt::endl;
                    for (const QJsonValue &value : list) {
                        const QJsonObject mailbox = value.toObject();
                        out() << "    " << mailbox.value(QLatin1String("id")).toString()
                              << "  name=" << mailbox.value(QLatin1String("name")).toString()
                              << "  role="
                              << (mailbox.value(QLatin1String("role")).isNull()
                                      ? QStringLiteral("(none)")
                                      : mailbox.value(QLatin1String("role")).toString())
                              << "  total="
                              << mailbox.value(QLatin1String("totalEmails")).toInt()
                              << "  unread="
                              << mailbox.value(QLatin1String("unreadEmails")).toInt()
                              << Qt::endl;
                    }
                } else {
                    out() << "    "
                          << QString::fromUtf8(
                                 QJsonDocument(response.arguments).toJson(QJsonDocument::Compact))
                          << Qt::endl;
                }
            }
            result = 0;
            loop.quit();
        });
    });

    session.discover(credentials);
    loop.exec();
    return result;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QStringList args = app.arguments();
    if (args.size() > 2 && args.at(1) == QLatin1String("--live")) {
        const QString scheme = args.value(3);
        return runLive(args.at(2),
                       scheme == QLatin1String("--bearer") ? args.value(4) : QString());
    }
    return runChecks();
}
