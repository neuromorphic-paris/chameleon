#include "../source/flow_display.hpp"
#include "../source/background_cleaner.hpp"
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>

struct event {
    uint16_t x;
    uint16_t y;
    uint64_t t;
    float vx;
    float vy;
};

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    qmlRegisterType<chameleon::background_cleaner>("Chameleon", 1, 0, "BackgroundCleaner");
    qmlRegisterType<chameleon::flow_display>("Chameleon", 1, 0, "FlowDisplay");
    QQmlApplicationEngine application_engine;
    application_engine.loadData(R""(
        import QtQuick 2.3
        import QtQuick.Window 2.2
        import Chameleon 1.0
        Window {
            id: window
            visible: true
            width: 304
            height: 240
            Timer {
                interval: 20
                running: true
                repeat: true
                onTriggered: {
                    flow_display.trigger_draw();
                }
            }
            BackgroundCleaner {
                width: window.width
                height: window.height
            }
            FlowDisplay {
                id: flow_display
                objectName: "flow_display"
                canvas_size: "304x240"
                width: window.width
                height: window.height
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
    auto flow_display = window->findChild<chameleon::flow_display*>("flow_display");
    std::atomic_bool running(true);
    std::thread loop([&]() {
        std::random_device random_device;
        std::mt19937 engine(random_device());
        std::uniform_real_distribution<float> speed_distribution{0.0, 0.0002};
        std::normal_distribution<double> distribution{200, 30};
        std::uint64_t t = 0;
        const auto time_reference = std::chrono::high_resolution_clock::now();
        while (running.load(std::memory_order_relaxed)) {
            for (std::size_t index = 0; index < 1000; ++index) {
                flow_display->push(event{
                    static_cast<uint16_t>(
                        static_cast<uint64_t>(
                            304.0 * (static_cast<double>(t % 5000000) / 5000000.0) + distribution(engine) + 1)
                        % 304),
                    static_cast<uint16_t>(
                        static_cast<uint64_t>(
                            240.0 * (static_cast<double>(t % 5000000) / 5000000.0) + distribution(engine) + 1)
                        % 240),
                    t,
                    speed_distribution(engine),
                    speed_distribution(engine),
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
