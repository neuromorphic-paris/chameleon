#pragma once

#include <QQmlParserStatus>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFunctions_3_3_Core>
#include <QtQuick/QQuickItem>
#include <QtQuick/qquickwindow.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

/// chameleon provides Qt components for event stream display.
namespace chameleon {

    /// grey_display_renderer handles openGL calls for a grey_display.
    class grey_display_renderer : public QObject, public QOpenGLFunctions_3_3_Core {
        Q_OBJECT
        public:
        grey_display_renderer(QSize canvas_size) :
            _canvas_size(canvas_size),
            _exposures(_canvas_size.width() * _canvas_size.height(), 0),
            _program_setup(false) {
            _accessing_exposures.clear(std::memory_order_release);
        }
        grey_display_renderer(const grey_display_renderer&) = delete;
        grey_display_renderer(grey_display_renderer&&) = delete;
        grey_display_renderer& operator=(const grey_display_renderer&) = delete;
        grey_display_renderer& operator=(grey_display_renderer&&) = delete;
        virtual ~grey_display_renderer() {
            glDeleteBuffers(1, &_pbo_id);
            glDeleteTextures(1, &_texture_id);
            glDeleteBuffers(static_cast<GLsizei>(_vertex_buffers_ids.size()), _vertex_buffers_ids.data());
            glDeleteVertexArrays(1, &_vertex_array_id);
            glDeleteProgram(_program_id);
        }

        /// set_rendering_area defines the rendering area.
        virtual void set_rendering_area(QRectF clear_area, QRectF paint_area, int window_height) {
            _clear_area = clear_area;
            _clear_area.moveTop(window_height - _clear_area.top() - _clear_area.height());
            _paint_area = paint_area;
            _paint_area.moveTop(window_height - _paint_area.top() - _paint_area.height());
        }

        /// push adds an event to the display.
        template <typename Event>
        void push(Event event) {
            const auto index =
                static_cast<std::size_t>(event.x) + static_cast<std::size_t>(event.y) * _canvas_size.width();
            while (_accessing_exposures.test_and_set(std::memory_order_acquire)) {
            }
            _exposures[index] = static_cast<float>(event.exposure);
            _accessing_exposures.clear(std::memory_order_release);
        }

        /// assign sets all the pixels at once.
        template <typename Iterator>
        void assign(Iterator begin, Iterator end) {
            while (_accessing_exposures.test_and_set(std::memory_order_acquire)) {
            }
            _exposures.assign(begin, end);
            _accessing_exposures.clear(std::memory_order_release);
        }

        public slots:

        /// paint sends commands to the GPU.
        void paint() {
            if (!initializeOpenGLFunctions()) {
                throw std::runtime_error("initializing the OpenGL context failed");
            }
            if (!_program_setup) {
                _program_setup = true;

                // compile the vertex shader
                const auto vertex_shader_id = glCreateShader(GL_VERTEX_SHADER);
                {
                    const std::string vertex_shader(R""(
                        #version 330 core
                        in vec2 coordinates;
                        out vec2 uv;
                        uniform float width;
                        uniform float height;
                        void main() {
                            gl_Position = vec4(coordinates, 0.0, 1.0);
                            uv = vec2((coordinates.x + 1) / 2 * width, (coordinates.y + 1) / 2 * height);
                        }
                    )"");
                    auto vertex_shader_content = vertex_shader.c_str();
                    auto vertex_shader_size = vertex_shader.size();
                    glShaderSource(
                        vertex_shader_id,
                        1,
                        static_cast<const GLchar**>(&vertex_shader_content),
                        reinterpret_cast<const GLint*>(&vertex_shader_size));
                }
                glCompileShader(vertex_shader_id);
                check_shader_error(vertex_shader_id);

                // compile the fragment shader
                const auto fragment_shader_id = glCreateShader(GL_FRAGMENT_SHADER);
                {
                    const std::string fragment_shader(R""(
                        #version 330 core
                        in vec2 uv;
                        out vec4 color;
                        uniform sampler2DRect sampler;
                        void main() {
                            color = texture(sampler, uv).xxxw;
                        }
                    )"");
                    auto fragment_shader_content = fragment_shader.c_str();
                    auto fragment_shader_size = fragment_shader.size();
                    glShaderSource(
                        fragment_shader_id,
                        1,
                        static_cast<const GLchar**>(&fragment_shader_content),
                        reinterpret_cast<const GLint*>(&fragment_shader_size));
                }
                glCompileShader(fragment_shader_id);
                check_shader_error(fragment_shader_id);

                // create the shaders pipeline
                _program_id = glCreateProgram();
                glAttachShader(_program_id, vertex_shader_id);
                glAttachShader(_program_id, fragment_shader_id);
                glLinkProgram(_program_id);
                glDeleteShader(vertex_shader_id);
                glDeleteShader(fragment_shader_id);
                glUseProgram(_program_id);
                check_program_error(_program_id);

                // create the vertex array object
                glGenVertexArrays(1, &_vertex_array_id);
                glBindVertexArray(_vertex_array_id);
                glGenBuffers(static_cast<GLsizei>(_vertex_buffers_ids.size()), _vertex_buffers_ids.data());
                {
                    glBindBuffer(GL_ARRAY_BUFFER, std::get<0>(_vertex_buffers_ids));
                    std::array<float, 8> coordinates{-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f};
                    glBufferData(
                        GL_ARRAY_BUFFER,
                        coordinates.size() * sizeof(decltype(coordinates)::value_type),
                        coordinates.data(),
                        GL_STATIC_DRAW);
                    glEnableVertexAttribArray(glGetAttribLocation(_program_id, "coordinates"));
                    glVertexAttribPointer(glGetAttribLocation(_program_id, "coordinates"), 2, GL_FLOAT, GL_FALSE, 0, 0);
                }
                {
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, std::get<1>(_vertex_buffers_ids));
                    std::array<GLuint, 4> indices{0, 1, 2, 3};
                    glBufferData(
                        GL_ELEMENT_ARRAY_BUFFER,
                        indices.size() * sizeof(decltype(indices)::value_type),
                        indices.data(),
                        GL_STATIC_DRAW);
                }
                glBindVertexArray(0);

                // set the uniform values
                glUniform1f(glGetUniformLocation(_program_id, "width"), static_cast<GLfloat>(_canvas_size.width()));
                glUniform1f(glGetUniformLocation(_program_id, "height"), static_cast<GLfloat>(_canvas_size.height()));

                // create the texture
                glGenTextures(1, &_texture_id);
                glBindTexture(GL_TEXTURE_RECTANGLE, _texture_id);
                glTexImage2D(
                    GL_TEXTURE_RECTANGLE,
                    0,
                    GL_RED,
                    _canvas_size.width(),
                    _canvas_size.height(),
                    0,
                    GL_RED,
                    GL_FLOAT,
                    nullptr);
                glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindTexture(GL_TEXTURE_RECTANGLE, 0);

                // create the pbo
                glGenBuffers(1, &_pbo_id);
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _pbo_id);
                glBufferData(
                    GL_PIXEL_UNPACK_BUFFER,
                    _exposures.size() * sizeof(decltype(_exposures)::value_type),
                    nullptr,
                    GL_DYNAMIC_DRAW);
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            }

            // send data to the GPU
            glUseProgram(_program_id);
            glViewport(
                static_cast<GLint>(_paint_area.left()),
                static_cast<GLint>(_paint_area.top()),
                static_cast<GLsizei>(_paint_area.width()),
                static_cast<GLsizei>(_paint_area.height()));
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBindTexture(GL_TEXTURE_RECTANGLE, _texture_id);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _pbo_id);
            glTexSubImage2D(
                GL_TEXTURE_RECTANGLE, 0, 0, 0, _canvas_size.width(), _canvas_size.height(), GL_RED, GL_FLOAT, 0);
            {
                auto buffer = reinterpret_cast<float*>(glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY));
                if (!buffer) {
                    throw std::logic_error("glMapBuffer returned an null pointer");
                }
                while (_accessing_exposures.test_and_set(std::memory_order_acquire)) {
                }
                std::copy(_exposures.begin(), _exposures.end(), buffer);
                _accessing_exposures.clear(std::memory_order_release);
                glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
            }
            glBindVertexArray(_vertex_array_id);
            glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_INT, 0);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            glBindTexture(GL_TEXTURE_RECTANGLE, 0);
            glBindVertexArray(0);
            glUseProgram(0);
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

        /// check_shader_error checks for shader compilation errors.
        virtual void check_shader_error(GLuint shader_id) {
            GLint status = 0;
            glGetShaderiv(shader_id, GL_COMPILE_STATUS, &status);

            if (status != GL_TRUE) {
                GLint message_length = 0;
                glGetShaderiv(shader_id, GL_INFO_LOG_LENGTH, &message_length);
                std::vector<char> error_message(message_length);
                glGetShaderInfoLog(shader_id, message_length, nullptr, error_message.data());
                throw std::logic_error("Shader error: " + std::string(error_message.data()));
            }
        }

        /// check_program_error checks for program errors.
        virtual void check_program_error(GLuint program_id) {
            GLint status = 0;
            glGetProgramiv(program_id, GL_LINK_STATUS, &status);

            if (status != GL_TRUE) {
                GLint message_length = 0;
                glGetProgramiv(program_id, GL_INFO_LOG_LENGTH, &message_length);
                std::vector<char> error_message(message_length);
                glGetShaderInfoLog(program_id, message_length, nullptr, error_message.data());
                throw std::logic_error("program error: " + std::string(error_message.data()));
            }
        }

        QSize _canvas_size;
        std::vector<float> _exposures;
        std::atomic_flag _accessing_exposures;
        QRectF _clear_area;
        QRectF _paint_area;
        bool _program_setup;
        GLuint _program_id;
        GLuint _vertex_array_id;
        GLuint _texture_id;
        GLuint _pbo_id;
        std::array<GLuint, 2> _vertex_buffers_ids;
    };

    /// grey_display displays a stream of events without tone-mapping.
    class grey_display : public QQuickItem {
        Q_OBJECT
        Q_INTERFACES(QQmlParserStatus)
        Q_PROPERTY(QSize canvas_size READ canvas_size WRITE set_canvas_size)
        Q_PROPERTY(QRectF paint_area READ paint_area)
        public:
        grey_display() : _ready(false), _renderer_ready(false) {
            connect(this, &QQuickItem::windowChanged, this, &grey_display::handle_window_changed);
        }
        grey_display(const grey_display&) = delete;
        grey_display(grey_display&&) = delete;
        grey_display& operator=(const grey_display&) = delete;
        grey_display& operator=(grey_display&&) = delete;
        virtual ~grey_display() {}

        /// set_canvas_size defines the display coordinates.
        /// The canvas size will be passed to the openGL renderer, therefore it should only be set during qml
        /// construction.
        virtual void set_canvas_size(QSize canvas_size) {
            if (_ready.load(std::memory_order_acquire)) {
                throw std::logic_error("canvas_size can only be set during qml construction");
            }
            _canvas_size = canvas_size;
            setImplicitWidth(canvas_size.width());
            setImplicitHeight(canvas_size.height());
        }

        /// canvas_size returns the currently used canvas_size.
        virtual QSize canvas_size() const {
            return _canvas_size;
        }

        /// paint_area returns the paint area in window coordinates.
        virtual QRectF paint_area() const {
            return _paint_area;
        }

        /// push adds an event to the display.
        template <typename Event>
        void push(Event event) {
            while (!_renderer_ready.load(std::memory_order_acquire)) {
            }
            _grey_display_renderer->push<Event>(event);
        }

        /// assign sets all the pixels at once.
        template <typename Iterator>
        void assign(Iterator begin, Iterator end) {
            while (!_renderer_ready.load(std::memory_order_acquire)) {
            }
            _grey_display_renderer->assign<Iterator>(begin, end);
        }

        /// componentComplete is called when all the qml values are bound.
        virtual void componentComplete() {
            if (_canvas_size.width() <= 0 || _canvas_size.height() <= 0) {
                throw std::logic_error("canvas_size cannot have a null component, make sure that it is set in qml");
            }
            _ready.store(true, std::memory_order_release);
        }

        signals:

        /// paintAreaChanged notifies a paint area change.
        void paintAreaChanged(QRectF paint_area);

        public slots:

        /// sync adapts the renderer to external changes.
        void sync() {
            if (_ready.load(std::memory_order_acquire)) {
                if (!_grey_display_renderer) {
                    _grey_display_renderer =
                        std::unique_ptr<grey_display_renderer>(new grey_display_renderer(_canvas_size));
                    connect(
                        window(),
                        &QQuickWindow::beforeRendering,
                        _grey_display_renderer.get(),
                        &grey_display_renderer::paint,
                        Qt::DirectConnection);
                    _renderer_ready.store(true, std::memory_order_release);
                }
                auto clear_area =
                    QRectF(0, 0, width() * window()->devicePixelRatio(), height() * window()->devicePixelRatio());
                for (auto item = static_cast<QQuickItem*>(this); item; item = item->parentItem()) {
                    clear_area.moveLeft(clear_area.left() + item->x() * window()->devicePixelRatio());
                    clear_area.moveTop(clear_area.top() + item->y() * window()->devicePixelRatio());
                }
                if (clear_area != _clear_area) {
                    _clear_area = std::move(clear_area);
                    if (clear_area.width() * _canvas_size.height() > clear_area.height() * _canvas_size.width()) {
                        _paint_area.setWidth(clear_area.height() * _canvas_size.width() / _canvas_size.height());
                        _paint_area.setHeight(clear_area.height());
                        _paint_area.moveLeft(clear_area.left() + (clear_area.width() - _paint_area.width()) / 2);
                        _paint_area.moveTop(clear_area.top());
                    } else {
                        _paint_area.setWidth(clear_area.width());
                        _paint_area.setHeight(clear_area.width() * _canvas_size.height() / _canvas_size.width());
                        _paint_area.moveLeft(clear_area.left());
                        _paint_area.moveTop(clear_area.top() + (clear_area.height() - _paint_area.height()) / 2);
                    }
                    _grey_display_renderer->set_rendering_area(
                        _clear_area, _paint_area, window()->height() * window()->devicePixelRatio());
                    paintAreaChanged(_paint_area);
                }
            }
        }

        /// cleanup frees the owned renderer.
        void cleanup() {
            _grey_display_renderer.reset();
        }

        /// trigger_draw requests a window refresh.
        void trigger_draw() {
            if (window()) {
                window()->update();
            }
        }

        private slots:

        /// handle_window_changed must be triggered after a window transformation.
        void handle_window_changed(QQuickWindow* window) {
            if (window) {
                connect(window, &QQuickWindow::beforeSynchronizing, this, &grey_display::sync, Qt::DirectConnection);
                connect(
                    window, &QQuickWindow::sceneGraphInvalidated, this, &grey_display::cleanup, Qt::DirectConnection);
                window->setClearBeforeRendering(false);
            }
        }

        protected:
        std::atomic_bool _ready;
        std::atomic_bool _renderer_ready;
        QSize _canvas_size;
        std::unique_ptr<grey_display_renderer> _grey_display_renderer;
        QRectF _clear_area;
        QRectF _paint_area;
    };
}
