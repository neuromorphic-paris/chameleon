#include "../source/delta_t_display.hpp"
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>

struct event {
    uint64_t delta_t;
    uint16_t x;
    uint16_t y;
};

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    qmlRegisterType<chameleon::delta_t_display>("Chameleon", 1, 0, "DeltaTDisplay");
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
                    delta_t_display.trigger_draw();
                }
            }
            DeltaTDisplay {
                id: delta_t_display
                objectName: "delta_t_display"
                canvas_size: "320x240"
                width: window.width
                height: window.height
                colormap: DeltaTDisplay.Hot
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
    auto delta_t_display = window->findChild<chameleon::delta_t_display*>("delta_t_display");
    std::atomic_bool running(true);
    std::thread loop([&]() {
        std::random_device random_device;
        std::mt19937 engine(random_device());
        std::uniform_int_distribution<uint64_t> delta_t_distribution;
        std::normal_distribution<double> distribution{200, 30};
        std::uint64_t t = 0;
        const auto time_reference = std::chrono::high_resolution_clock::now();
        while (running.load(std::memory_order_relaxed)) {
            for (std::size_t index = 0; index < 1000; ++index) {
                delta_t_display->push(event{
                    delta_t_distribution(engine),
                    static_cast<uint16_t>(
                        static_cast<uint64_t>(
                            320.0 * (static_cast<double>(t % 5000000) / 5000000.0) + distribution(engine) + 1)
                        % 320),
                    static_cast<uint16_t>(
                        static_cast<uint64_t>(
                            240.0 * (static_cast<double>(t % 5000000) / 5000000.0) + distribution(engine) + 1)
                        % 240),
                });
                t += 20;
            }
            std::this_thread::sleep_until(time_reference + std::chrono::microseconds(t));
        }
    });
    const auto error = app.exec();
    running.store(false, std::memory_order_relaxed);
    loop.join();
    return error;
}
