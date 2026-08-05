// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

#include "mailbackend.h"

class JmapSession;
class QNetworkAccessManager;
class QNetworkReply;

/**
 * One JMAP method-call batch: build calls, POST them once, get the responses
 * back in order (RFC 8620 §3).
 *
 * This is the whole of JMAP's transport. Where IMAP has a command per thing
 * you want and a session that serializes them, JMAP has one POST carrying as
 * many calls as fit, each answered independently — which is why the backend
 * needs no connection pool and no backfill cursor. A folder listing is
 * `Mailbox/get`; a page of headers is `Email/query` and `Email/get` in the
 * *same* request, the second reading the first's output through a
 * back-reference, so paging costs one round trip rather than two.
 *
 * Deliberately not a queue and not a retry policy. It sends one batch and
 * reports what came back; deciding what a failure means, and whether to ask
 * again, is JmapBackend's. The single exception is a 401, handled here because
 * it is not a failure of the request at all — the session expired, and the
 * caller cannot usefully be told to re-send something it has no reason to
 * think was wrong.
 *
 * Building is separated from sending, and parsing from receiving, for the same
 * reason as in JmapSession: the JSON both ways is where the protocol actually
 * lives, and it is worth testing without a server in the room.
 */
class JmapRequest : public QObject
{
    Q_OBJECT

public:
    /// One entry of `methodResponses`. Errors are responses too in JMAP — a
    /// method that failed answers with the name "error" rather than breaking
    /// the batch — so a caller must check every one it cares about.
    struct Response {
        QString method;       ///< "Mailbox/get", or "error"
        QJsonObject arguments;
        QString callId;       ///< matches the id addCall() returned

        bool isError() const { return method == QLatin1String("error"); }
        /// The JMAP error type ("invalidArguments", "rateLimit", …), empty
        /// when this is not an error response.
        QString errorType() const;
    };

    /// How a batch reports back. \a error is Error::None when the POST itself
    /// succeeded — individual calls may still have answered with errors, which
    /// is what \a responses is for.
    using Callback = std::function<void(MailBackend::Error error,
                                        const QList<Response> &responses,
                                        const QString &message)>;

    /// \a session must outlive the request and must be valid; the request
    /// reads the API URL and the Authorization header from it at send() time,
    /// so a session refreshed in between is picked up automatically.
    explicit JmapRequest(JmapSession *session, QObject *parent = nullptr);
    ~JmapRequest() override;

    // --- Building ----------------------------------------------------------

    /// Appends a method call and returns the id it was given, which is what
    /// back-references and Response::callId use. Returns an empty string when
    /// the batch is already at the server's maxCallsInRequest — the caller
    /// splits, because only it knows which calls may be separated without
    /// breaking a back-reference.
    QString addCall(const QString &method, const QJsonObject &arguments);
    /// Declares a capability this batch relies on. Core and mail are declared
    /// for you; submission and the rest are the caller's to add, and a server
    /// refuses a method whose capability was not named.
    void useCapability(const QString &uri);

    int callCount() const { return m_calls.size(); }
    /// True when addCall() would refuse. Zero means the server stated no
    /// limit, in which case there is none to hit.
    bool isFull() const;

    /// A back-reference (RFC 8620 §3.7): "the value of \a path in whatever
    /// call \a callId returns". Used as the value of an argument whose name is
    /// prefixed with `#` — `args["#ids"] = resultReference(queryId,
    /// "Email/query", "/ids")` feeds a query's output straight into a get,
    /// inside one request.
    static QJsonObject resultReference(const QString &callId, const QString &method,
                                       const QString &path);

    /// The request body as it will be sent. Pure, and public because it is
    /// worth asserting on.
    QJsonObject requestObject() const;

    // --- Sending -----------------------------------------------------------

    /// POSTs the batch. \a done is called exactly once. Sending an empty batch
    /// is a caller error and answers Error::Protocol without touching the
    /// network.
    void send(const Callback &done);
    /// Abandons a request in flight without calling the callback.
    void cancel();

    /// Parses a JMAP response document. \a sessionState receives the server's
    /// `sessionState`, which the caller compares against JmapSession::state()
    /// to notice a session whose endpoints or accounts may have moved.
    static bool parseResponse(const QByteArray &json, QList<Response> &responses,
                              QString &sessionState, QString *error);
    /// The MailBackend error a JMAP method-error type means. Anything
    /// unrecognised is Error::Protocol: the server refused the request itself,
    /// which is the honest reading of a type we do not know.
    static MailBackend::Error errorForType(const QString &type);
    /// The first error response in \a responses, or a default-constructed one.
    static Response firstError(const QList<Response> &responses);

private:
    void post(bool isRetry);
    void handleReply(QNetworkReply *reply, bool isRetry);
    void finish(MailBackend::Error error, const QList<Response> &responses,
                const QString &message);

    JmapSession *m_session = nullptr;
    QNetworkAccessManager *m_net = nullptr;
    QPointer<QNetworkReply> m_reply;
    QJsonArray m_calls;
    QStringList m_capabilities;
    Callback m_done;
    int m_nextCallId = 0;
};
