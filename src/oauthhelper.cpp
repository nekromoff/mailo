#include "oauthhelper.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

OAuthHelper::OAuthHelper(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

OAuthHelper::Endpoints OAuthHelper::endpointsFor(Provider provider)
{
    if (provider == Gmail) {
        return {QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth"),
                QStringLiteral("https://oauth2.googleapis.com/token"),
                QStringLiteral("https://mail.google.com/")};
    }
    return {QStringLiteral("https://login.microsoftonline.com/common/oauth2/v2.0/authorize"),
            QStringLiteral("https://login.microsoftonline.com/common/oauth2/v2.0/token"),
            QStringLiteral("https://outlook.office365.com/IMAP.AccessAsUser.All "
                           "https://outlook.office365.com/SMTP.Send offline_access")};
}

static QByteArray base64Url(const QByteArray &data)
{
    return data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

static QString randomString(int length)
{
    static const char chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    QString out;
    out.reserve(length);
    for (int i = 0; i < length; ++i)
        out.append(QLatin1Char(chars[QRandomGenerator::system()->bounded(
            int(sizeof(chars)) - 1)]));
    return out;
}

void OAuthHelper::authorize(Provider provider, const QString &clientId,
                            const QString &clientSecret)
{
    if (clientId.isEmpty()) {
        Q_EMIT failed(tr("No OAuth client ID configured for this account."));
        return;
    }
    delete m_server;
    m_server = new QTcpServer(this);
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        Q_EMIT failed(tr("Could not open a local port for the OAuth redirect."));
        return;
    }
    // "localhost", not 127.0.0.1 — the shipped client IDs are registered with
    // a localhost redirect and Google rejects other loopback spellings (400).
    const QString redirect =
        QStringLiteral("http://localhost:%1/").arg(m_server->serverPort());

    m_codeVerifier = randomString(64);
    const QByteArray challenge = base64Url(
        QCryptographicHash::hash(m_codeVerifier.toLatin1(), QCryptographicHash::Sha256));

    const Endpoints ep = endpointsFor(provider);
    QUrl url(ep.authUrl);
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("client_id"), clientId);
    q.addQueryItem(QStringLiteral("redirect_uri"), redirect);
    q.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    q.addQueryItem(QStringLiteral("scope"), ep.scope);
    q.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(challenge));
    q.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    q.addQueryItem(QStringLiteral("access_type"), QStringLiteral("offline")); // Google
    q.addQueryItem(QStringLiteral("prompt"), QStringLiteral("consent"));      // force refresh token
    url.setQuery(q);

    connect(m_server, &QTcpServer::newConnection, this,
            [this, provider, clientId, clientSecret, redirect] {
        QTcpSocket *sock = m_server->nextPendingConnection();
        connect(sock, &QTcpSocket::readyRead, this,
                [this, sock, provider, clientId, clientSecret, redirect] {
            const QByteArray request = sock->readAll();
            const int pathStart = request.indexOf(' ') + 1;
            const int pathEnd = request.indexOf(' ', pathStart);
            const QUrl reqUrl = QUrl(QStringLiteral("http://127.0.0.1")
                                     + QString::fromLatin1(
                                         request.mid(pathStart, pathEnd - pathStart)));
            const QUrlQuery query(reqUrl);
            const QString code = query.queryItemValue(QStringLiteral("code"));
            const QString error = query.queryItemValue(QStringLiteral("error"));

            const QByteArray page = code.isEmpty()
                ? QByteArrayLiteral("<h2>Sign-in failed.</h2>You can close this tab.")
                : QByteArrayLiteral("<h2>Signed in.</h2>You can return to Mailo "
                                    "and close this tab.");
            sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close"
                        "\r\nContent-Length: " + QByteArray::number(page.size())
                        + "\r\n\r\n" + page);
            sock->flush();
            sock->disconnectFromHost();
            m_server->close();

            if (code.isEmpty()) {
                Q_EMIT failed(tr("Browser sign-in failed: %1")
                                  .arg(error.isEmpty() ? tr("no code returned") : error));
                return;
            }
            requestToken(provider, clientId, clientSecret,
                         {{QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
                          {QStringLiteral("code"), code},
                          {QStringLiteral("redirect_uri"), redirect},
                          {QStringLiteral("code_verifier"), m_codeVerifier}});
        });
    });

    QDesktopServices::openUrl(url);
}

void OAuthHelper::refresh(Provider provider, const QString &clientId,
                          const QString &clientSecret, const QString &refreshToken)
{
    if (clientId.isEmpty() || refreshToken.isEmpty()) {
        Q_EMIT failed(tr("No stored OAuth sign-in for this account."));
        return;
    }
    requestToken(provider, clientId, clientSecret,
                 {{QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
                  {QStringLiteral("refresh_token"), refreshToken}});
}

void OAuthHelper::requestToken(Provider provider, const QString &clientId,
                               const QString &clientSecret,
                               const QList<std::pair<QString, QString>> &grant)
{
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("client_id"), clientId);
    if (!clientSecret.isEmpty())
        form.addQueryItem(QStringLiteral("client_secret"), clientSecret);
    for (const auto &[key, value] : grant)
        form.addQueryItem(key, QUrl::toPercentEncoding(value));

    QNetworkRequest req{QUrl(endpointsFor(provider).tokenUrl)};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));
    QNetworkReply *reply =
        m_nam->post(req, form.toString(QUrl::FullyEncoded).toLatin1());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        const QString accessToken = obj.value(QStringLiteral("access_token")).toString();
        if (accessToken.isEmpty()) {
            const QString desc = obj.value(QStringLiteral("error_description")).toString();
            const QString err = obj.value(QStringLiteral("error")).toString();
            Q_EMIT failed(tr("OAuth token request failed: %1")
                              .arg(!desc.isEmpty() ? desc
                                                   : !err.isEmpty() ? err
                                                                    : reply->errorString()));
            return;
        }
        const int expiresIn = obj.value(QStringLiteral("expires_in")).toInt(3600);
        Q_EMIT tokensReady(accessToken,
                           obj.value(QStringLiteral("refresh_token")).toString(),
                           QDateTime::currentDateTimeUtc().addSecs(expiresIn - 60));
    });
}
