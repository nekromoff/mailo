#pragma once

#include <QByteArray>
#include <QHash>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlSchemeHandler>

#include <atomic>

/**
 * Blocks every network request from the message viewer except inline data
 * and our own mailo: message scheme. Remote content (tracking pixels,
 * external images) never leaves the machine. A per-message "load remote
 * images" opt-in can relax this later.
 */
class ViewerRequestInterceptor : public QWebEngineUrlRequestInterceptor
{
    Q_OBJECT

public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    void interceptRequest(QWebEngineUrlRequestInfo &info) override;

    /// Per-message opt-in: when true, http(s) subresources (images, CSS,
    /// fonts) are allowed through. JavaScript stays disabled regardless —
    /// that is a WebEngineView setting, not the interceptor's.
    void setRemoteContentAllowed(bool allow) { m_allowRemote.store(allow); }

private:
    std::atomic<bool> m_allowRemote{false}; // interceptRequest runs on the IO thread
};

/**
 * Serves the currently displayed message body under mailo://message/<n>.
 *
 * WebEngineView.loadHtml() routes through a data: URL, which Chromium caps
 * at ~2 MB — larger HTML mails render as a blank page. Serving the bytes
 * through a scheme handler has no size limit and gives us a place to serve
 * cid: inline attachments later.
 */
class ViewerSchemeHandler : public QWebEngineUrlSchemeHandler
{
    Q_OBJECT

public:
    using QWebEngineUrlSchemeHandler::QWebEngineUrlSchemeHandler;

    void requestStarted(QWebEngineUrlRequestJob *job) override;

    /// Stores the body to serve and returns the (cache-busting) URL for it.
    QString setMessageHtml(const QByteArray &html);

    /// Registers an inline MIME part served as mailo:cid/<contentId>.
    void setInlinePart(const QString &contentId, const QByteArray &mimeType,
                       const QByteArray &data);
    /// Drops all inline parts (call before loading a new message).
    void clearInlineParts();

    /// Forwards the remote-content opt-in to the profile's interceptor.
    void setRemoteContentAllowed(bool allow);

    /// Call before the QGuiApplication is constructed.
    static void registerScheme();

    /// Installs interceptor + handler on the default profile; returns the handler.
    static ViewerSchemeHandler *install();

private:
    struct InlinePart {
        QByteArray mimeType;
        QByteArray data;
    };

    QByteArray m_html;
    QHash<QString, InlinePart> m_inlineParts;
    ViewerRequestInterceptor *m_interceptor = nullptr;
    quint64 m_serial = 0;
};
