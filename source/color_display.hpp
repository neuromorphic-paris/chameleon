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

    /// color_display_renderer handles openGL calls for a color_display.
    class color_display_renderer : public QObject, public QOpenGLFunctions_3_3_Core {
        Q_OBJECT
        public:
        color_display_renderer(QSize canvas_size, QColor background_color) :
            _canvas_size(canvas_size),
            _background_color(background_color),
            _indices(_canvas_size.width() * _canvas_size.height()),
            _colors(_canvas_size.width() * _canvas_size.height() * 3, 0),
            _duplicated_colors(_canvas_size.width() * _canvas_size.height() * 3),
            _program_setup(false) {
            for (auto index_iterator = _indices.begin(); index_iterator != _indices.end(); ++index_iterator) {
                *index_iterator = static_cast<qint32>(std::distance(_indices.begin(), index_iterator));
            }
            _coordinates.reserve(_canvas_size.width() * _canvas_size.height() * 2);
            for (qint32 y = 0; y < _canvas_size.height(); ++y) {
                for (qint32 x = 0; x < _canvas_size.width(); ++x) {
                    _coordinates.push_back(static_cast<float>(x));
                    _coordinates.push_back(static_cast<float>(y));
                }
            }
            _accessing_colors.clear(std::memory_order_release);
        }
        color_display_renderer(const color_display_renderer&) = delete;
        color_display_renderer(color_display_renderer&&) = default;
        color_display_renderer& operator=(const color_display_renderer&) = delete;
        color_display_renderer& operator=(color_display_renderer&&) = default;
        virtual ~color_display_renderer() {
            glDeleteBuffers(2, _vertex_buffers_ids.data());
            glDeleteVertexArrays(1, &_vertex_array_id);
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
                (static_cast<std::size_t>(event.x) + static_cast<std::size_t>(event.y) * _canvas_size.width()) * 3;
            while (_accessing_colors.test_and_set(std::memory_order_acquire)) {
            }
            _colors[index] = static_cast<float>(event.r);
            _colors[index + 1] = static_cast<float>(event.g);
            _colors[index + 2] = static_cast<float>(event.b);
            _accessing_colors.clear(std::memory_order_release);
        }

        /// assign sets all the pixels at once.
        template <typename Iterator>
        void assign(Iterator begin, Iterator end) {
            std::size_t index = 0;
            while (_accessing_colors.test_and_set(std::memory_order_acquire)) {
            }
            for (; begin != end; ++begin) {
                _colors[index] = static_cast<float>(begin->r);
                _colors[index + 1] = static_cast<float>(begin->g);
                _colors[index + 2] = static_cast<float>(begin->g);
                index += 3;
            }
            _accessing_colors.clear(std::memory_order_release);
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
                        in vec3 color;
                        out vec4 geometry_color;
                        uniform float width;
                        uniform float height;
                        void main() {
                            gl_Position =
                                vec4(coordinates.x / width * 2.0 - 1.0, coordinates.y / height * 2.0 - 1.0, 0.0, 1.0);
                            geometry_color = vec4(color[0], color[1], color[2], 1.0);
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

                // compile the geometry shader
                const auto geometry_shader_id = glCreateShader(GL_GEOMETRY_SHADER);
                {
                    const std::string geometry_shader(R""(
                        #version 330 core
                        layout (points) in;
                        layout (triangle_strip, max_vertices = 4) out;
                        in vec4 geometry_color[];
                        out vec4 fragment_color;
                        uniform float width;
                        uniform float height;
                        void main() {
                            fragment_color = geometry_color[0];
                            float pixel_width = 2.0 / width;
                            float pixel_height = 2.0 / height;
                            gl_Position = vec4(gl_in[0].gl_Position.x, gl_in[0].gl_Position.y, 0.0, 1.0);
                            EmitVertex();
                            gl_Position = vec4(gl_in[0].gl_Position.x, gl_in[0].gl_Position.y + pixel_height, 0.0, 1.0);
                            EmitVertex();
                            gl_Position = vec4(gl_in[0].gl_Position.x + pixel_width, gl_in[0].gl_Position.y, 0.0, 1.0);
                            EmitVertex();
                            gl_Position = vec4(
                                gl_in[0].gl_Position.x + pixel_width, gl_in[0].gl_Position.y + pixel_height, 0.0, 1.0);
                            EmitVertex();
                        }
                    )"");
                    auto geometry_shader_content = geometry_shader.c_str();
                    auto geometry_shader_size = geometry_shader.size();
                    glShaderSource(
                        geometry_shader_id,
                        1,
                        static_cast<const GLchar**>(&geometry_shader_content),
                        reinterpret_cast<const GLint*>(&geometry_shader_size));
                }
                glCompileShader(geometry_shader_id);
                check_shader_error(geometry_shader_id);

                // compile the fragment shader
                const auto fragment_shader_id = glCreateShader(GL_FRAGMENT_SHADER);
                {
                    const std::string fragment_shader(R""(
                            #version 330 core
                            in vec4 fragment_color;
                            out vec4 color;
                            void main() {
                                color = fragment_color;
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
                glAttachShader(_program_id, geometry_shader_id);
                glAttachShader(_program_id, fragment_shader_id);
                glLinkProgram(_program_id);
                glDeleteShader(vertex_shader_id);
                glDeleteShader(geometry_shader_id);
                glDeleteShader(fragment_shader_id);
                glUseProgram(_program_id);
                check_program_error(_program_id);

                // create the vertex buffer objects
                glGenBuffers(3, _vertex_buffers_ids.data());
                glBindBuffer(GL_ARRAY_BUFFER, std::get<0>(_vertex_buffers_ids));
                glBufferData(
                    GL_ARRAY_BUFFER,
                    _coordinates.size() * sizeof(decltype(_coordinates)::value_type),
                    _coordinates.data(),
                    GL_STATIC_DRAW);
                glBindBuffer(GL_ARRAY_BUFFER, std::get<1>(_vertex_buffers_ids));
                glBufferData(
                    GL_ARRAY_BUFFER,
                    _duplicated_colors.size() * sizeof(decltype(_duplicated_colors)::value_type),
                    _duplicated_colors.data(),
                    GL_DYNAMIC_DRAW);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, std::get<2>(_vertex_buffers_ids));
                glBufferData(
                    GL_ELEMENT_ARRAY_BUFFER,
                    _indices.size() * sizeof(decltype(_indices)::value_type),
                    _indices.data(),
                    GL_STATIC_DRAW);

                // create the vertex array object
                glGenVertexArrays(1, &_vertex_array_id);
                glBindVertexArray(_vertex_array_id);
                glBindBuffer(GL_ARRAY_BUFFER, std::get<0>(_vertex_buffers_ids));
                glEnableVertexAttribArray(glGetAttribLocation(_program_id, "coordinates"));
                glVertexAttribPointer(glGetAttribLocation(_program_id, "coordinates"), 2, GL_FLOAT, GL_FALSE, 0, 0);
                glBindBuffer(GL_ARRAY_BUFFER, std::get<1>(_vertex_buffers_ids));
                glEnableVertexAttribArray(glGetAttribLocation(_program_id, "color"));
                glVertexAttribPointer(glGetAttribLocation(_program_id, "color"), 3, GL_FLOAT, GL_FALSE, 0, 0);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, std::get<2>(_vertex_buffers_ids));
                glBindVertexArray(0);

                // set the uniform values
                glUniform1f(glGetUniformLocation(_program_id, "width"), static_cast<GLfloat>(_canvas_size.width()));
                glUniform1f(glGetUniformLocation(_program_id, "height"), static_cast<GLfloat>(_canvas_size.height()));
            } else {
                // copy the events to minimise the strain on the event pipeline
                while (_accessing_colors.test_and_set(std::memory_order_acquire)) {
                }
                std::copy(_colors.begin(), _colors.end(), _duplicated_colors.begin());
                _accessing_colors.clear(std::memory_order_release);

                // send data to the GPU
                glUseProgram(_program_id);
                glBindBuffer(GL_ARRAY_BUFFER, std::get<1>(_vertex_buffers_ids));
                glBufferData(
                    GL_ARRAY_BUFFER,
                    _duplicated_colors.size() * sizeof(decltype(_duplicated_colors)::value_type),
                    nullptr,
                    GL_DYNAMIC_DRAW);
                glBufferSubData(
                    GL_ARRAY_BUFFER,
                    0,
                    _duplicated_colors.size() * sizeof(decltype(_duplicated_colors)::value_type),
                    _duplicated_colors.data());

                // resize the rendering area
                glUseProgram(_program_id);
                glEnable(GL_SCISSOR_TEST);
                glScissor(
                    static_cast<GLint>(_clear_area.left()),
                    static_cast<GLint>(_clear_area.top()),
                    static_cast<GLsizei>(_clear_area.width()),
                    static_cast<GLsizei>(_clear_area.height()));
                glClearColor(
                    static_cast<GLfloat>(_background_color.redF()),
                    static_cast<GLfloat>(_background_color.greenF()),
                    static_cast<GLfloat>(_background_color.blueF()),
                    static_cast<GLfloat>(_background_color.alphaF()));
                glClear(GL_COLOR_BUFFER_BIT);
                glDisable(GL_SCISSOR_TEST);
                glViewport(
                    static_cast<GLint>(_paint_area.left()),
                    static_cast<GLint>(_paint_area.top()),
                    static_cast<GLsizei>(_paint_area.width()),
                    static_cast<GLsizei>(_paint_area.height()));

                // send varying data to the GPU
                glBindVertexArray(_vertex_array_id);
                glDrawElements(GL_POINTS, static_cast<GLsizei>(_indices.size()), GL_UNSIGNED_INT, nullptr);
                glBindVertexArray(0);
            }
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
        QColor _background_color;
        std::vector<GLuint> _indices;
        std::vector<float> _coordinates;
        std::vector<float> _colors;
        std::vector<float> _duplicated_colors;
        std::atomic_flag _accessing_colors;
        QRectF _clear_area;
        QRectF _paint_area;
        bool _program_setup;
        GLuint _program_id;
        GLuint _vertex_array_id;
        std::array<GLuint, 3> _vertex_buffers_ids;
    };

    /// color_display displays a stream of color events without tone-mapping.
    class color_display : public QQuickItem {
        Q_OBJECT
        Q_INTERFACES(QQmlParserStatus)
        Q_PROPERTY(QSize canvas_size READ canvas_size WRITE set_canvas_size)
        Q_PROPERTY(QColor background_color READ background_color WRITE set_background_color)
        Q_PROPERTY(QRectF paint_area READ paint_area)
        public:
        color_display() : _ready(false), _renderer_ready(false), _background_color(Qt::black) {
            connect(this, &QQuickItem::windowChanged, this, &color_display::handle_window_changed);
        }
        color_display(const color_display&) = delete;
        color_display(color_display&&) = default;
        color_display& operator=(const color_display&) = delete;
        color_display& operator=(color_display&&) = default;
        virtual ~color_display() {}

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

        /// set_background_color defines the background color used to compensate the parent's shape.
        /// The background color will be passed to the openGL renderer, therefore it should only be set during qml
        /// construction.
        virtual void set_background_color(QColor background_color) {
            if (_ready.load(std::memory_order_acquire)) {
                throw std::logic_error("background_color can only be set during qml construction");
            }
            _background_color = background_color;
        }

        /// background_color returns the currently used background_color.
        virtual QColor background_color() const {
            return _background_color;
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
            _color_display_renderer->push<Event>(event);
        }

        /// assign sets all the pixels at once.
        template <typename Iterator>
        void assign(Iterator begin, Iterator end) {
            while (!_renderer_ready.load(std::memory_order_acquire)) {
            }
            _color_display_renderer->assign<Iterator>(begin, end);
        }

        /// componentComplete is called when all the qml values are bound.
        virtual void componentComplete() override {
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
            if (_ready.load(std::memory_order_relaxed)) {
                if (!_color_display_renderer) {
                    _color_display_renderer = std::unique_ptr<color_display_renderer>(
                        new color_display_renderer(_canvas_size, _background_color));
                    connect(
                        window(),
                        &QQuickWindow::beforeRendering,
                        _color_display_renderer.get(),
                        &color_display_renderer::paint,
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
                    _color_display_renderer->set_rendering_area(
                        _clear_area, _paint_area, window()->height() * window()->devicePixelRatio());
                    paintAreaChanged(_paint_area);
                }
            }
        }

        /// cleanup frees the owned renderer.
        void cleanup() {
            _color_display_renderer.reset();
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
                connect(window, &QQuickWindow::beforeSynchronizing, this, &color_display::sync, Qt::DirectConnection);
                connect(
                    window, &QQuickWindow::sceneGraphInvalidated, this, &color_display::cleanup, Qt::DirectConnection);
                window->setClearBeforeRendering(false);
            }
        }

        protected:
        std::atomic_bool _ready;
        std::atomic_bool _renderer_ready;
        QSize _canvas_size;
        QColor _background_color;
        std::unique_ptr<color_display_renderer> _color_display_renderer;
        QRectF _clear_area;
        QRectF _paint_area;
    };
}
