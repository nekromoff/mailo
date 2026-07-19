#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include "documenthandler.h"
#include "mailclient.h"
#include "viewersecurity.h"

int main(int argc, char *argv[])
{
    ViewerSchemeHandler::registerScheme();
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("mailo"));
    QGuiApplication::setApplicationName(QStringLiteral("mailo"));

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

    return app.exec();
}
