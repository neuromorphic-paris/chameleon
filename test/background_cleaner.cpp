#include "../source/background_cleaner.hpp"
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    qmlRegisterType<chameleon::background_cleaner>("Chameleon", 1, 0, "BackgroundCleaner");
    QQmlApplicationEngine application_engine;
    application_engine.loadData(R""(
        import QtQuick 2.7
        import QtQuick.Window 2.2
        import Chameleon 1.0
        Window {
            id: window
            visible: true
            width: 320
            height: 240
            BackgroundCleaner {
                id: background_cleaner
                width: window.width
                height: window.height
                color: "#7eaa5f"
            }
        }
    )"");
    auto window = qobject_cast<QQuickWindow*>(application_engine.rootObjects().first());
    {
        QSurfaceFormat format;
        format.setDepthBufferSize(24);
        format.setStencilBufferSize(8);
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        window->setFormat(format);
    }
    return app.exec();
}
