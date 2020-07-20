#pragma once

#include <QQmlParserStatus>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFunctions_3_3_Core>
#include <QtQuick/QQuickItem>
#include <QtQuick/qquickwindow.h>
#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

/// chameleon provides Qt components for event stream display.
namespace chameleon {

    /// dvs_display_renderer handles openGL calls for a display.
    class dvs_display_renderer : public QObject, public QOpenGLFunctions_3_3_Core {
        Q_OBJECT
        public:
        dvs_display_renderer(
            QSize canvas_size,
            float parameter,
            std::size_t style,
            const QVector<QColor>& on_colormap,
            const QVector<QColor>& off_colormap,
            QColor background_color) :
            _canvas_size(canvas_size),
            _parameter(parameter),
            _style(style),
            _background_color(background_color),
            _ts_and_ons(_canvas_size.width() * _canvas_size.height() * 2),
            _current_t(0),
            _program_setup(false) {
            std::stringstream fragment_shader_stream;
            fragment_shader_stream << "#version 330 core\n"
                                   << "in vec2 uv;\n"
                                   << "out vec4 color;\n"
                                   << "uniform float parameter;\n"
                                   << "uniform uint current_t;\n"
                                   << "uniform usampler2DRect sampler;\n"
                                   << "const float on_color_table_scale = " << on_colormap.size() - 1 << ";\n"
                                   << "const float off_color_table_scale = " << off_colormap.size() - 1 << ";\n";
            const auto table_size = std::max(on_colormap.size(), off_colormap.size()) + 1;
            fragment_shader_stream << "const vec4 on_color_table[" << table_size << "] = vec4[](\n";
            auto add_color_to_stream = [&](QColor color) {
                fragment_shader_stream << "    vec4(" << color.redF() << ", " << color.greenF() << ", " << color.blueF()
                                       << ", " << color.alphaF() << ")";
            };
            for (const auto& color : on_colormap) {
                add_color_to_stream(color);
                fragment_shader_stream << ",\n";
            }
            add_color_to_stream(on_colormap.back());
            if (on_colormap.size() < off_colormap.size()) {
                for (std::size_t index = 0; index < off_colormap.size() - on_colormap.size(); ++index) {
                    fragment_shader_stream << ",\n";
                    add_color_to_stream(on_colormap.back());
                }
            }
            fragment_shader_stream << ");\n";
            fragment_shader_stream << "const vec4 off_color_table[" << table_size << "] = vec4[](\n";
            for (const auto& color : off_colormap) {
                add_color_to_stream(color);
                fragment_shader_stream << ",\n";
            }
            add_color_to_stream(on_colormap.back());
            if (off_colormap.size() < on_colormap.size()) {
                for (std::size_t index = 0; index < on_colormap.size() - off_colormap.size(); ++index) {
                    fragment_shader_stream << ",\n";
                    add_color_to_stream(off_colormap.back());
                }
            }
            fragment_shader_stream << ");\n";
            fragment_shader_stream << "void main() {\n"
                                   << "    uvec2 t_and_on = texture(sampler, uv).xy;\n";
            switch (style) {
                case 0:
                    fragment_shader_stream
                        << "    float lambda = 1.0f - exp(-float(current_t - t_and_on.x) / parameter);\n";
                    break;
                case 1:
                    fragment_shader_stream << "    float lambda = (current_t - t_and_on.x) < parameter ? (current_t - t_and_on.x) / parameter : 1.0f;\n";
                    break;
                case 2:
                    fragment_shader_stream << "    float lambda = (current_t - t_and_on.x) < parameter ? 0.0f : 1.0f;\n";
                    break;
                default:
                    throw std::logic_error("unknown style");
            }
            fragment_shader_stream
                << "    float scaled_lambda = lambda * (t_and_on.y == 1u ? on_color_table_scale : "
                   "off_color_table_scale);\n"
                << "    color = t_and_on.x == 0u ? on_color_table[int(on_color_table_scale)] : mix(\n"
                << "        (t_and_on.y == 1u ? on_color_table : off_color_table)[int(scaled_lambda)],\n"
                << "        (t_and_on.y == 1u ? on_color_table : off_color_table)[int(scaled_lambda) + 1],\n"
                << "        scaled_lambda - float(int(scaled_lambda)));\n"
                << "}\n";
            _fragment_shader = fragment_shader_stream.str();
            _accessing_ts_and_ons.clear(std::memory_order_release);
        }
        dvs_display_renderer(const dvs_display_renderer&) = delete;
        dvs_display_renderer(dvs_display_renderer&&) = delete;
        dvs_display_renderer& operator=(const dvs_display_renderer&) = delete;
        dvs_display_renderer& operator=(dvs_display_renderer&&) = delete;
        virtual ~dvs_display_renderer() {
            glDeleteBuffers(1, &_pbo_id);
            glDeleteTextures(1, &_texture_id);
            glDeleteBuffers(static_cast<GLsizei>(_vertex_buffers_ids.size()), _vertex_buffers_ids.data());
            glDeleteVertexArrays(1, &_vertex_array_id);
            glDeleteProgram(_program_id);
        }

        /// set_rendering_area defines the rendering area.
        virtual void set_rendering_area(QRectF paint_area, int window_height) {
            _paint_area = paint_area;
            _paint_area.moveTop(window_height - _paint_area.top() - _paint_area.height());
        }

        /// set_parameter changes the exponential decay constant.
        virtual void set_parameter(float parameter) {
            _parameter.store(parameter, std::memory_order_relaxed);
        }

        /// push adds an event to the display.
        template <typename Event>
        void push(Event event) {
            const auto index =
                (static_cast<std::size_t>(event.x) + static_cast<std::size_t>(event.y) * _canvas_size.width()) * 2;
            while (_accessing_ts_and_ons.test_and_set(std::memory_order_acquire)) {
            }
            _ts_and_ons[index] = static_cast<uint32_t>(event.t);
            _ts_and_ons[index + 1] = event.on ? 1 : 0;
            _current_t = static_cast<uint32_t>(event.t);
            _accessing_ts_and_ons.clear(std::memory_order_release);
        }

        /// assign sets all the pixels at once.
        template <typename Iterator>
        void assign(Iterator begin, Iterator end) {
            std::size_t index = 0;
            while (_accessing_ts_and_ons.test_and_set(std::memory_order_acquire)) {
            }
            for (; begin != end; ++begin) {
                _ts_and_ons[index] = static_cast<uint32_t>(begin->t);
                ++index;
                _ts_and_ons[index] = begin->on ? 1 : 0;
                ++index;
                if (static_cast<uint32_t>(begin->t) > _current_t) {
                    _current_t = static_cast<uint32_t>(begin->t);
                }
            }
            _accessing_ts_and_ons.clear(std::memory_order_release);
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
                    auto fragment_shader_content = _fragment_shader.c_str();
                    auto fragment_shader_size = _fragment_shader.size();
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
                glUniform1f(
                    glGetUniformLocation(_program_id, "parameter"),
                    static_cast<GLfloat>(_parameter.load(std::memory_order_relaxed)) * (_style == 1 ? 2.0f : 1.0f));
                _current_t_location = glGetUniformLocation(_program_id, "current_t");

                // create the texture
                glGenTextures(1, &_texture_id);
                glBindTexture(GL_TEXTURE_RECTANGLE, _texture_id);
                glTexImage2D(
                    GL_TEXTURE_RECTANGLE,
                    0,
                    GL_RG32UI,
                    _canvas_size.width(),
                    _canvas_size.height(),
                    0,
                    GL_RG_INTEGER,
                    GL_UNSIGNED_INT,
                    nullptr);
                glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindTexture(GL_TEXTURE_RECTANGLE, 0);

                // create the pbo
                glGenBuffers(1, &_pbo_id);
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _pbo_id);
                glBufferData(
                    GL_PIXEL_UNPACK_BUFFER,
                    _ts_and_ons.size() * sizeof(decltype(_ts_and_ons)::value_type),
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
                GL_TEXTURE_RECTANGLE,
                0,
                0,
                0,
                _canvas_size.width(),
                _canvas_size.height(),
                GL_RG_INTEGER,
                GL_UNSIGNED_INT,
                0);
            {
                auto buffer = reinterpret_cast<uint32_t*>(glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY));
                if (!buffer) {
                    throw std::logic_error("glMapBuffer returned an null pointer");
                }
                while (_accessing_ts_and_ons.test_and_set(std::memory_order_acquire)) {
                }
                glUniform1ui(_current_t_location, _current_t);
                std::copy(_ts_and_ons.begin(), _ts_and_ons.end(), buffer);
                _accessing_ts_and_ons.clear(std::memory_order_release);
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
                auto error_message = std::vector<char>(message_length);
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
        std::atomic<float> _parameter;
        std::size_t _style;
        QColor _background_color;
        std::vector<uint32_t> _ts_and_ons;
        uint32_t _current_t;
        std::string _fragment_shader;
        std::atomic_flag _accessing_ts_and_ons;
        QRectF _paint_area;
        bool _program_setup;
        GLuint _program_id;
        GLuint _vertex_array_id;
        GLuint _texture_id;
        GLuint _pbo_id;
        std::array<GLuint, 2> _vertex_buffers_ids;
        GLuint _current_t_location;
    };

    /// dvs_display displays a stream of DVS events.
    class dvs_display : public QQuickItem {
        Q_OBJECT
        Q_INTERFACES(QQmlParserStatus)
        Q_PROPERTY(QSize canvas_size READ canvas_size WRITE set_canvas_size)
        Q_PROPERTY(float parameter READ parameter WRITE set_parameter)
        Q_PROPERTY(Style style READ style WRITE set_style)
        Q_PROPERTY(QVector<QColor> on_colormap READ on_colormap WRITE set_on_colormap)
        Q_PROPERTY(QVector<QColor> off_colormap READ off_colormap WRITE set_off_colormap)
        Q_PROPERTY(QColor background_color READ background_color WRITE set_background_color)
        Q_PROPERTY(QRectF paint_area READ paint_area)
        Q_ENUMS(Style)
        public:
        /// Styles lists available decay functions.
        enum Style { Exponential, Linear, Window };

        dvs_display() :
            _ready(false),
            _renderer_ready(false),
            _parameter(1e5),
            _on_colormap({Qt::white, Qt::darkGray}),
            _off_colormap({Qt::black, Qt::darkGray}),
            _background_color(Qt::black),
            _style(Style::Exponential) {
            connect(this, &QQuickItem::windowChanged, this, &dvs_display::handle_window_changed);
        }
        dvs_display(const dvs_display&) = delete;
        dvs_display(dvs_display&&) = delete;
        dvs_display& operator=(const dvs_display&) = delete;
        dvs_display& operator=(dvs_display&&) = delete;
        virtual ~dvs_display() {}

        /// set_canvas_size defines the display coordinates.
        /// The canvas size is used to generate OpenGL shaders.
        /// It can only be set during qml initialization.
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

        /// set_parameter defines the chosen style's time parameter.
        virtual void set_parameter(float parameter) {
            if (_ready.load(std::memory_order_acquire)) {
                _dvs_display_renderer->set_parameter(parameter);
            }
            _parameter = parameter;
        }

        /// parameter returns the currently used parameter.
        virtual float parameter() const {
            return _parameter;
        }

        /// set_style defines the style.
        /// The style is used to generate OpenGL shaders.
        /// It can only be set during qml initialization.
        virtual void set_style(Style style) {
            if (_ready.load(std::memory_order_acquire)) {
                throw std::logic_error("style can only be set during qml initialization");
            }
            _style = style;
        }

        /// style returns the currently used style.
        virtual Style style() const {
            return _style;
        }

        /// set_on_colormap defines the colormap used to convert time deltas to colors for ON events.
        /// The ON colormap is used to generate OpenGL shaders.
        /// It can only be set during qml initialization.
        virtual void set_on_colormap(const QVector<QColor>& on_colormap) {
            if (on_colormap.size() < 2) {
                throw std::logic_error("on_colormap must contain at least two elements");
            }
            if (_ready.load(std::memory_order_acquire)) {
                throw std::logic_error("on_colormap can only be set during qml construction");
            }
            _on_colormap = on_colormap;
        }

        /// on_colormap returns the currently used ON colormap.
        virtual QVector<QColor> on_colormap() const {
            return _on_colormap;
        }

        /// set_off_colormap defines the colormap used to convert time deltas to colors for OFF events.
        /// The OFF colormap is used to generate OpenGL shaders.
        /// It can only be set during qml initialization.
        virtual void set_off_colormap(const QVector<QColor>& off_colormap) {
            if (off_colormap.size() < 2) {
                throw std::logic_error("off_colormap must contain at least two elements");
            }
            if (_ready.load(std::memory_order_acquire)) {
                throw std::logic_error("off_colormap can only be set during qml construction");
            }
            _off_colormap = off_colormap;
        }

        /// off_colormap returns the currently used OFF colormap.
        virtual QVector<QColor> off_colormap() const {
            return _off_colormap;
        }

        /// set_background_color defines the background color used to compensate the parent's shape.
        /// The background color is used to generate OpenGL shaders.
        /// It can only be set during qml initialization.
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
            _dvs_display_renderer->push<Event>(event);
        }

        /// assign sets all the pixels at once.
        template <typename Iterator>
        void assign(Iterator begin, Iterator end) {
            while (!_renderer_ready.load(std::memory_order_acquire)) {
            }
            _dvs_display_renderer->assign<Iterator>(begin, end);
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
                if (!_dvs_display_renderer) {
                    _dvs_display_renderer = std::unique_ptr<dvs_display_renderer>(new dvs_display_renderer(
                        _canvas_size,
                        _parameter,
                        static_cast<std::size_t>(_style),
                        _on_colormap,
                        _off_colormap,
                        _background_color));
                    connect(
                        window(),
                        &QQuickWindow::beforeRendering,
                        _dvs_display_renderer.get(),
                        &dvs_display_renderer::paint,
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
                    _dvs_display_renderer->set_rendering_area(
                        _paint_area, window()->height() * window()->devicePixelRatio());
                    paintAreaChanged(_paint_area);
                }
            }
        }

        /// cleanup frees the owned renderer.
        void cleanup() {
            _dvs_display_renderer.reset();
        }

        /// trigger_draw requests a window refresh.
        void trigger_draw() {
            if (window()) {
                window()->update();
            }
        }

        private slots:

        /// handle_window_changed must be triggered after a window change.
        void handle_window_changed(QQuickWindow* window) {
            if (window) {
                connect(window, &QQuickWindow::beforeSynchronizing, this, &dvs_display::sync, Qt::DirectConnection);
                connect(
                    window, &QQuickWindow::sceneGraphInvalidated, this, &dvs_display::cleanup, Qt::DirectConnection);
                window->setClearBeforeRendering(false);
            }
        }

        protected:
        std::atomic_bool _ready;
        std::atomic_bool _renderer_ready;
        QSize _canvas_size;
        float _parameter;
        Style _style;
        QVector<QColor> _on_colormap;
        QVector<QColor> _off_colormap;
        QColor _background_color;
        std::unique_ptr<dvs_display_renderer> _dvs_display_renderer;
        QRectF _clear_area;
        QRectF _paint_area;
    };
}
