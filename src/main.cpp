// SPDX-FileCopyrightText: (c) 2026 Daniel Duris, dusoft@staznosti.sk
// SPDX-License-Identifier: LGPL-3.0-or-later

#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include "documenthandler.h"
#include "mailclient.h"
#include "messagecontext.h"
#include "viewersecurity.h"

#include <QLoggingCategory>

#include <cstdio>

Q_DECLARE_LOGGING_CATEGORY(logTrace)

/// Drops two Qt warnings that say nothing about mailo and everything about the
/// mail being read. Both come out of QTextDocument while it parses a sender's
/// HTML for the plain-text preview and the search index:
///
///   QFont::setPixelSize: Pixel size <= 0        — "font-size:0", the standard
///                                                 way to hide preheader text
///   QTextHtmlParser: Unknown color name '#abc ' — a colour with a stray space,
///                                                 which Qt does not trim
///
/// Neither is actionable, both fire per message, and their volume is chosen by
/// the sender — a single message can bury the log in them, which is enough to
/// make real diagnostics unreadable. Anything else is passed through untouched.
static QtMessageHandler g_previousHandler = nullptr;

static void filterMailHtmlNoise(QtMsgType type, const QMessageLogContext &context,
                                const QString &message)
{
    if (message.startsWith(QLatin1String("QFont::setPixelSize: Pixel size <= 0"))
        || message.startsWith(QLatin1String("QTextHtmlParser::applyAttributes: "
                                            "Unknown color name")))
        return;
    if (g_previousHandler)
        g_previousHandler(type, context, message);
    else
        fprintf(stderr, "%s\n", qPrintable(qFormatLogMessage(type, context, message)));
}

int main(int argc, char *argv[])
{
    g_previousHandler = qInstallMessageHandler(filterMailHtmlNoise);

    ViewerSchemeHandler::registerScheme();
    QtWebEngineQuick::initialize();

    // QApplication, not QGuiApplication, purely for the file pickers. KDE's
    // native ones are widget-based, so without QtWidgets in the process the
    // platform theme cannot offer them and Qt Quick's own pickers open
    // instead — no Places sidebar, and colors of their own rather than the
    // desktop's. Nothing else here uses a widget.
    QApplication app(argc, argv);
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
    // What anything org.kde.desktop does not implement falls back to. It does
    // not implement the file/folder/color pickers, and off a KDE session (no
    // platform theme to supply the native ones) Qt Quick's are what opens.
    // Left to itself that fallback is the Basic style, whose colors are
    // hardcoded rather than taken from the palette: a pale blue selection
    // under white text, roughly 1.6:1, and identical in a dark theme. Fusion
    // follows the system palette instead.
    QQuickStyle::setFallbackStyle(QStringLiteral("Fusion"));

    ViewerSchemeHandler *viewerHandler = ViewerSchemeHandler::install();

    MailClient client;
    client.setViewerHandler(viewerHandler);

    QQmlApplicationEngine engine;
    qmlRegisterSingletonInstance("Mailo.Core", 1, 0, "Mail", &client);
    qmlRegisterType<DocumentHandler>("Mailo.Core", 1, 0, "DocumentHandler");
    // Created only by MailClient (reading pane + detached message windows).
    qmlRegisterUncreatableType<MessageContext>(
        "Mailo.Core", 1, 0, "MessageContext",
        QStringLiteral("MessageContext instances come from Mail"));
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
