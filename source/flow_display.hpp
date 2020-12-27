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

    /// flow_display_renderer handles openGL calls for a flow_display.
    class flow_display_renderer : public QObject, public QOpenGLFunctions_3_3_Core {
        Q_OBJECT
        public:
        flow_display_renderer(QSize canvas_size, float speed_to_length, float decay) :
            _canvas_size(canvas_size), _speed_to_length(speed_to_length), _decay(decay), _program_setup(false) {
            _indices.reserve(_canvas_size.width() * _canvas_size.height());
            _coordinates.reserve(_canvas_size.width() * _canvas_size.height() * 2);
            _ts_and_flows.reserve(_canvas_size.width() * _canvas_size.height() * 3);
            for (qint32 y = 0; y < _canvas_size.height(); ++y) {
                for (qint32 x = 0; x < _canvas_size.width(); ++x) {
                    _indices.push_back(x + y * _canvas_size.width());
                    _coordinates.push_back(static_cast<float>(x));
                    _coordinates.push_back(static_cast<float>(y));
                    _ts_and_flows.push_back(-std::numeric_limits<float>::infinity());
                    _ts_and_flows.push_back(static_cast<float>(0.0));
                    _ts_and_flows.push_back(static_cast<float>(0.0));
                }
            }

            _accessing_flows.clear(std::memory_order_release);
        }
        flow_display_renderer(const flow_display_renderer&) = delete;
        flow_display_renderer(flow_display_renderer&&) = delete;
        flow_display_renderer& operator=(const flow_display_renderer&) = delete;
        flow_display_renderer& operator=(flow_display_renderer&&) = delete;
        virtual ~flow_display_renderer() {
            glDeleteBuffers(static_cast<GLsizei>(_vertex_buffers_ids.size()), _vertex_buffers_ids.data());
            glDeleteVertexArrays(1, &_vertex_array_id);
            glDeleteProgram(_program_id);
        }

        /// set_rendering_area defines the rendering area.
        virtual void set_rendering_area(QRectF paint_area, int window_height) {
            _paint_area = paint_area;
            _paint_area.moveTop(window_height - _paint_area.top() - _paint_area.height());
        }

        /// push adds an event to the display.
        template <typename Event>
        void push(Event event) {
            const auto index =
                (static_cast<std::size_t>(event.x) + static_cast<std::size_t>(event.y) * _canvas_size.width()) * 3;
            while (_accessing_flows.test_and_set(std::memory_order_acquire)) {
            }
            _current_t = event.t;
            _ts_and_flows[index] = static_cast<float>(event.t);
            _ts_and_flows[index + 1] = static_cast<float>(event.vx);
            _ts_and_flows[index + 2] = static_cast<float>(event.vy);
            _accessing_flows.clear(std::memory_order_release);
        }

        /// assign sets all the pixels at once.
        template <typename Iterator>
        void assign(Iterator begin, Iterator end) {
            std::size_t index = 0;
            while (_accessing_flows.test_and_set(std::memory_order_acquire)) {
            }
            for (; begin != end; ++begin) {
                if (begin->t > _current_t) {
                    _current_t = begin->t;
                }
                _ts_and_flows[index] = static_cast<float>(begin->t);
                _ts_and_flows[index + 1] = static_cast<float>(begin->vx);
                _ts_and_flows[index + 2] = static_cast<float>(begin->vy);
                index += 3;
            }
            _accessing_flows.clear(std::memory_order_release);
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
                        in vec3 t_and_flow;
                        out vec3 geometry_t_and_flow;
                        uniform float width;
                        uniform float height;
                        void main() {
                            gl_Position = vec4(coordinates.x, coordinates.y, 0.0, 1.0);
                            geometry_t_and_flow = t_and_flow;
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
                        #define flow_display_pi 3.1415926535897932384626433832795
                        layout(points) in;
                        layout(line_strip, max_vertices = 2) out;
                        in vec3 geometry_t_and_flow[];
                        out vec4 fragment_color;
                        uniform float width;
                        uniform float height;
                        uniform float speed_to_length;
                        uniform float decay;
                        uniform float current_t;
                        const vec3 color_table[7] = vec3[](
                            vec3(1.0, 1.0, 0.0),
                            vec3(0.0, 1.0, 0.0),
                            vec3(0.0, 1.0, 1.0),
                            vec3(0.0, 0.0, 1.0),
                            vec3(1.0, 0.0, 1.0),
                            vec3(1.0, 0.0, 0.0),
                            vec3(1.0, 1.0, 0.0));
                        void main() {
                            if (geometry_t_and_flow[0].x > current_t) {
                                return;
                            }
                            vec2 speed_vector = vec2(geometry_t_and_flow[0].y, geometry_t_and_flow[0].z)
                                                * speed_to_length;
                            float speed = length(speed_vector);
                            if (speed == 0) {
                                return;
                            }
                            float alpha = exp(-(current_t - geometry_t_and_flow[0].x) / decay);
                            float float_index =
                                clamp(atan(speed_vector.y, speed_vector.x) / (2 * flow_display_pi) + 0.5, 0.0, 1.0)
                                * 6.0;
                            int integer_index = int(float_index);
                            if (float_index == integer_index) {
                                fragment_color = vec4(color_table[integer_index], alpha);
                            } else {
                                fragment_color = vec4(
                                    mix(color_table[integer_index],
                                        color_table[integer_index + 1],
                                        float_index - integer_index),
                                    alpha);
                            }
                            vec2 origin = vec2(gl_in[0].gl_Position.x + 0.5, gl_in[0].gl_Position.y + 0.5);
                            vec2 tip = origin + speed_vector;
                            gl_Position = vec4(origin.x / width * 2.0 - 1.0, origin.y / height * 2.0 - 1.0, 0.0, 1.0);
                            EmitVertex();
                            gl_Position = vec4(tip.x / width * 2.0 - 1.0, tip.y / height * 2.0 - 1.0, 0.0, 1.0);
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

                // create the vertex buffer and array objects
                glGenBuffers(static_cast<GLsizei>(_vertex_buffers_ids.size()), _vertex_buffers_ids.data());
                glBindBuffer(GL_ARRAY_BUFFER, std::get<0>(_vertex_buffers_ids));
                glBufferData(
                    GL_ARRAY_BUFFER,
                    _coordinates.size() * sizeof(decltype(_coordinates)::value_type),
                    _coordinates.data(),
                    GL_STATIC_DRAW);
                glBindBuffer(GL_ARRAY_BUFFER, std::get<1>(_vertex_buffers_ids));
                glBufferData(
                    GL_ARRAY_BUFFER,
                    _ts_and_flows.size() * sizeof(decltype(_ts_and_flows)::value_type),
                    nullptr,
                    GL_DYNAMIC_DRAW);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, std::get<2>(_vertex_buffers_ids));
                glBufferData(
                    GL_ELEMENT_ARRAY_BUFFER,
                    _indices.size() * sizeof(decltype(_indices)::value_type),
                    _indices.data(),
                    GL_STATIC_DRAW);
                glGenVertexArrays(1, &_vertex_array_id);
                glBindVertexArray(_vertex_array_id);
                glBindBuffer(GL_ARRAY_BUFFER, std::get<0>(_vertex_buffers_ids));
                glEnableVertexAttribArray(glGetAttribLocation(_program_id, "coordinates"));
                glVertexAttribPointer(glGetAttribLocation(_program_id, "coordinates"), 2, GL_FLOAT, GL_FALSE, 0, 0);
                glBindBuffer(GL_ARRAY_BUFFER, std::get<1>(_vertex_buffers_ids));
                glEnableVertexAttribArray(glGetAttribLocation(_program_id, "t_and_flow"));
                glVertexAttribPointer(glGetAttribLocation(_program_id, "t_and_flow"), 3, GL_FLOAT, GL_FALSE, 0, 0);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, std::get<2>(_vertex_buffers_ids));
                glBindVertexArray(0);

                // set uniform values
                glUniform1f(glGetUniformLocation(_program_id, "width"), static_cast<GLfloat>(_canvas_size.width()));
                glUniform1f(glGetUniformLocation(_program_id, "height"), static_cast<GLfloat>(_canvas_size.height()));
                glUniform1f(
                    glGetUniformLocation(_program_id, "speed_to_length"), static_cast<GLfloat>(_speed_to_length));
                glUniform1f(glGetUniformLocation(_program_id, "decay"), static_cast<GLfloat>(_decay));
                _current_t_location = glGetUniformLocation(_program_id, "current_t");
            }

            // send data to the GPU
            std::vector<float> local_ts_and_flows(_ts_and_flows.size());
            while (_accessing_flows.test_and_set(std::memory_order_acquire)) {
            }
            const auto local_current_t = _current_t;
            std::copy(_ts_and_flows.begin(), _ts_and_flows.end(), local_ts_and_flows.begin());
            _accessing_flows.clear(std::memory_order_release);
            glUseProgram(_program_id);
            glViewport(
                static_cast<GLint>(_paint_area.left()),
                static_cast<GLint>(_paint_area.top()),
                static_cast<GLsizei>(_paint_area.width()),
                static_cast<GLsizei>(_paint_area.height()));
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBindBuffer(GL_ARRAY_BUFFER, std::get<1>(_vertex_buffers_ids));
            glBufferData(
                GL_ARRAY_BUFFER,
                local_ts_and_flows.size() * sizeof(decltype(local_ts_and_flows)::value_type),
                nullptr,
                GL_DYNAMIC_DRAW);
            glBufferSubData(
                GL_ARRAY_BUFFER,
                0,
                local_ts_and_flows.size() * sizeof(decltype(local_ts_and_flows)::value_type),
                local_ts_and_flows.data());
            glUniform1f(_current_t_location, static_cast<GLfloat>(local_current_t));
            glBindVertexArray(_vertex_array_id);
            glDrawElements(GL_POINTS, static_cast<GLsizei>(_indices.size()), GL_UNSIGNED_INT, nullptr);
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
        float _speed_to_length;
        float _decay;
        float _current_t;
        std::vector<GLuint> _indices;
        std::vector<float> _coordinates;
        std::vector<float> _ts_and_flows;
        std::atomic_flag _accessing_flows;
        QRectF _paint_area;
        bool _program_setup;
        GLuint _program_id;
        GLuint _vertex_array_id;
        std::array<GLuint, 3> _vertex_buffers_ids;
        GLuint _current_t_location;
    };

    /// flow_display displays a stream of flow events.
    class flow_display : public QQuickItem {
        Q_OBJECT
        Q_INTERFACES(QQmlParserStatus)
        Q_PROPERTY(QSize canvas_size READ canvas_size WRITE set_canvas_size)
        Q_PROPERTY(float speed_to_length READ speed_to_length WRITE set_speed_to_length)
        Q_PROPERTY(float decay READ decay WRITE set_decay)
        public:
        flow_display() : _ready(false), _renderer_ready(false), _speed_to_length(1e6), _decay(1e5) {
            connect(this, &QQuickItem::windowChanged, this, &flow_display::handle_window_changed);
        }
        flow_display(const flow_display&) = delete;
        flow_display(flow_display&&) = delete;
        flow_display& operator=(const flow_display&) = delete;
        flow_display& operator=(flow_display&&) = delete;
        virtual ~flow_display() {}

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

        /// set_speed_to_length defines the length in pixels of the arrow representing a one-pixel-per-microsecond
        /// speed. The length to speed ratio will be passed to the openGL renderer, therefore it should only be set
        /// during qml construction.
        virtual void set_speed_to_length(float speed_to_length) {
            if (_ready.load(std::memory_order_acquire)) {
                throw std::logic_error("speed_to_length can only be set during qml construction");
            }
            _speed_to_length = speed_to_length;
        }

        /// speed_to_length returns the currently used speed_to_length.
        virtual float speed_to_length() const {
            return _speed_to_length;
        }

        /// set_decay defines the flow decay.
        /// The decay will be passed to the openGL renderer, therefore it should only be set during qml construction.
        virtual void set_decay(float decay) {
            if (_ready.load(std::memory_order_acquire)) {
                throw std::logic_error("decay can only be set during qml construction");
            }
            _decay = decay;
        }

        /// decay returns the currently used decay.
        virtual float decay() const {
            return _decay;
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
            _flow_display_renderer->push<Event>(event);
        }

        /// assign sets all the pixels at once.
        template <typename Iterator>
        void assign(Iterator begin, Iterator end) {
            while (!_renderer_ready.load(std::memory_order_acquire)) {
            }
            _flow_display_renderer->assign<Iterator>(begin, end);
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
            if (_ready.load(std::memory_order_acquire)) {
                if (!_flow_display_renderer) {
                    _flow_display_renderer = std::unique_ptr<flow_display_renderer>(
                        new flow_display_renderer(_canvas_size, _speed_to_length, _decay));
                    connect(
                        window(),
                        &QQuickWindow::beforeRendering,
                        _flow_display_renderer.get(),
                        &flow_display_renderer::paint,
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
                    _flow_display_renderer->set_rendering_area(
                        _paint_area, window()->height() * window()->devicePixelRatio());
                    paintAreaChanged(_paint_area);
                }
            }
        }

        /// cleanup frees the owned renderer.
        void cleanup() {
            _flow_display_renderer.reset();
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
                connect(window, &QQuickWindow::beforeSynchronizing, this, &flow_display::sync, Qt::DirectConnection);
                connect(
                    window, &QQuickWindow::sceneGraphInvalidated, this, &flow_display::cleanup, Qt::DirectConnection);
                window->setClearBeforeRendering(false);
            }
        }

        protected:
        std::atomic_bool _ready;
        std::atomic_bool _renderer_ready;
        QSize _canvas_size;
        float _speed_to_length;
        float _decay;
        std::unique_ptr<flow_display_renderer> _flow_display_renderer;
        QRectF _clear_area;
        QRectF _paint_area;
    };
}
