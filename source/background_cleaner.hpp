#pragma once

#include <QQmlParserStatus>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFunctions_3_3_Core>
#include <QtQuick/QQuickItem>
#include <QtQuick/qquickwindow.h>
#include <atomic>
#include <memory>

/// chameleon provides Qt components for event stream display.
namespace chameleon {

    /// background_cleaner_renderer handles openGL calls for a background cleaner.
    class background_cleaner_renderer : public QObject, public QOpenGLFunctions_3_3_Core {
        Q_OBJECT
        public:
        background_cleaner_renderer(QColor color) : _color(color) {}
        background_cleaner_renderer(const background_cleaner_renderer&) = delete;
        background_cleaner_renderer(background_cleaner_renderer&&) = delete;
        background_cleaner_renderer& operator=(const background_cleaner_renderer&) = delete;
        background_cleaner_renderer& operator=(background_cleaner_renderer&&) = delete;
        virtual ~background_cleaner_renderer() {}

        /// set_rendering_area defines the clear area.
        virtual void set_rendering_area(QRectF clear_area, int window_height) {
            _clear_area = clear_area;
            _clear_area.moveTop(window_height - _clear_area.top() - _clear_area.height());
        }

        public slots:

        /// paint sends commands to the GPU.
        void paint() {
            if (!initializeOpenGLFunctions()) {
                throw std::runtime_error("initializing the OpenGL context failed");
            }
            glEnable(GL_SCISSOR_TEST);
            glScissor(
                static_cast<GLint>(_clear_area.left()),
                static_cast<GLint>(_clear_area.top()),
                static_cast<GLsizei>(_clear_area.width()),
                static_cast<GLsizei>(_clear_area.height()));
            glClearColor(
                static_cast<GLfloat>(_color.redF()),
                static_cast<GLfloat>(_color.greenF()),
                static_cast<GLfloat>(_color.blueF()),
                static_cast<GLfloat>(_color.alphaF()));
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            check_opengl_error();
        }

        protected:
        /// check_opengl_error throws if openGL generated an error.
        virtual void check_opengl_error() {
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
        QRectF _clear_area;
    };

    /// background_cleaner cleans the background for OpenGL renderers.
    class background_cleaner : public QQuickItem {
        Q_OBJECT
        Q_INTERFACES(QQmlParserStatus)
        Q_PROPERTY(QColor color READ color WRITE set_color)
        public:
        background_cleaner() : _ready(false), _color(Qt::black) {
            connect(this, &QQuickItem::windowChanged, this, &background_cleaner::handle_window_changed);
        }
        background_cleaner(const background_cleaner&) = delete;
        background_cleaner(background_cleaner&&) = delete;
        background_cleaner& operator=(const background_cleaner&) = delete;
        background_cleaner& operator=(background_cleaner&&) = delete;
        virtual ~background_cleaner() {}

        /// set_color defines the clear color.
        /// The color will be passed to the openGL renderer, therefore it should only be set during qml construction.
        virtual void set_color(QColor color) {
            if (_ready.load(std::memory_order_acquire)) {
                throw std::logic_error("color can only be set during qml construction");
            }
            _color = color;
        }

        /// color returns the currently used color.
        virtual QColor color() const {
            return _color;
        }

        /// componentComplete is called when all the qml values are bound.
        virtual void componentComplete() {
            _ready.store(true, std::memory_order_release);
        }

        public slots:

        /// sync adapts the renderer to external changes.
        void sync() {
            if (_ready.load(std::memory_order_acquire)) {
                if (!_background_cleaner_renderer) {
                    _background_cleaner_renderer =
                        std::unique_ptr<background_cleaner_renderer>(new background_cleaner_renderer(_color));
                    connect(
                        window(),
                        &QQuickWindow::beforeRendering,
                        _background_cleaner_renderer.get(),
                        &background_cleaner_renderer::paint,
                        Qt::DirectConnection);
                }
                auto clear_area =
                    QRectF(0, 0, width() * window()->devicePixelRatio(), height() * window()->devicePixelRatio());
                for (auto item = static_cast<QQuickItem*>(this); item; item = item->parentItem()) {
                    clear_area.moveLeft(clear_area.left() + item->x() * window()->devicePixelRatio());
                    clear_area.moveTop(clear_area.top() + item->y() * window()->devicePixelRatio());
                }
                if (clear_area != _clear_area) {
                    _clear_area = std::move(clear_area);
                    _background_cleaner_renderer->set_rendering_area(
                        _clear_area, window()->height() * window()->devicePixelRatio());
                }
            }
        }

        /// cleanup resets the renderer.
        void cleanup() {
            _background_cleaner_renderer.reset();
        }

        private slots:

        /// handle_window_changed must be triggered after a window change.
        void handle_window_changed(QQuickWindow* window) {
            if (window) {
                connect(
                    window, &QQuickWindow::beforeSynchronizing, this, &background_cleaner::sync, Qt::DirectConnection);
                connect(
                    window,
                    &QQuickWindow::sceneGraphInvalidated,
                    this,
                    &background_cleaner::cleanup,
                    Qt::DirectConnection);
                window->setClearBeforeRendering(false);
            }
        }

        protected:
        std::atomic_bool _ready;
        QColor _color;
        std::unique_ptr<background_cleaner_renderer> _background_cleaner_renderer;
        QRectF _clear_area;
    };
}
