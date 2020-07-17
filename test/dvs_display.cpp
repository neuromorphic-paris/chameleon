#include "../source/dvs_display.hpp"
#include "../source/background_cleaner.hpp"
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>

struct event {
    uint64_t t;
    uint16_t x;
    uint16_t y;
    bool on;
};

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    qmlRegisterType<chameleon::background_cleaner>("Chameleon", 1, 0, "BackgroundCleaner");
    qmlRegisterType<chameleon::dvs_display>("Chameleon", 1, 0, "DvsDisplay");
    QQmlApplicationEngine application_engine;
    application_engine.loadData(R""(
        import QtQuick 2.7
        import QtQuick.Layouts 1.1
        import QtQuick.Window 2.2
        import Chameleon 1.0
        Window {
            id: window
            property var hot_colormap: [
                "#ffffffff",
                "#ffffff00",
                "#ffff8000",
                "#ffff0000",
                "#ff800000",
                "#ff000000",
            ]
            property var parameter: 1e6
            visible: true
            width: 320 * 3 + 10 * 2
            height: 240 * 2 + 10
            Timer {
                interval: 20
                running: true
                repeat: true
                onTriggered: {
                    dvs_display_0.trigger_draw();
                    dvs_display_1.trigger_draw();
                    dvs_display_2.trigger_draw();
                    dvs_display_3.trigger_draw();
                    dvs_display_4.trigger_draw();
                    dvs_display_5.trigger_draw();
                }
            }
            BackgroundCleaner {
                width: window.width
                height: window.height
                color: "#888888"
            }
            GridLayout {
                width: window.width
                height: window.height
                columns: 3
                columnSpacing: 10
                rowSpacing: 10
                DvsDisplay {
                    id: dvs_display_0
                    objectName: "dvs_display_0"
                    canvas_size: "320x240"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    parameter: window.parameter
                    style: DvsDisplay.Exponential
                }
                DvsDisplay {
                    id: dvs_display_1
                    objectName: "dvs_display_1"
                    canvas_size: "320x240"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    parameter: window.parameter
                    style: DvsDisplay.Linear
                }
                DvsDisplay {
                    id: dvs_display_2
                    objectName: "dvs_display_2"
                    canvas_size: "320x240"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    parameter: window.parameter
                    style: DvsDisplay.Window
                }
                DvsDisplay {
                    id: dvs_display_3
                    objectName: "dvs_display_3"
                    canvas_size: "320x240"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    parameter: window.parameter
                    style: DvsDisplay.Exponential
                    on_colormap: window.hot_colormap
                    off_colormap: window.hot_colormap
                }
                DvsDisplay {
                    id: dvs_display_4
                    objectName: "dvs_display_4"
                    canvas_size: "320x240"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    parameter: window.parameter
                    style: DvsDisplay.Linear
                    on_colormap: window.hot_colormap
                    off_colormap: window.hot_colormap
                }
                DvsDisplay {
                    id: dvs_display_5
                    objectName: "dvs_display_5"
                    canvas_size: "320x240"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    parameter: window.parameter
                    style: DvsDisplay.Window
                    on_colormap: window.hot_colormap
                    off_colormap: window.hot_colormap
                }
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
    
    std::array<chameleon::dvs_display*, 6> displays = {
        window->findChild<chameleon::dvs_display*>("dvs_display_0"),
        window->findChild<chameleon::dvs_display*>("dvs_display_1"),
        window->findChild<chameleon::dvs_display*>("dvs_display_2"),
        window->findChild<chameleon::dvs_display*>("dvs_display_3"),
        window->findChild<chameleon::dvs_display*>("dvs_display_4"),
        window->findChild<chameleon::dvs_display*>("dvs_display_5"),
    };
    std::atomic_bool running(true);
    std::thread loop([&]() {
        std::random_device random_device;
        std::mt19937 engine(random_device());
        std::normal_distribution<double> distribution{200, 30};
        std::uint64_t t = 0;
        const auto time_reference = std::chrono::high_resolution_clock::now();
        while (running.load(std::memory_order_relaxed)) {
            for (std::size_t index = 0; index < 1000; ++index) {
                const event random_event{
                    t,
                    static_cast<uint16_t>(
                        static_cast<uint64_t>(
                            320.0 * (static_cast<double>(t % 5000000) / 5000000.0) + distribution(engine) + 1)
                        % 320),
                    static_cast<uint16_t>(
                        static_cast<uint64_t>(
                            240.0 * (static_cast<double>(t % 5000000) / 5000000.0) + distribution(engine) + 1)
                        % 240),
                    engine() < std::numeric_limits<uint_fast32_t>::max() / 2,
                };
                for (auto display : displays) {
                    display->push(random_event);
                }
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
