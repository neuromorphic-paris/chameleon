#pragma once

#include <QQmlParserStatus>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFunctions_3_3_Core>
#include <QtQuick/QQuickItem>
#include <QtQuick/qquickwindow.h>
#include <array>
#include <atomic>
#include <cmath>
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
            float tau,
            float gamma,
            std::size_t style,
            const QVector<QColor>& on_colormap,
            const QVector<QColor>& off_colormap,
            QColor background_color) :
            _canvas_size(canvas_size),
            _tau(tau),
            _gamma(gamma),
            _style(style),
            _local_style(style),
            _on_colormap(flatten_colormap(on_colormap)),
            _off_colormap(flatten_colormap(off_colormap)),
            _background_color(background_color),
            _ts_and_activities(_canvas_size.width() * _canvas_size.height() * 2),
            _current_t(0),
            _program_setup(false),
            _offset_t(0) {
            if (style >= _style_to_program_id.size()) {
                throw std::logic_error("style out of range");
            }
            for (std::size_t style = 0; style < _style_to_fragment_shader.size(); ++style) {
                std::stringstream fragment_shader_stream;
                fragment_shader_stream << "#version 330 core\n"
                                       << "in vec2 uv;\n"
                                       << "out vec4 color;\n"
                                       << "uniform float tau;\n"
                                       << "uniform float gamma;\n"
                                       << "uniform uint current_t;\n"
                                       << "uniform usampler2DRect sampler;\n"
                                       << "uniform vec4 on_colormap_table[17];\n"
                                       << "uniform vec4 off_colormap_table[17];\n"
                                       << "uniform float on_colormap_scale;\n"
                                       << "uniform float off_colormap_scale;\n"
                                       << "void main() {\n"
                                       << "    uvec2 t_and_activity = texture(sampler, uv).xy;\n"
                                       << "    uint t = t_and_activity.x;\n"
                                       << "    float activity = uintBitsToFloat(t_and_activity.y);\n";
                switch (style) {
                    case 0:
                        fragment_shader_stream << "    float lambda = exp(-float(current_t - t) / tau);\n";
                        break;
                    case 1:
                        fragment_shader_stream
                            << "    float lambda = (current_t - t) < tau ? 1.0f - (current_t - t) / tau : 0.0f;\n";
                        break;
                    case 2:
                        fragment_shader_stream << "    float lambda = (current_t - t) < tau ? 1.0f : 0.0f;\n";
                        break;
                    case 3:
                        fragment_shader_stream
                            << "    float lambda = abs(activity) * exp(-float(current_t - t) / tau);\n";
                        break;
                    default:
                        throw std::logic_error("unknown style");
                }
                fragment_shader_stream
                    << "    lambda = (1.0f - min(1.0f, lambda / gamma)) * (activity >= 0 ? on_colormap_scale : "
                       "off_colormap_scale);\n"
                    << "    color = t == 0u ? on_colormap_table[int(on_colormap_scale)] : mix(\n"
                    << "        (activity >= 0 ? on_colormap_table : off_colormap_table)[int(lambda)],\n"
                    << "        (activity >= 0 ? on_colormap_table : off_colormap_table)[int(lambda) + 1],\n"
                    << "        lambda - float(int(lambda)));\n"
                    << "}\n";
                _style_to_fragment_shader[style] = fragment_shader_stream.str();
            }
            _accessing_ts_and_activities.clear(std::memory_order_release);
            _accessing_colormaps.clear(std::memory_order_release);
        }
        dvs_display_renderer(const dvs_display_renderer&) = delete;
        dvs_display_renderer(dvs_display_renderer&&) = delete;
        dvs_display_renderer& operator=(const dvs_display_renderer&) = delete;
        dvs_display_renderer& operator=(dvs_display_renderer&&) = delete;
        virtual ~dvs_display_renderer() {
            glDeleteBuffers(1, &_pbo_id);
            glDeleteTextures(1, &_texture_id);
            glDeleteVertexArrays(1, &_vertex_array_id);
            for (std::size_t style = 0; style < _style_to_program_id.size(); ++style) {
                glDeleteProgram(_style_to_program_id[style]);
            }
        }

        /// set_rendering_area defines the rendering area.
        virtual void set_rendering_area(QRectF paint_area, int window_height) {
            _paint_area = paint_area;
            _paint_area.moveTop(window_height - _paint_area.top() - _paint_area.height());
        }

        /// lock acquires the spin-lock mutex protecting the shared time context.
        void lock() {
            while (_accessing_ts_and_activities.test_and_set(std::memory_order_acquire)) {
            }
        }

        /// unlock releases the spin-lock mutex protecting the shared time context.
        /// This function must only be called after acquiring the lock.
        void unlock() {
            _accessing_ts_and_activities.clear(std::memory_order_release);
        }

        /// set_tau changes the exponential decay constant.
        virtual void set_tau(float tau) {
            lock();
            _tau = tau;
            unlock();
        }

        /// set_gamma changes the activity to color conversion scale.
        virtual void set_gamma(float gamma) {
            _gamma.store(gamma, std::memory_order_release);
        }

        /// push_unsafe adds an event to the display.
        /// This function must only be called after acquiring the lock.
        template <typename Event>
        void push_unsafe(Event event) {
            const auto index =
                (static_cast<std::size_t>(event.x) + static_cast<std::size_t>(event.y) * _canvas_size.width()) * 2;
            auto activity_view = reinterpret_cast<float*>(_ts_and_activities.data());
            const auto minus_delta_t = -static_cast<float>(event.t - _offset_t - _ts_and_activities[index]);
            while (event.t - _offset_t > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
                _offset_t += std::numeric_limits<uint32_t>::max() / 2;
                for (std::size_t index = 0; index < _ts_and_activities.size(); index += 2) {
                    if (_ts_and_activities[index] > std::numeric_limits<uint32_t>::max() / 2) {
                        _ts_and_activities[index] -= std::numeric_limits<uint32_t>::max() / 2;
                    } else {
                        _ts_and_activities[index] = 0;
                    }
                }
            }
            _ts_and_activities[index] = static_cast<uint32_t>(event.t - _offset_t);
            if (_local_style == 3) {
                if (event.on != (activity_view[index + 1] > 0)) {
                    activity_view[index + 1] = -activity_view[index + 1];
                }
                activity_view[index + 1] =
                    activity_view[index + 1] * std::exp(minus_delta_t / _tau) + (event.on ? 1.0f : -1.0f);
            } else {
                activity_view[index + 1] = event.on ? 1.0f : -1.0f;
            }
            _current_t = static_cast<uint32_t>(event.t - _offset_t);
        }

        /// push adds an event to the display.
        template <typename Event>
        void push(Event event) {
            lock();
            push_unsafe<Event>(event);
            unlock();
        }

        /// assign sets all the pixels at once.
        template <typename Iterator>
        void assign(Iterator begin, Iterator end) {
            std::size_t index = 0;
            auto activity_view = reinterpret_cast<float*>(_ts_and_activities.data());
            while (_accessing_ts_and_activities.test_and_set(std::memory_order_acquire)) {
            }
            for (; begin != end; ++begin) {
                _ts_and_activities[index] = static_cast<uint32_t>(begin->t);
                ++index;
                activity_view[index] = begin->on ? 1.0f : 0.0f;
                ++index;
                if (static_cast<uint32_t>(begin->t) > _current_t) {
                    _current_t = static_cast<uint32_t>(begin->t);
                }
            }
            _accessing_ts_and_activities.clear(std::memory_order_release);
        }

        /// set_style changes the decay style.
        void set_style(std::size_t style) {
            if (style >= _style_to_program_id.size()) {
                throw std::logic_error("style out of range");
            }
            _style.store(style, std::memory_order_release);
        }

        /// set_on_colormap defines the colormap used to convert time deltas to colors for ON events.
        void set_on_colormap(const QVector<QColor>& on_colormap) {
            while (_accessing_colormaps.test_and_set(std::memory_order_acquire)) {
            }
            _on_colormap = flatten_colormap(on_colormap);
            _accessing_colormaps.clear(std::memory_order_release);
        }

        /// set_on_colormap defines the colormap used to convert time deltas to colors for OFF events.
        void set_off_colormap(const QVector<QColor>& off_colormap) {
            while (_accessing_colormaps.test_and_set(std::memory_order_acquire)) {
            }
            _off_colormap = flatten_colormap(off_colormap);
            _accessing_colormaps.clear(std::memory_order_release);
        }

        public slots:

        /// paint sends commands to the GPU.
        void paint() {
            if (!initializeOpenGLFunctions()) {
                throw std::runtime_error("initializing the OpenGL context failed");
            }
            if (!_program_setup) {
                _program_setup = true;
                glGenVertexArrays(1, &_vertex_array_id);
                glGenBuffers(1, &_pbo_id);
                for (std::size_t style = 0; style < _style_to_program_id.size(); ++style) {
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
                        auto fragment_shader_content = _style_to_fragment_shader[style].c_str();
                        auto fragment_shader_size = _style_to_fragment_shader[style].size();
                        glShaderSource(
                            fragment_shader_id,
                            1,
                            static_cast<const GLchar**>(&fragment_shader_content),
                            reinterpret_cast<const GLint*>(&fragment_shader_size));
                    }
                    glCompileShader(fragment_shader_id);
                    check_shader_error(fragment_shader_id);

                    // create the shaders pipeline
                    _style_to_program_id[style] = glCreateProgram();
                    glAttachShader(_style_to_program_id[style], vertex_shader_id);
                    glAttachShader(_style_to_program_id[style], fragment_shader_id);
                    glLinkProgram(_style_to_program_id[style]);
                    glDeleteShader(vertex_shader_id);
                    glDeleteShader(fragment_shader_id);
                    glUseProgram(_style_to_program_id[style]);
                    check_program_error(_style_to_program_id[style]);

                    // create the vertex buffers
                    glBindVertexArray(_vertex_array_id);
                    std::array<GLuint, 2> vertex_buffers_ids;
                    glGenBuffers(static_cast<GLsizei>(vertex_buffers_ids.size()), vertex_buffers_ids.data());
                    {
                        glBindBuffer(GL_ARRAY_BUFFER, std::get<0>(vertex_buffers_ids));
                        std::array<float, 8> coordinates{-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f};
                        glBufferData(
                            GL_ARRAY_BUFFER,
                            coordinates.size() * sizeof(decltype(coordinates)::value_type),
                            coordinates.data(),
                            GL_STATIC_DRAW);
                        glEnableVertexAttribArray(glGetAttribLocation(_style_to_program_id[style], "coordinates"));
                        glVertexAttribPointer(
                            glGetAttribLocation(_style_to_program_id[style], "coordinates"),
                            2,
                            GL_FLOAT,
                            GL_FALSE,
                            0,
                            0);
                    }
                    {
                        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, std::get<1>(vertex_buffers_ids));
                        std::array<GLuint, 4> indices{0, 1, 2, 3};
                        glBufferData(
                            GL_ELEMENT_ARRAY_BUFFER,
                            indices.size() * sizeof(decltype(indices)::value_type),
                            indices.data(),
                            GL_STATIC_DRAW);
                    }
                    glBindVertexArray(0);

                    // set the uniform values
                    glUniform1f(
                        glGetUniformLocation(_style_to_program_id[style], "width"),
                        static_cast<GLfloat>(_canvas_size.width()));
                    glUniform1f(
                        glGetUniformLocation(_style_to_program_id[style], "height"),
                        static_cast<GLfloat>(_canvas_size.height()));
                    _style_to_current_t_location[style] =
                        glGetUniformLocation(_style_to_program_id[style], "current_t");
                    _style_to_tau_location[style] = glGetUniformLocation(_style_to_program_id[style], "tau");
                    _style_to_gamma_location[style] = glGetUniformLocation(_style_to_program_id[style], "gamma");
                    _style_to_on_colormap_table_location[style] =
                        glGetUniformLocation(_style_to_program_id[style], "on_colormap_table");
                    _style_to_off_colormap_table_location[style] =
                        glGetUniformLocation(_style_to_program_id[style], "off_colormap_table");
                    _style_to_on_colormap_scale_location[style] =
                        glGetUniformLocation(_style_to_program_id[style], "on_colormap_scale");
                    _style_to_off_colormap_scale_location[style] =
                        glGetUniformLocation(_style_to_program_id[style], "off_colormap_scale");

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
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _pbo_id);
                    glBufferData(
                        GL_PIXEL_UNPACK_BUFFER,
                        _ts_and_activities.size() * sizeof(decltype(_ts_and_activities)::value_type),
                        nullptr,
                        GL_DYNAMIC_DRAW);
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                }
            }

            // send data to the GPU
            glUseProgram(_style_to_program_id[_local_style]);
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
            glUniform1f(
                _style_to_gamma_location[_local_style], static_cast<GLfloat>(_gamma.load(std::memory_order_acquire)));
            while (_accessing_colormaps.test_and_set(std::memory_order_acquire)) {
            }
            glUniform4fv(
                _style_to_on_colormap_table_location[_local_style],
                static_cast<GLsizei>(_on_colormap.size() / 4),
                reinterpret_cast<GLfloat*>(_on_colormap.data()));
            glUniform4fv(
                _style_to_off_colormap_table_location[_local_style],
                static_cast<GLsizei>(_off_colormap.size() / 4),
                reinterpret_cast<GLfloat*>(_off_colormap.data()));
            glUniform1f(
                _style_to_on_colormap_scale_location[_local_style], static_cast<GLfloat>(_on_colormap.size() / 4 - 2));
            glUniform1f(
                _style_to_off_colormap_scale_location[_local_style],
                static_cast<GLfloat>(_off_colormap.size() / 4 - 2));
            _accessing_colormaps.clear(std::memory_order_release);
            {
                auto buffer = reinterpret_cast<uint32_t*>(glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY));
                if (!buffer) {
                    throw std::logic_error("glMapBuffer returned an null pointer");
                }
                lock();
                glUniform1f(
                    _style_to_tau_location[_local_style],
                    static_cast<GLfloat>(_tau * (_local_style == 1 ? 2.0f : 1.0f)));
                glUniform1ui(_style_to_current_t_location[_local_style], _current_t);
                std::copy(_ts_and_activities.begin(), _ts_and_activities.end(), buffer);
                _local_style = _style.load(std::memory_order_acquire);
                unlock();
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
        /// flatten extracts colormap values as a vector.
        static std::vector<float> flatten_colormap(const QVector<QColor>& colormap) {
            std::vector<float> result(4 * (colormap.size() + 1));
            for (int32_t index = 0; index < colormap.size(); ++index) {
                result[4 * index] = colormap[index].redF();
                result[4 * index + 1] = colormap[index].greenF();
                result[4 * index + 2] = colormap[index].blueF();
                result[4 * index + 3] = colormap[index].alphaF();
            }
            result[4 * colormap.size()] = colormap[colormap.size() - 1].redF();
            result[4 * colormap.size() + 1] = colormap[colormap.size() - 1].greenF();
            result[4 * colormap.size() + 2] = colormap[colormap.size() - 1].blueF();
            result[4 * colormap.size() + 3] = colormap[colormap.size() - 1].alphaF();
            return result;
        }

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
        float _tau;
        std::atomic<float> _gamma;
        std::atomic<std::size_t> _style;
        std::size_t _local_style;
        std::vector<GLfloat> _on_colormap;
        std::vector<GLfloat> _off_colormap;
        std::atomic_flag _accessing_colormaps;
        QColor _background_color;
        std::vector<uint32_t> _ts_and_activities;
        uint32_t _current_t;
        std::array<std::string, 4> _style_to_fragment_shader;
        std::atomic_flag _accessing_ts_and_activities;
        QRectF _paint_area;
        bool _program_setup;
        std::array<GLuint, 4> _style_to_program_id;
        GLuint _vertex_array_id;
        GLuint _texture_id;
        GLuint _pbo_id;
        std::array<GLuint, 4> _style_to_current_t_location;
        std::array<GLuint, 4> _style_to_tau_location;
        std::array<GLuint, 4> _style_to_gamma_location;
        std::array<GLuint, 4> _style_to_on_colormap_table_location;
        std::array<GLuint, 4> _style_to_off_colormap_table_location;
        std::array<GLuint, 4> _style_to_on_colormap_scale_location;
        std::array<GLuint, 4> _style_to_off_colormap_scale_location;
        uint64_t _offset_t;
    };

    /// dvs_display displays a stream of DVS events.
    class dvs_display : public QQuickItem {
        Q_OBJECT
        Q_INTERFACES(QQmlParserStatus)
        Q_PROPERTY(QSize canvas_size READ canvas_size WRITE set_canvas_size)
        Q_PROPERTY(float tau READ tau WRITE set_tau)
        Q_PROPERTY(float gamma READ gamma WRITE set_gamma)
        Q_PROPERTY(Style style READ style WRITE set_style)
        Q_PROPERTY(QVector<QString> on_colormap READ on_colormap WRITE set_on_colormap)
        Q_PROPERTY(QVector<QString> off_colormap READ off_colormap WRITE set_off_colormap)
        Q_PROPERTY(QColor background_color READ background_color WRITE set_background_color)
        Q_PROPERTY(QRectF paint_area READ paint_area)
        Q_ENUMS(Style)
        public:
        /// Styles lists available decay functions.
        enum Style { Exponential, Linear, Window, Cumulative };

        dvs_display() :
            _ready(false),
            _renderer_ready(false),
            _tau(1e5),
            _gamma(1.0),
            _on_colormap({Qt::white, Qt::darkGray}),
            _off_colormap({Qt::black, Qt::darkGray}),
            _background_color(Qt::black),
            _style(Style::Exponential) {
            connect(this, &QQuickItem::windowChanged, this, &dvs_display::handle_window_changed);
            _accessing_renderer.clear(std::memory_order_release);
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

        /// set_tau defines the chosen style's time parameter.
        virtual void set_tau(float tau) {
            while (_accessing_renderer.test_and_set(std::memory_order_acquire)) {
            }
            if (_renderer_ready.load(std::memory_order_acquire)) {
                _dvs_display_renderer->set_tau(tau);
            }
            _tau = tau;
            _accessing_renderer.clear(std::memory_order_release);
        }

        /// parameter returns the current time parameter.
        virtual float tau() const {
            return _tau;
        }

        /// set_gamma defines the chosen style's time to color mapping.
        virtual void set_gamma(float gamma) {
            while (_accessing_renderer.test_and_set(std::memory_order_acquire)) {
            }
            if (_renderer_ready.load(std::memory_order_acquire)) {
                _dvs_display_renderer->set_gamma(gamma);
            }
            _gamma = gamma;
            _accessing_renderer.clear(std::memory_order_release);
        }

        /// gamma returns the current color mapping parameter.
        virtual float gamma() const {
            return _gamma;
        }

        /// set_style defines the style.
        /// The style is used to generate OpenGL shaders.
        /// It can only be set during qml initialization.
        virtual void set_style(Style style) {
            while (_accessing_renderer.test_and_set(std::memory_order_acquire)) {
            }
            if (_renderer_ready.load(std::memory_order_acquire)) {
                _dvs_display_renderer->set_style(static_cast<std::size_t>(style));
            }
            _style = style;
            _accessing_renderer.clear(std::memory_order_release);
        }

        /// style returns the currently used style.
        virtual Style style() const {
            return _style;
        }

        /// set_on_colormap defines the colormap used to convert time deltas to colors for ON events.
        virtual void set_on_colormap(const QVector<QString>& on_colormap) {
            if (on_colormap.size() < 2 || on_colormap.size() > 16) {
                throw std::logic_error("on_colormap must contain between 2 and 16 elements");
            }
            while (_accessing_renderer.test_and_set(std::memory_order_acquire)) {
            }
            _on_colormap.clear();
            _on_colormap.reserve(on_colormap.size());
            for (const auto& color : on_colormap) {
                _on_colormap.push_back(color);
            }
            if (_renderer_ready.load(std::memory_order_acquire)) {
                _dvs_display_renderer->set_on_colormap(_on_colormap);
            }
            _accessing_renderer.clear(std::memory_order_release);
        }

        /// on_colormap returns the current ON colormap.
        virtual QVector<QString> on_colormap() const {
            QVector<QString> result;
            result.reserve(_on_colormap.size());
            for (const auto& color : _on_colormap) {
                result.push_back(color.name());
            }
            return result;
        }

        /// set_off_colormap defines the colormap used to convert time deltas to colors for ON events.
        virtual void set_off_colormap(const QVector<QString>& off_colormap) {
            if (off_colormap.size() < 2 || off_colormap.size() > 16) {
                throw std::logic_error("off_colormap must contain between 2 and 16 elements");
            }
            while (_accessing_renderer.test_and_set(std::memory_order_acquire)) {
            }
            _off_colormap.clear();
            _off_colormap.reserve(off_colormap.size());
            for (const auto& color : off_colormap) {
                _off_colormap.push_back(color);
            }
            if (_renderer_ready.load(std::memory_order_acquire)) {
                _dvs_display_renderer->set_off_colormap(_off_colormap);
            }
            _accessing_renderer.clear(std::memory_order_release);
        }

        /// off_colormap returns the current OFF colormap.
        virtual QVector<QString> off_colormap() const {
            QVector<QString> result;
            result.reserve(_off_colormap.size());
            for (const auto& color : _off_colormap) {
                result.push_back(color.name());
            }
            return result;
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

        /// lock acquires the spin-lock mutex protecting the shared time context.
        void lock() {
            while (!_renderer_ready.load(std::memory_order_acquire)) {
            }
            _dvs_display_renderer->lock();
        }

        /// unlock releases the spin-lock mutex protecting the shared time context.
        /// This function must only be called after acquiring the lock.
        void unlock() {
            _dvs_display_renderer->unlock();
        }

        /// push_unsafe adds an event to the display.
        /// This function must only be called after acquiring the lock.
        template <typename Event>
        void push_unsafe(Event event) {
            _dvs_display_renderer->push_unsafe<Event>(event);
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
            if (_ready.load(std::memory_order_acquire)) {
                if (!_dvs_display_renderer) {
                    while (_accessing_renderer.test_and_set(std::memory_order_acquire)) {
                    }
                    _dvs_display_renderer = std::unique_ptr<dvs_display_renderer>(new dvs_display_renderer(
                        _canvas_size,
                        _tau,
                        _gamma,
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
                    _accessing_renderer.clear(std::memory_order_release);
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
        std::atomic_flag _accessing_renderer;
        QSize _canvas_size;
        float _tau;
        float _gamma;
        Style _style;
        QVector<QColor> _on_colormap;
        QVector<QColor> _off_colormap;
        QColor _background_color;
        std::unique_ptr<dvs_display_renderer> _dvs_display_renderer;
        QRectF _clear_area;
        QRectF _paint_area;
    };
}
