// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "viewersecurity.h"

#include <QBuffer>
#include <QQuickWebEngineProfile>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>

void ViewerRequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info)
{
    const QString scheme = info.requestUrl().scheme();
    if (scheme == QLatin1String("data") || scheme == QLatin1String("about")
        || scheme == QLatin1String("mailo"))
        return;
    // Per-message opt-in: allow http(s) subresources (images, CSS, fonts),
    // but never navigation of the viewer itself to a remote page.
    if (m_allowRemote.load()
        && (scheme == QLatin1String("https") || scheme == QLatin1String("http"))
        && info.resourceType() != QWebEngineUrlRequestInfo::ResourceTypeMainFrame)
        return;
    info.block(true);
}

void ViewerSchemeHandler::requestStarted(QWebEngineUrlRequestJob *job)
{
    const QString path = job->requestUrl().path();

    // Inline attachments: mailo:cid/<contentId>
    if (path.startsWith(QLatin1String("cid/"))) {
        const QString cid = path.mid(4);
        const auto it = m_inlineParts.constFind(cid);
        if (it == m_inlineParts.constEnd()) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        auto *buffer = new QBuffer(job);
        buffer->setData(it->data);
        job->reply(it->mimeType, buffer);
        return;
    }

    // Only the document URL itself gets the body. Mail that references
    // root-relative assets ("/packs/assets/x.woff2") resolves them against
    // this scheme, and answering those with the message HTML made Chromium
    // try to parse an email as a font. Those requests have no answer here.
    if (!path.startsWith(QLatin1String("message/"))) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    // reply() wants a bare MIME type — a "; charset=" suffix can make
    // Chromium treat the response as unknown and render a blank page.
    // Charset is declared via a <meta> tag in the served HTML instead.
    auto *buffer = new QBuffer(job);
    buffer->setData(m_html);
    job->reply(QByteArrayLiteral("text/html"), buffer);
}

void ViewerSchemeHandler::setInlinePart(const QString &contentId, const QByteArray &mimeType,
                                        const QByteArray &data)
{
    m_inlineParts.insert(contentId, {mimeType.isEmpty() ? QByteArrayLiteral("application/octet-stream")
                                                        : mimeType,
                                     data});
}

void ViewerSchemeHandler::clearInlineParts()
{
    m_inlineParts.clear();
}

void ViewerSchemeHandler::setRemoteContentAllowed(bool allow)
{
    if (m_interceptor)
        m_interceptor->setRemoteContentAllowed(allow);
}

QString ViewerSchemeHandler::setMessageHtml(const QByteArray &html)
{
    m_html = html;
    return QStringLiteral("mailo:message/%1").arg(++m_serial);
}

void ViewerSchemeHandler::registerScheme()
{
    QWebEngineUrlScheme scheme(QByteArrayLiteral("mailo"));
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme);
    QWebEngineUrlScheme::registerScheme(scheme);
}

ViewerSchemeHandler *ViewerSchemeHandler::install()
{
    QQuickWebEngineProfile *profile = QQuickWebEngineProfile::defaultProfile();
    profile->setOffTheRecord(true);
    auto *interceptor = new ViewerRequestInterceptor(profile);
    profile->setUrlRequestInterceptor(interceptor);
    auto *handler = new ViewerSchemeHandler(profile);
    handler->m_interceptor = interceptor;
    profile->installUrlSchemeHandler(QByteArrayLiteral("mailo"), handler);
    return handler;
}
