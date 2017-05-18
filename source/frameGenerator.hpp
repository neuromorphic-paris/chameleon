#pragma once

#include <QQmlParserStatus>
#include <QtCore/QDir>
#include <QQmlParserStatus>
#include <QtGui/QOpenGLFunctions_3_3_Core>
#include <QtGui/QOpenGLContext>
#include <QtGui/QImage>
#include <QtQuick/QQuickItem>
#include <QtQuick/qquickwindow.h>

#include <memory>
#include <atomic>
#include <condition_variable>
#include <stdexcept>

/// chameleon provides Qt components for event stream display.
namespace chameleon {

    /// FrameGeneratorRenderer handles openGL calls for a frameGenerator.
    class FrameGeneratorRenderer : public QObject, public QOpenGLFunctions_3_3_Core {
        Q_OBJECT
        public:
            FrameGeneratorRenderer() :
                _programSetup(false),
                _beforeRenderingDone(false),
                _closing(false)
            {
                _renderingNotRequired.clear(std::memory_order_release);
            }
            FrameGeneratorRenderer(const FrameGeneratorRenderer&) = delete;
            FrameGeneratorRenderer(FrameGeneratorRenderer&&) = default;
            FrameGeneratorRenderer& operator=(const FrameGeneratorRenderer&) = delete;
            FrameGeneratorRenderer& operator=(FrameGeneratorRenderer&&) = default;
            virtual ~FrameGeneratorRenderer() {}

            /// setRenderingArea defines the rendering area.
            virtual void setRenderingArea(QRectF captureArea, int windowHeight) {
                _captureArea = captureArea;
                _captureArea.moveTop(windowHeight - _captureArea.top() - _captureArea.height());
            }

            /// saveFrameTo waits for a complete render, takes a screenshots and writes it to a file.
            virtual bool saveFrameTo(const QString& filename) {
                _renderingNotRequired.clear(std::memory_order_release);
                _pixelsMutex.lock();
                if (_closing) {
                    _pixelsMutex.unlock();
                    return true;
                }
                _pixelsMutex.unlock();
                auto lock = std::unique_lock<std::mutex>(_pixelsMutex);
                _pixelsUpdated.wait(lock);
                auto success = true;
                if (!_closing) {
                    success = QImage(
                        _pixels.data(),
                        static_cast<int>(_imageWidth),
                        static_cast<int>(_imageHeight),
                        static_cast<int>(4 * _imageWidth),
                        QImage::Format_RGBA8888_Premultiplied
                    ).mirrored().save(filename);
                }
                lock.unlock();
                return success;
            }

        public slots:

            /// beforeRenderingCallback must be called when a rendering starts.
            void beforeRenderingCallback() {
                if (!_renderingNotRequired.test_and_set(std::memory_order_relaxed)) {
                    _beforeRenderingDone = true;
                }
            }

            /// afterRenderingCallback is called when a rendering ends.
            void afterRenderingCallback() {
                if (!initializeOpenGLFunctions()) {
                    throw std::runtime_error("initializing the OpenGL context failed");
                }
                if (!_programSetup) {
                    _programSetup = true;
                    _programId = glCreateProgram();
                    glLinkProgram(_programId);
                } else {
                    if (_beforeRenderingDone) {
                        _beforeRenderingDone = false;
                        glUseProgram(_programId);
                        glEnable(GL_SCISSOR_TEST);
                        {
                            const std::lock_guard<std::mutex> lock(_pixelsMutex);
                            _pixels.reserve(_captureArea.width() * _captureArea.height() * 4);
                            glReadPixels(
                                static_cast<GLint>(_captureArea.left()),
                                static_cast<GLint>(_captureArea.top()),
                                static_cast<GLsizei>(_captureArea.width()),
                                static_cast<GLsizei>(_captureArea.height()),
                                GL_RGBA,
                                GL_UNSIGNED_BYTE,
                                _pixels.data()
                            );
                            _imageWidth = _captureArea.width();
                            _imageHeight = _captureArea.height();
                        }
                        _pixelsUpdated.notify_one();
                        glDisable(GL_SCISSOR_TEST);
                    }
                }
            }

            /// closing is called when the window is about to be closed.
            void closing() {
                {
                    const std::lock_guard<std::mutex> lock(_pixelsMutex);
                    _closing = true;
                }
                _pixelsUpdated.notify_one();
            }

        protected:
            QRectF _captureArea;
            bool _programSetup;
            GLuint _programId;
            std::atomic_flag _renderingNotRequired;
            bool _beforeRenderingDone;
            std::vector<unsigned char> _pixels;
            std::mutex _pixelsMutex;
            std::condition_variable _pixelsUpdated;
            std::size_t _imageWidth;
            std::size_t _imageHeight;
            bool _closing;
    };

    /// FrameGenerator takes screenshots of the window.
    class FrameGenerator : public QQuickItem {
        Q_OBJECT
        public:
            FrameGenerator() :
                _closing(false),
                _rendererReady(false)
            {
                connect(this, &QQuickItem::windowChanged, this, &FrameGenerator::handleWindowChanged);
            }
            FrameGenerator(const FrameGenerator&) = delete;
            FrameGenerator(FrameGenerator&&) = default;
            FrameGenerator& operator=(const FrameGenerator&) = delete;
            FrameGenerator& operator=(FrameGenerator&&) = default;
            virtual ~FrameGenerator() {}

            /// saveFrameTo triggers a frame render and stores the resulting png image to the given file.
            virtual void saveFrameTo(const std::string& filename) {
                while (!_rendererReady.load(std::memory_order_acquire)) {}
                if (!_closing.load(std::memory_order_relaxed)) {
                    if (!_frameGeneratorRenderer->saveFrameTo(QString::fromStdString(filename))) {
                        throw std::runtime_error(std::string("saving a frame to '") + filename + "' failed");
                    }
                }
            }

        public slots:

            /// sync addapts the renderer to external changes.
            void sync() {
                if (!_frameGeneratorRenderer) {
                    _frameGeneratorRenderer = std::unique_ptr<FrameGeneratorRenderer>(new FrameGeneratorRenderer());
                    connect(
                        window(),
                        SIGNAL(closing(QQuickCloseEvent*)),
                        _frameGeneratorRenderer.get(),
                        SLOT(closing()),
                        Qt::DirectConnection
                    );
                    connect(
                        window(),
                        SIGNAL(closing(QQuickCloseEvent*)),
                        this,
                        SLOT(closing()),
                        Qt::DirectConnection
                    );
                    connect(
                        window(),
                        &QQuickWindow::beforeRendering,
                        _frameGeneratorRenderer.get(),
                        &FrameGeneratorRenderer::beforeRenderingCallback,
                        Qt::DirectConnection
                    );
                    connect(
                        window(),
                        &QQuickWindow::afterRendering,
                        _frameGeneratorRenderer.get(),
                        &FrameGeneratorRenderer::afterRenderingCallback,
                        Qt::DirectConnection
                    );
                    _rendererReady.store(true, std::memory_order_release);
                }

                auto captureArea = QRectF(0, 0, width() * window()->devicePixelRatio(), height() * window()->devicePixelRatio());
                for (auto item = static_cast<QQuickItem*>(this); item; item = item->parentItem()) {
                    captureArea.moveLeft(captureArea.left() + item->x() * window()->devicePixelRatio());
                    captureArea.moveTop(captureArea.top() + item->y() * window()->devicePixelRatio());
                }
                if (captureArea != _captureArea) {
                    _captureArea = std::move(captureArea);
                    _frameGeneratorRenderer->setRenderingArea(_captureArea, window()->height() * window()->devicePixelRatio());
                }
            }

            /// cleanup resets the renderer.
            void cleanup() {
                _frameGeneratorRenderer.reset();
            }

            /// triggerDraw triggers a draw.
            void triggerDraw() {
                if (window()) {
                    window()->update();
                }
            }

            /// closing is called when the window is about to be closed.
            void closing() {
                _closing.store(true, std::memory_order_release);
            }

        private slots:

            /// handleWindowChanged is triggered after a window change.
            void handleWindowChanged(QQuickWindow* window) {
                if (window) {
                    connect(window, &QQuickWindow::beforeSynchronizing, this, &FrameGenerator::sync, Qt::DirectConnection);
                    connect(window, &QQuickWindow::sceneGraphInvalidated, this, &FrameGenerator::cleanup, Qt::DirectConnection);
                    window->setClearBeforeRendering(false);
                }
            }

        protected:
            std::atomic_bool _closing;
            std::atomic_bool _rendererReady;
            std::unique_ptr<FrameGeneratorRenderer> _frameGeneratorRenderer;
            QRectF _captureArea;
    };
}
