#include "../source/blob_display.hpp"
#include "../source/background_cleaner.hpp"
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>

struct blob {
    float x;
    float y;
    float sigma_x_squared;
    float sigma_xy;
    float sigma_y_squared;
} __attribute__((packed));

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    qmlRegisterType<chameleon::background_cleaner>("Chameleon", 1, 0, "BackgroundCleaner");
    qmlRegisterType<chameleon::blob_display>("Chameleon", 1, 0, "BlobDisplay");
    QQmlApplicationEngine application_engine;
    application_engine.loadData(R""(
        import QtQuick 2.3
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
                    blob_display.trigger_draw();
                }
            }
            BackgroundCleaner {
                width: window.width
                height: window.height
            }
            BlobDisplay {
                id: blob_display
                objectName: "blob_display"
                canvas_size: "320x240"
                width: window.width
                height: window.height
                stroke_color: "#7eaa5f"
                stroke_thickness: 3
                fill_color: "#887eaa5f"
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
    auto blob_display = window->findChild<chameleon::blob_display*>("blob_display");
    std::atomic_bool running(true);
    std::thread loop([&]() {
        std::uint64_t t = 0;
        const auto time_reference = std::chrono::high_resolution_clock::now();
        for (std::size_t id = 0; id < 3; ++id) {
            blob_display->insert(
                id,
                blob{
                    static_cast<float>(102 + 50 * id),
                    120,
                    std::pow(20 * (std::cos(2 * static_cast<float>(M_PI) * (t - id * 1e6f / 3) / 1e6f) + 1.5f), 2.0f),
                    0,
                    std::pow(20 * (std::sin(2 * static_cast<float>(M_PI) * (t - id * 1e6f / 3) / 1e6f) + 1.5f), 2.0f)});
        }
        while (running.load(std::memory_order_relaxed)) {
            for (std::size_t index = 0; index < 10; ++index) {
                for (std::size_t id = 0; id < 3; ++id) {
                    blob_display->update(
                        id,
                        blob{static_cast<float>(102 + 50 * id),
                             120,
                             std::pow(
                                 20 * (std::cos(2 * static_cast<float>(M_PI) * (t - id * 1e6f / 3) / 1e6f) + 1.5f),
                                 2.0f),
                             0,
                             std::pow(
                                 20 * (std::sin(2 * static_cast<float>(M_PI) * (t - id * 1e6f / 3) / 1e6f) + 1.5f),
                                 2.0f)});
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
