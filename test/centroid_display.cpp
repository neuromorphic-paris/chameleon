#include "../source/centroid_display.hpp"
#include "../source/background_cleaner.hpp"
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>

struct centroid {
    float x;
    float y;
};

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    qmlRegisterType<chameleon::background_cleaner>("Chameleon", 1, 0, "BackgroundCleaner");
    qmlRegisterType<chameleon::centroid_display>("Chameleon", 1, 0, "CentroidDisplay");
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
            Timer {
                interval: 20
                running: true
                repeat: true
                onTriggered: {
                    centroid_display.trigger_draw();
                }
            }
            BackgroundCleaner {
                width: window.width
                height: window.height
            }
            CentroidDisplay {
                id: centroid_display
                objectName: "centroid_display"
                canvas_size: "320x240"
                width: window.width
                height: window.height
                stroke_color: "#7eaa5f"
                stroke_thickness: 3
                fill_color: "#887eaa5f"
                radius: 0.5
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
    auto centroid_display = window->findChild<chameleon::centroid_display*>("centroid_display");
    std::atomic_bool running(true);
    std::thread loop([&]() {
        std::uint64_t t = 0;
        const auto time_reference = std::chrono::high_resolution_clock::now();
        for (std::size_t id = 0; id < 3; ++id) {
            centroid_display->insert(id, centroid{static_cast<float>(102 + 50 * id), 120});
        }
        while (running.load(std::memory_order_relaxed)) {
            for (std::size_t index = 0; index < 10; ++index) {
                for (std::size_t id = 0; id < 3; ++id) {
                    centroid_display->update(
                        id,
                        centroid{
                            static_cast<float>(
                                102 + 50 * id
                                + 5 * (std::cos(2 * static_cast<float>(M_PI) * (t - id * 1e6f / 3) / 1e6f) + 1.5f)),
                            120 + 5 * (std::sin(2 * static_cast<float>(M_PI) * (t - id * 1e6f / 3) / 1e6f) + 1.5f)});
                }
                t += 2000;
            }
            std::this_thread::sleep_until(time_reference + std::chrono::microseconds(t));
        }
    });
    const auto error = app.exec();
    running.store(false, std::memory_order_relaxed);
    loop.join();
    return error;
}
