#include "../source/t_delta_display.hpp"
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>

struct event {
    uint16_t x;
    uint16_t y;
    uint64_t t_delta;
} __attribute__((packed));

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    qmlRegisterType<chameleon::t_delta_display>("Chameleon", 1, 0, "LogarithmicDisplay");
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
                    t_delta_display.trigger_draw();
                }
            }
            LogarithmicDisplay {
                id: t_delta_display
                objectName: "t_delta_display"
                canvas_size: "320x240"
                width: window.width
                height: window.height
                colormap: LogarithmicDisplay.Heat
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
    auto t_delta_display = window->findChild<chameleon::t_delta_display*>("t_delta_display");
    std::atomic_bool running(true);
    std::thread loop([&]() {
        std::random_device random_device;
        std::mt19937 engine(random_device());
        std::uniform_int_distribution<uint64_t> t_delta_distribution;
        std::normal_distribution<double> distribution{200, 30};
        std::uint64_t t = 0;
        const auto time_reference = std::chrono::high_resolution_clock::now();
        while (running.load(std::memory_order_relaxed)) {
            for (std::size_t index = 0; index < 1000; ++index) {
                t_delta_display->push(
                    event{static_cast<uint16_t>(
                              static_cast<uint64_t>(
                                  320.0 * (static_cast<double>(t % 5000000) / 5000000.0) + distribution(engine) + 1)
                              % 320),
                          static_cast<uint16_t>(
                              static_cast<uint64_t>(
                                  240.0 * (static_cast<double>(t % 5000000) / 5000000.0) + distribution(engine) + 1)
                              % 240),
                          t_delta_distribution(engine)});
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
