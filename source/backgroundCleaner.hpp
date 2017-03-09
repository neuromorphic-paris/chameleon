#pragma once

#include <QColor>
#include <QtQuick/qquickwindow.h>
#include <QtGui/QOpenGLFunctions_3_3_Core>
#include <QtQuick/QQuickItem>
#include <QtGui/QOpenGLContext>

#include <memory>
#include <atomic>

/// chameleon provides Qt components for event stream display.
namespace chameleon {

    /// BackgroundCleanerRenderer handles openGL calls for a background cleaner.
    class BackgroundCleanerRenderer : public QObject, public QOpenGLFunctions_3_3_Core {
        Q_OBJECT
        public:
            BackgroundCleanerRenderer(const QColor& color) :
                _color(color)
            {
            }
            BackgroundCleanerRenderer(const BackgroundCleanerRenderer&) = delete;
            BackgroundCleanerRenderer(BackgroundCleanerRenderer&&) = default;
            BackgroundCleanerRenderer& operator=(const BackgroundCleanerRenderer&) = delete;
            BackgroundCleanerRenderer& operator=(BackgroundCleanerRenderer&&) = default;
            virtual ~BackgroundCleanerRenderer() {}

            /// setRenderingArea defines the clear area.
            virtual void setRenderingArea(const QRectF& clearArea, const int& windowHeight) {
                _clearArea = clearArea;
                _clearArea.moveTop(windowHeight - _clearArea.top() - _clearArea.height());
            }

        public slots:

            /// paint sends commands to the GPU.
            void paint() {
                if (!initializeOpenGLFunctions()) {
                    throw std::runtime_error("initializing the OpenGL context failed");
                }

                // resize the rendering area
                glEnable(GL_SCISSOR_TEST);
                glScissor(
                    static_cast<GLint>(_clearArea.left()),
                    static_cast<GLint>(_clearArea.top()),
                    static_cast<GLsizei>(_clearArea.width()),
                    static_cast<GLsizei>(_clearArea.height())
                );
                glClearColor(
                    static_cast<GLfloat>(_color.redF()),
                    static_cast<GLfloat>(_color.greenF()),
                    static_cast<GLfloat>(_color.blueF()),
                    static_cast<GLfloat>(_color.alphaF())
                );
                glClear(GL_COLOR_BUFFER_BIT);
                glDisable(GL_SCISSOR_TEST);
                checkOpenGLError();
            }

        protected:

            /// checkOpenGLError throws if openGL generated an error.
            virtual void checkOpenGLError() {
                switch (glGetError()) {
                    case GL_NO_ERROR:
                        break;
                    case GL_INVALID_ENUM:
                        throw std::logic_error("OpenGL error: GL_INVALID_ENUM");
                    case GL_INVALID_VALUE:
                        throw std::logic_error("OpenGL error: GL_INVALID_VALUE");
                    case GL_INVALID_OPERATION:
                        throw std::logic_error("OpenGL error: GL_INVALID_OPERATION");
                    case GL_OUT_OF_MEMORY:
                        throw std::logic_error("OpenGL error: GL_OUT_OF_MEMORY");
                }
            }

            QColor _color;
            QRectF _clearArea;
    };

    /// BackgroundCleaner cleans the background for OpenGL renderers.
    class BackgroundCleaner : public QQuickItem {
        Q_OBJECT
        Q_INTERFACES(QQmlParserStatus)
        Q_PROPERTY(QColor color READ color WRITE setColor)
        public:
            BackgroundCleaner() :
                _ready(false),
                _color(Qt::black)
            {
                connect(this, &QQuickItem::windowChanged, this, &BackgroundCleaner::handleWindowChanged);
            }
            BackgroundCleaner(const BackgroundCleaner&) = delete;
            BackgroundCleaner(BackgroundCleaner&&) = default;
            BackgroundCleaner& operator=(const BackgroundCleaner&) = delete;
            BackgroundCleaner& operator=(BackgroundCleaner&&) = default;
            virtual ~BackgroundCleaner() {}

            /// setColor defines the clear color.
            /// The color will be passed to the openGL renderer, therefore it should only be set during qml construction.
            virtual void setColor(QColor color) {
                if (_ready.load(std::memory_order_acquire)) {
                    throw std::logic_error("color can only be set during qml construction");
                }
                _color = color;
            }

            /// color returns the currently used color.
            virtual QColor color() const {
                return _color;
            }

            /// componentComplete is called when all the qml values are binded.
            virtual void componentComplete() {
                _ready.store(true, std::memory_order_release);
            }

        public slots:

            /// sync adapts the renderer to external changes.
            void sync() {
                if (_ready.load(std::memory_order_relaxed)) {
                    if (!_backgroundCleanerRenderer) {
                        _backgroundCleanerRenderer = std::unique_ptr<BackgroundCleanerRenderer>(
                            new BackgroundCleanerRenderer(_color)
                        );
                        connect(
                            window(),
                            &QQuickWindow::beforeRendering,
                            _backgroundCleanerRenderer.get(),
                            &BackgroundCleanerRenderer::paint,
                            Qt::DirectConnection
                        );
                    }
                    auto clearArea = QRectF(0, 0, width() * window()->devicePixelRatio(), height() * window()->devicePixelRatio());
                    for (auto item = static_cast<QQuickItem*>(this); item; item = item->parentItem()) {
                        clearArea.moveLeft(clearArea.left() + item->x() * window()->devicePixelRatio());
                        clearArea.moveTop(clearArea.top() + item->y() * window()->devicePixelRatio());
                    }
                    if (clearArea != _clearArea) {
                        _clearArea = std::move(clearArea);
                        _backgroundCleanerRenderer->setRenderingArea(_clearArea, window()->height() * window()->devicePixelRatio());
                    }
                }
            }

            /// cleanup resets the renderer.
            void cleanup() {
                _backgroundCleanerRenderer.reset();
            }

            /// triggerDraw triggers a draw.
            void triggerDraw() {
                if (window()) {
                    window()->update();
                }
            }

        private slots:

            /// handleWindowChanged must be triggered after a window change.
            void handleWindowChanged(QQuickWindow* window) {
                if (window) {
                    connect(window, &QQuickWindow::beforeSynchronizing, this, &BackgroundCleaner::sync, Qt::DirectConnection);
                    connect(window, &QQuickWindow::sceneGraphInvalidated, this, &BackgroundCleaner::cleanup, Qt::DirectConnection);
                    window->setClearBeforeRendering(false);
                }
            }

        protected:
            std::atomic_bool _ready;
            QColor _color;
            std::unique_ptr<BackgroundCleanerRenderer> _backgroundCleanerRenderer;
            QRectF _clearArea;
    };
}
