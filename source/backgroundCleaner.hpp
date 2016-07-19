#pragma once

#include <QColor>
#include <QtQuick/qquickwindow.h>
#include <QtGui/QOpenGLFunctions>
#include <QtQuick/QQuickItem>
#include <QtGui/QOpenGLContext>

#include <memory>
#include <stdexcept>
#include <atomic>
#include <algorithm>
#include <limits>

/// chameleon provides Qt components for event stream display.
namespace chameleon {

    /// BackgroundCleanerRenderer handles openGL calls for a background cleaner.
    class BackgroundCleanerRenderer : public QObject, public QOpenGLFunctions {
        Q_OBJECT
        public:
            BackgroundCleanerRenderer(QColor color) :
                _color(color),
                _programSetup(false)
            {
            }
            ~BackgroundCleanerRenderer() {}

            /// setRenderingArea defines the rendering area.
            virtual void setRenderingArea(QRect renderingArea, int windowHeight) {
                _clearArea = std::move(renderingArea);
                _clearArea.moveTop(windowHeight - _clearArea.top() - _clearArea.height());
            }

        public slots:
            void paint() {
                initializeOpenGLFunctions();
                if (!_programSetup) {
                    _programSetup = true;
                    _programId = glCreateProgram();
                    glLinkProgram(_programId);
                    glUseProgram(_programId);
                } else {
                    // Resize the rendering area
                    glUseProgram(_programId);
                    glEnable(GL_SCISSOR_TEST);
                    glScissor(
                        static_cast<GLint>(_clearArea.left()),
                        static_cast<GLint>(_clearArea.top()),
                        static_cast<GLsizei>(_clearArea.width()),
                        static_cast<GLsizei>(_clearArea.height())
                    );
                    glClearColor(_color.redF(), _color.greenF(), _color.blueF(), _color.alphaF());
                    glClear(GL_COLOR_BUFFER_BIT);
                    glDisable(GL_SCISSOR_TEST);
                }
            }

        protected:
            QColor _color;
            QRect _clearArea;
            bool _programSetup;
            GLuint _programId;
    };

    /// BackgroundCleaner cleans the background for OpenGL renderers.
    class BackgroundCleaner : public QQuickItem {
        Q_OBJECT
        Q_PROPERTY(QColor color READ color WRITE setColor)
        public:
            BackgroundCleaner() :
                _colorSet(false)
            {
                connect(this, &QQuickItem::windowChanged, this, &BackgroundCleaner::handleWindowChanged);
            }

            /// setColor defines the clear color.
            /// The color will be passed to the openGL renderer, therefore it should only be set once.
            virtual void setColor(QColor color) {
                if (!_colorSet.load(std::memory_order_relaxed)) {
                    _color = color;
                    _colorSet.store(true, std::memory_order_release);
                }
            }

            /// color returns the currently used color.
            virtual QColor color() const {
                return _color;
            }

        public slots:

            /// sync addapts the renderer to external changes.
            void sync() {
                if (_colorSet.load(std::memory_order_acquire)) {
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
                    _backgroundCleanerRenderer->setRenderingArea(QRect(x(), y(), width(), height()), window()->height());
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

            /// handleWindowChanged is triggered after a window change.
            void handleWindowChanged(QQuickWindow* window) {
                if (window) {
                    connect(window, &QQuickWindow::beforeSynchronizing, this, &BackgroundCleaner::sync, Qt::DirectConnection);
                    connect(window, &QQuickWindow::sceneGraphInvalidated, this, &BackgroundCleaner::cleanup, Qt::DirectConnection);
                    window->setClearBeforeRendering(false);
                }
            }

        protected:
            QColor _color;
            std::atomic_bool _colorSet;
            std::unique_ptr<BackgroundCleanerRenderer> _backgroundCleanerRenderer;
    };
}
