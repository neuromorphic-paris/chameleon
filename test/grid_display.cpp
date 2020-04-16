#include "../source/grid_display.hpp"
#include "../source/background_cleaner.hpp"
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    qmlRegisterType<chameleon::background_cleaner>("Chameleon", 1, 0, "BackgroundCleaner");
    qmlRegisterType<chameleon::grid_display>("Chameleon", 1, 0, "GridDisplay");
    QQmlApplicationEngine application_engine;
    application_engine.loadData(R""(
        import QtQuick 2.7
        import QtQuick.Window 2.2
        import Chameleon 1.0
        Window {
            id: window
            visible: true
            width: 240
            height: 180
            BackgroundCleaner {
                width: window.width
                height: window.height
            }
            GridDisplay {
                id: grid_display
                objectName: "grid_display"
                canvas_size: "240x180"
                width: window.width
                height: window.height
                stroke_color: "#00275c"
                offset_x: 5
                offset_y: 5
                pitch: 20
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
    auto grid_display = window->findChild<chameleon::grid_display*>("grid_display");
    
    return app.exec();
}
