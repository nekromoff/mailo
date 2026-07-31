// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include "documenthandler.h"
#include "mailclient.h"
#include "viewersecurity.h"

#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(logTrace)

int main(int argc, char *argv[])
{
    ViewerSchemeHandler::registerScheme();
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("mailo"));
    QGuiApplication::setApplicationName(QStringLiteral("mailo"));
    QGuiApplication::setApplicationVersion(QStringLiteral(MAILO_VERSION));
    // Wayland matches a window to its .desktop entry (and hence its icon) by
    // app_id, which Qt takes from the desktop file name — it must be the
    // desktop entry's basename, not the application name. X11 uses the window
    // icon instead, so set both.
    QGuiApplication::setDesktopFileName(QStringLiteral("org.mailo.Mailo"));

    // The UI asks for named icons (mail-attachment, arrow-down, …), which only
    // resolve once an icon theme is set. A KDE session sets one; anything else
    // — a bare Wayland/X11 session, or the AppImage, which bundles Breeze but
    // has no session to announce it — leaves it unset and every icon renders
    // as an empty square. Only overridden when nothing usable is configured,
    // so a user's own theme still wins.
    QIcon::setFallbackThemeName(QStringLiteral("breeze"));
    if (QIcon::themeName().isEmpty()
        || !QIcon::hasThemeIcon(QStringLiteral("mail-message-new"))) {
        QIcon::setThemeName(QStringLiteral("breeze"));
    }
    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("org.mailo.Mailo")));

    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));

    ViewerSchemeHandler *viewerHandler = ViewerSchemeHandler::install();

    MailClient client;
    client.setViewerHandler(viewerHandler);

    QQmlApplicationEngine engine;
    qmlRegisterSingletonInstance("Mailo.Core", 1, 0, "Mail", &client);
    qmlRegisterType<DocumentHandler>("Mailo.Core", 1, 0, "DocumentHandler");
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("Mailo", "Main");

    const int rc = app.exec();
    // Bracket the teardown: if the window disappears but the process does not,
    // this says whether the event loop even returned before the destructors ran.
    qCDebug(logTrace, "shutdown: event loop returned %d", rc);
    return rc;
}
