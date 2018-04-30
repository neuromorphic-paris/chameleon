#pragma once

#include <QQmlParserStatus>
#include <QtCore/QDir>
#include <QtGui/QImage>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFunctions_3_3_Core>
#include <QtQuick/QQuickItem>
#include <QtQuick/qquickwindow.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <stdexcept>

/// chameleon provides Qt components for event stream display.
namespace chameleon {

    /// frame_generator_renderer handles openGL calls for a frame_generator.
    class frame_generator_renderer : public QObject, public QOpenGLFunctions_3_3_Core {
        Q_OBJECT
        public:
        frame_generator_renderer() : _program_setup(false), _before_rendering_done(false), _closing(false) {
            _rendering_not_required.clear(std::memory_order_release);
        }
        frame_generator_renderer(const frame_generator_renderer&) = delete;
        frame_generator_renderer(frame_generator_renderer&&) = default;
        frame_generator_renderer& operator=(const frame_generator_renderer&) = delete;
        frame_generator_renderer& operator=(frame_generator_renderer&&) = default;
        virtual ~frame_generator_renderer() {}

        /// set_rendering_area defines the rendering area.
        virtual void set_rendering_area(QRectF capture_area, int window_height) {
            _capture_area = capture_area;
            _capture_area.moveTop(window_height - _capture_area.top() - _capture_area.height());
        }

        /// save_frame_to waits for a complete render, takes a screenshots and writes it to a file.
        virtual bool save_frame_to(const QString& filename) {
            _rendering_not_required.clear(std::memory_order_release);
            _pixels_mutex.lock();
            if (_closing) {
                _pixels_mutex.unlock();
                return true;
            }
            _pixels_mutex.unlock();
            std::unique_lock<std::mutex> lock(_pixels_mutex);
            _pixels_updated.wait(lock);
            auto success = true;
            if (!_closing) {
                success = QImage(
                              _pixels.data(),
                              static_cast<int>(_image_width),
                              static_cast<int>(_image_height),
                              static_cast<int>(4 * _image_width),
                              QImage::Format_RGBA8888_Premultiplied)
                              .mirrored()
                              .save(filename);
            }
            lock.unlock();
            return success;
        }

        public slots:

        /// before_rendering_callback must be called when a rendering starts.
        void before_rendering_callback() {
            if (!_rendering_not_required.test_and_set(std::memory_order_relaxed)) {
                _before_rendering_done = true;
            }
        }

        /// after_rendering_callback is called when a rendering ends.
        void after_rendering_callback() {
            if (!initializeOpenGLFunctions()) {
                throw std::runtime_error("initializing the OpenGL context failed");
            }
            if (!_program_setup) {
                _program_setup = true;
                _program_id = glCreateProgram();
                glLinkProgram(_program_id);
            } else {
                if (_before_rendering_done) {
                    _before_rendering_done = false;
                    glUseProgram(_program_id);
                    glEnable(GL_SCISSOR_TEST);
                    {
                        const std::lock_guard<std::mutex> lock(_pixels_mutex);
                        _pixels.resize(_capture_area.width() * _capture_area.height() * 4);
                        glReadPixels(
                            static_cast<GLint>(_capture_area.left()),
                            static_cast<GLint>(_capture_area.top()),
                            static_cast<GLsizei>(_capture_area.width()),
                            static_cast<GLsizei>(_capture_area.height()),
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            _pixels.data());
                        _image_width = _capture_area.width();
                        _image_height = _capture_area.height();
                    }
                    _pixels_updated.notify_one();
                    glDisable(GL_SCISSOR_TEST);
                }
            }
        }

        /// closing is called when the window is about to be closed.
        void closing() {
            {
                const std::lock_guard<std::mutex> lock(_pixels_mutex);
                _closing = true;
            }
            _pixels_updated.notify_one();
        }

        protected:
        QRectF _capture_area;
        bool _program_setup;
        GLuint _program_id;
        std::atomic_flag _rendering_not_required;
        bool _before_rendering_done;
        std::vector<unsigned char> _pixels;
        std::mutex _pixels_mutex;
        std::condition_variable _pixels_updated;
        std::size_t _image_width;
        std::size_t _image_height;
        bool _closing;
    };

    /// frame_generator takes screenshots of the window.
    class frame_generator : public QQuickItem {
        Q_OBJECT
        public:
        frame_generator() : _closing(false), _renderer_ready(false) {
            connect(this, &QQuickItem::windowChanged, this, &frame_generator::handle_window_changed);
        }
        frame_generator(const frame_generator&) = delete;
        frame_generator(frame_generator&&) = default;
        frame_generator& operator=(const frame_generator&) = delete;
        frame_generator& operator=(frame_generator&&) = default;
        virtual ~frame_generator() {}

        /// save_frame_to triggers a frame render and stores the resulting png image to the given file.
        virtual void save_frame_to(const std::string& filename) {
            while (!_renderer_ready.load(std::memory_order_acquire)) {
            }
            if (!_closing.load(std::memory_order_relaxed)) {
                if (!_frame_generator_renderer->save_frame_to(QString::fromStdString(filename))) {
                    throw std::runtime_error(std::string("saving a frame to '") + filename + "' failed");
                }
            }
        }

        public slots:

        /// sync addapts the renderer to external changes.
        void sync() {
            if (!_frame_generator_renderer) {
                _frame_generator_renderer = std::unique_ptr<frame_generator_renderer>(new frame_generator_renderer());
                connect(
                    window(),
                    SIGNAL(closing(QQuickCloseEvent*)),
                    _frame_generator_renderer.get(),
                    SLOT(closing()),
                    Qt::DirectConnection);
                connect(window(), SIGNAL(closing(QQuickCloseEvent*)), this, SLOT(closing()), Qt::DirectConnection);
                connect(
                    window(),
                    &QQuickWindow::beforeRendering,
                    _frame_generator_renderer.get(),
                    &frame_generator_renderer::before_rendering_callback,
                    Qt::DirectConnection);
                connect(
                    window(),
                    &QQuickWindow::afterRendering,
                    _frame_generator_renderer.get(),
                    &frame_generator_renderer::after_rendering_callback,
                    Qt::DirectConnection);
                _renderer_ready.store(true, std::memory_order_release);
            }

            auto capture_area =
                QRectF(0, 0, width() * window()->devicePixelRatio(), height() * window()->devicePixelRatio());
            for (auto item = static_cast<QQuickItem*>(this); item; item = item->parentItem()) {
                capture_area.moveLeft(capture_area.left() + item->x() * window()->devicePixelRatio());
                capture_area.moveTop(capture_area.top() + item->y() * window()->devicePixelRatio());
            }
            if (capture_area != _capture_area) {
                _capture_area = std::move(capture_area);
                _frame_generator_renderer->set_rendering_area(
                    _capture_area, window()->height() * window()->devicePixelRatio());
            }
        }

        /// cleanup frees the owned renderer.
        void cleanup() {
            _frame_generator_renderer.reset();
        }

        /// trigger_draw requests a window refresh.
        void trigger_draw() {
            if (window()) {
                window()->update();
            }
        }

        /// closing is called when the window is about to be closed.
        void closing() {
            _closing.store(true, std::memory_order_release);
        }

        private slots:

        /// handle_window_changed is triggered after a window change.
        void handle_window_changed(QQuickWindow* window) {
            if (window) {
                connect(window, &QQuickWindow::beforeSynchronizing, this, &frame_generator::sync, Qt::DirectConnection);
                connect(
                    window,
                    &QQuickWindow::sceneGraphInvalidated,
                    this,
                    &frame_generator::cleanup,
                    Qt::DirectConnection);
                window->setClearBeforeRendering(false);
            }
        }

        protected:
        std::atomic_bool _closing;
        std::atomic_bool _renderer_ready;
        std::unique_ptr<frame_generator_renderer> _frame_generator_renderer;
        QRectF _capture_area;
    };
}
