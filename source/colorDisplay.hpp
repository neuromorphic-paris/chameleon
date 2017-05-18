#pragma once

#include <QQmlParserStatus>
#include <QtGui/QOpenGLFunctions_3_3_Core>
#include <QtGui/QOpenGLContext>
#include <QtQuick/QQuickItem>
#include <QtQuick/qquickwindow.h>

#include <memory>
#include <stdexcept>
#include <atomic>
#include <algorithm>
#include <limits>
#include <cmath>
#include <array>
#include <vector>

/// chameleon provides Qt components for event stream display.
namespace chameleon {

    /// ColorDisplayRenderer handles openGL calls for a ColorDisplay.
    class ColorDisplayRenderer : public QObject, public QOpenGLFunctions_3_3_Core {
        Q_OBJECT
        public:
            ColorDisplayRenderer(QSize canvasSize, QColor backgroundColor):
                _canvasSize(canvasSize),
                _backgroundColor(backgroundColor),
                _indices(_canvasSize.width() * _canvasSize.height()),
                _colors(_canvasSize.width() * _canvasSize.height() * 3, 0),
                _duplicatedColors(_canvasSize.width() * _canvasSize.height() * 3),
                _programSetup(false)
            {
                for (auto indexIterator = _indices.begin(); indexIterator != _indices.end(); ++indexIterator) {
                    *indexIterator = static_cast<qint32>(std::distance(_indices.begin(), indexIterator));
                }
                _coordinates.reserve(_canvasSize.width() * _canvasSize.height() * 2);
                for (qint32 y = 0; y < _canvasSize.height(); ++y) {
                    for (qint32 x = 0; x < _canvasSize.width(); ++x) {
                        _coordinates.push_back(static_cast<float>(x));
                        _coordinates.push_back(static_cast<float>(y));
                    }
                }
                _accessingColors.clear(std::memory_order_release);
            }
            ColorDisplayRenderer(const ColorDisplayRenderer&) = delete;
            ColorDisplayRenderer(ColorDisplayRenderer&&) = default;
            ColorDisplayRenderer& operator=(const ColorDisplayRenderer&) = delete;
            ColorDisplayRenderer& operator=(ColorDisplayRenderer&&) = default;
            virtual ~ColorDisplayRenderer() {
                glDeleteBuffers(2, _vertexBuffersIds.data());
                glDeleteVertexArrays(1, &_vertexArrayId);
            }

            /// setRenderingArea defines the rendering area.
            virtual void setRenderingArea(QRectF clearArea, QRectF paintArea, int windowHeight) {
                _clearArea = clearArea;
                _clearArea.moveTop(windowHeight - _clearArea.top() - _clearArea.height());
                _paintArea = paintArea;
                _paintArea.moveTop(windowHeight - _paintArea.top() - _paintArea.height());
            }

            /// push adds an event to the display.
            template<typename Event>
            void push(Event event) {
                const auto index = (static_cast<std::size_t>(event.x) + static_cast<std::size_t>(event.y) * _canvasSize.width()) * 3;
                while (_accessingColors.test_and_set(std::memory_order_acquire)) {}
                _colors[index] = static_cast<float>(event.r);
                _colors[index + 1] = static_cast<float>(event.g);
                _colors[index + 2] = static_cast<float>(event.b);
                _accessingColors.clear(std::memory_order_release);
            }

        public slots:

            /// paint sends commands to the GPU.
            void paint() {
                if (!initializeOpenGLFunctions()) {
                    throw std::runtime_error("initializing the OpenGL context failed");
                }
                if (!_programSetup) {
                    _programSetup = true;

                    // compile the vertex shader
                    const auto vertexShaderId = glCreateShader(GL_VERTEX_SHADER);
                    {
                        const auto vertexShader = std::string(
                            "#version 330 core\n"
                            "in vec2 coordinates;\n"
                            "in vec3 color;\n"
                            "out vec4 geometryColor;\n"
                            "uniform float width;\n"
                            "uniform float height;\n"
                            "void main() {\n"
                            "    gl_Position = vec4(\n"
                            "        coordinates.x / width * 2.0 - 1.0,\n"
                            "        coordinates.y / height * 2.0 - 1.0,\n"
                            "        0.0,\n"
                            "        1.0\n"
                            "    );\n"
                            "    geometryColor = vec4(color[0], color[1], color[2], 1.0);\n"
                            "}\n"
                        );
                        auto vertexShaderContent = vertexShader.c_str();
                        auto vertexShaderSize = vertexShader.size();
                        glShaderSource(
                            vertexShaderId,
                            1,
                            static_cast<const GLchar**>(&vertexShaderContent),
                            reinterpret_cast<const GLint*>(&vertexShaderSize)
                        );
                    }
                    glCompileShader(vertexShaderId);
                    checkShaderError(vertexShaderId);

                    // compile the geometry shader
                    const auto geometryShaderId = glCreateShader(GL_GEOMETRY_SHADER);
                    {
                        const auto geometryShader = std::string(
                            "#version 330 core\n"
                            "layout (points) in;\n"
                            "layout (triangle_strip, max_vertices = 4) out;\n"
                            "in vec4 geometryColor[];\n"
                            "out vec4 fragmentColor;\n"
                            "uniform float width;\n"
                            "uniform float height;\n"
                            "void main() {\n"
                            "    fragmentColor = geometryColor[0];\n"
                            "    float pixelWidth = 2.0 / width;\n"
                            "    float pixelHeight = 2.0 / height;\n"
                            "    gl_Position = vec4(gl_in[0].gl_Position.x, gl_in[0].gl_Position.y, 0.0, 1.0);\n"
                            "    EmitVertex();\n"
                            "    gl_Position = vec4(gl_in[0].gl_Position.x, gl_in[0].gl_Position.y + pixelHeight, 0.0, 1.0);\n"
                            "    EmitVertex();\n"
                            "    gl_Position = vec4(gl_in[0].gl_Position.x + pixelWidth, gl_in[0].gl_Position.y, 0.0, 1.0);\n"
                            "    EmitVertex();\n"
                            "    gl_Position = vec4(gl_in[0].gl_Position.x + pixelWidth, gl_in[0].gl_Position.y + pixelHeight, 0.0, 1.0);\n"
                            "    EmitVertex();\n"
                            "}\n"
                        );
                        auto geometryShaderContent = geometryShader.c_str();
                        auto geometryShaderSize = geometryShader.size();
                        glShaderSource(
                            geometryShaderId,
                            1,
                            static_cast<const GLchar**>(&geometryShaderContent),
                            reinterpret_cast<const GLint*>(&geometryShaderSize)
                        );
                    }
                    glCompileShader(geometryShaderId);
                    checkShaderError(geometryShaderId);

                    // compile the fragment shader
                    const auto fragmentShaderId = glCreateShader(GL_FRAGMENT_SHADER);
                    {
                        const auto fragmentShader = std::string(
                            "#version 330 core\n"
                            "in vec4 fragmentColor;\n"
                            "out vec4 color;\n"
                            "void main() {\n"
                            "    color = fragmentColor;\n"
                            "}\n"
                        );
                        auto fragmentShaderContent = fragmentShader.c_str();
                        auto fragmentShaderSize = fragmentShader.size();
                        glShaderSource(
                            fragmentShaderId,
                            1,
                            static_cast<const GLchar**>(&fragmentShaderContent),
                            reinterpret_cast<const GLint*>(&fragmentShaderSize)
                        );
                    }
                    glCompileShader(fragmentShaderId);
                    checkShaderError(fragmentShaderId);

                    // create the shaders pipeline
                    _programId = glCreateProgram();
                    glAttachShader(_programId, vertexShaderId);
                    glAttachShader(_programId, geometryShaderId);
                    glAttachShader(_programId, fragmentShaderId);
                    glLinkProgram(_programId);
                    glDeleteShader(vertexShaderId);
                    glDeleteShader(geometryShaderId);
                    glDeleteShader(fragmentShaderId);
                    glUseProgram(_programId);
                    checkProgramError(_programId);

                    // create the vertex buffer objects
                    glGenBuffers(3, _vertexBuffersIds.data());
                    glBindBuffer(GL_ARRAY_BUFFER, std::get<0>(_vertexBuffersIds));
                    glBufferData(
                        GL_ARRAY_BUFFER,
                        _coordinates.size() * sizeof(decltype(_coordinates)::value_type),
                        _coordinates.data(),
                        GL_STATIC_DRAW
                    );
                    glBindBuffer(GL_ARRAY_BUFFER, std::get<1>(_vertexBuffersIds));
                    glBufferData(
                        GL_ARRAY_BUFFER,
                        _duplicatedColors.size() * sizeof(decltype(_duplicatedColors)::value_type),
                        _duplicatedColors.data(),
                        GL_DYNAMIC_DRAW
                    );
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, std::get<2>(_vertexBuffersIds));
                    glBufferData(
                        GL_ELEMENT_ARRAY_BUFFER,
                        _indices.size() * sizeof(decltype(_indices)::value_type),
                        _indices.data(),
                        GL_STATIC_DRAW
                    );

                    // create the vertex array object
                    glGenVertexArrays(1, &_vertexArrayId);
                    glBindVertexArray(_vertexArrayId);
                    glBindBuffer(GL_ARRAY_BUFFER, std::get<0>(_vertexBuffersIds));
                    glEnableVertexAttribArray(glGetAttribLocation(_programId, "coordinates"));
                    glVertexAttribPointer(glGetAttribLocation(_programId, "coordinates"), 2, GL_FLOAT, GL_FALSE, 0, 0);
                    glBindBuffer(GL_ARRAY_BUFFER, std::get<1>(_vertexBuffersIds));
                    glEnableVertexAttribArray(glGetAttribLocation(_programId, "color"));
                    glVertexAttribPointer(glGetAttribLocation(_programId, "color"), 3, GL_FLOAT, GL_FALSE, 0, 0);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, std::get<2>(_vertexBuffersIds));
                    glBindVertexArray(0);

                    // set the uniform values
                    glUniform1f(glGetUniformLocation(_programId, "width"), static_cast<GLfloat>(_canvasSize.width()));
                    glUniform1f(glGetUniformLocation(_programId, "height"), static_cast<GLfloat>(_canvasSize.height()));
                } else {

                    // copy the events to minimise the strain on the event pipeline
                    while (_accessingColors.test_and_set(std::memory_order_acquire)) {}
                    std::copy(_colors.begin(), _colors.end(), _duplicatedColors.begin());
                    _accessingColors.clear(std::memory_order_release);

                    // send data to the GPU
                    glUseProgram(_programId);
                    glBindBuffer(GL_ARRAY_BUFFER, std::get<1>(_vertexBuffersIds));
                    glBufferData(
                        GL_ARRAY_BUFFER,
                        _duplicatedColors.size() * sizeof(decltype(_duplicatedColors)::value_type),
                        nullptr,
                        GL_DYNAMIC_DRAW
                    );
                    glBufferSubData(
                        GL_ARRAY_BUFFER,
                        0,
                        _duplicatedColors.size() * sizeof(decltype(_duplicatedColors)::value_type),
                        _duplicatedColors.data()
                    );

                    // resize the rendering area
                    glUseProgram(_programId);
                    glEnable(GL_SCISSOR_TEST);
                    glScissor(
                        static_cast<GLint>(_clearArea.left()),
                        static_cast<GLint>(_clearArea.top()),
                        static_cast<GLsizei>(_clearArea.width()),
                        static_cast<GLsizei>(_clearArea.height())
                    );
                    glClearColor(
                        static_cast<GLfloat>(_backgroundColor.redF()),
                        static_cast<GLfloat>(_backgroundColor.greenF()),
                        static_cast<GLfloat>(_backgroundColor.blueF()),
                        static_cast<GLfloat>(_backgroundColor.alphaF())
                    );
                    glClear(GL_COLOR_BUFFER_BIT);
                    glDisable(GL_SCISSOR_TEST);
                    glViewport(
                        static_cast<GLint>(_paintArea.left()),
                        static_cast<GLint>(_paintArea.top()),
                        static_cast<GLsizei>(_paintArea.width()),
                        static_cast<GLsizei>(_paintArea.height())
                    );

                    // send varying data to the GPU
                    glBindVertexArray(_vertexArrayId);
                    glDrawElements(GL_POINTS, static_cast<GLsizei>(_indices.size()), GL_UNSIGNED_INT, nullptr);
                    glBindVertexArray(0);
                }
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

            /// checkShaderError checks for shader compilation errors.
            virtual void checkShaderError(GLuint shaderId) {
                GLint status = 0;
                glGetShaderiv(shaderId, GL_COMPILE_STATUS, &status);

                if (status != GL_TRUE) {
                    GLint messageLength = 0;
                    glGetShaderiv(shaderId, GL_INFO_LOG_LENGTH, &messageLength);
                    std::vector<char> errorMessage(messageLength);
                    glGetShaderInfoLog(shaderId, messageLength, nullptr, errorMessage.data());
                    throw std::logic_error("Shader error: " + std::string(errorMessage.data()));
                }
            }

            /// checkProgramError checks for program errors.
            virtual void checkProgramError(GLuint programId) {
                GLint status = 0;
                glGetProgramiv(programId, GL_LINK_STATUS, &status);

                if (status != GL_TRUE) {
                    GLint messageLength = 0;
                    glGetProgramiv(programId, GL_INFO_LOG_LENGTH, &messageLength);
                    std::vector<char> errorMessage(messageLength);
                    glGetShaderInfoLog(programId, messageLength, nullptr, errorMessage.data());
                    throw std::logic_error("Program error: " + std::string(errorMessage.data()));
                }
            }

            QSize _canvasSize;
            QColor _backgroundColor;
            std::vector<GLuint> _indices;
            std::vector<float> _coordinates;
            std::vector<float> _colors;
            std::vector<float> _duplicatedColors;
            std::atomic_flag _accessingColors;
            QRectF _clearArea;
            QRectF _paintArea;
            bool _programSetup;
            GLuint _programId;
            GLuint _vertexArrayId;
            std::array<GLuint, 3> _vertexBuffersIds;
    };

    /// ColorDisplay displays a stream of color events without tone-mapping.
    class ColorDisplay : public QQuickItem {
        Q_OBJECT
        Q_INTERFACES(QQmlParserStatus)
        Q_PROPERTY(QSize canvasSize READ canvasSize WRITE setCanvasSize)
        Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor)
        Q_PROPERTY(QRectF paintArea READ paintArea)
        public:
            ColorDisplay() :
                _ready(false),
                _rendererReady(false),
                _backgroundColor(Qt::black)
            {
                connect(this, &QQuickItem::windowChanged, this, &ColorDisplay::handleWindowChanged);
            }
            ColorDisplay(const ColorDisplay&) = delete;
            ColorDisplay(ColorDisplay&&) = default;
            ColorDisplay& operator=(const ColorDisplay&) = delete;
            ColorDisplay& operator=(ColorDisplay&&) = default;
            virtual ~ColorDisplay() {}

            /// setCanvasSize defines the display coordinates.
            /// The canvas size will be passed to the openGL renderer, therefore it should only be set during qml construction.
            virtual void setCanvasSize(QSize canvasSize) {
                if (_ready.load(std::memory_order_acquire)) {
                    throw std::logic_error("canvasSize can only be set during qml construction");
                }
                _canvasSize = canvasSize;
                setImplicitWidth(canvasSize.width());
                setImplicitHeight(canvasSize.height());
            }

            /// canvasSize returns the currently used canvasSize.
            virtual QSize canvasSize() const {
                return _canvasSize;
            }

            /// setBackgroundColor defines the background color used to compensate the parent's shape.
            /// The background color will be passed to the openGL renderer, therefore it should only be set during qml construction.
            virtual void setBackgroundColor(QColor backgroundColor) {
                if (_ready.load(std::memory_order_acquire)) {
                    throw std::logic_error("backgroundColor can only be set during qml construction");
                }
                _backgroundColor = backgroundColor;
            }

            /// backgroundColor returns the currently used backgroundColor.
            virtual QColor backgroundColor() const {
                return _backgroundColor;
            }

            /// paintArea returns the paint area in window coordinates.
            virtual QRectF paintArea() const {
                return _paintArea;
            }

            /// push adds an event to the display.
            template<typename Event>
            void push(Event event) {
                while (!_rendererReady.load(std::memory_order_acquire)) {}
                _colorDisplayRenderer->push<Event>(event);
            }

            /// componentComplete is called when all the qml values are binded.
            virtual void componentComplete() {
                if (_canvasSize.width() <= 0 || _canvasSize.height() <= 0) {
                    throw std::logic_error("canvasSize cannot have a null component, make sure that it is set in qml");
                }
                _ready.store(true, std::memory_order_release);
            }

        signals:

            /// paintAreaChanged notifies a paint area change.
            void paintAreaChanged(QRectF paintArea);

        public slots:

            /// sync adapts the renderer to external changes.
            void sync() {
                if (_ready.load(std::memory_order_relaxed)) {
                    if (!_colorDisplayRenderer) {
                        _colorDisplayRenderer = std::unique_ptr<ColorDisplayRenderer>(
                            new ColorDisplayRenderer(_canvasSize, _backgroundColor)
                        );
                        connect(
                            window(),
                            &QQuickWindow::beforeRendering,
                            _colorDisplayRenderer.get(),
                            &ColorDisplayRenderer::paint,
                            Qt::DirectConnection
                        );
                        _rendererReady.store(true, std::memory_order_release);
                    }
                    auto clearArea = QRectF(0, 0, width() * window()->devicePixelRatio(), height() * window()->devicePixelRatio());
                    for (auto item = static_cast<QQuickItem*>(this); item; item = item->parentItem()) {
                        clearArea.moveLeft(clearArea.left() + item->x() * window()->devicePixelRatio());
                        clearArea.moveTop(clearArea.top() + item->y() * window()->devicePixelRatio());
                    }
                    if (clearArea != _clearArea) {
                        _clearArea = std::move(clearArea);
                        if (clearArea.width() * _canvasSize.height() > clearArea.height() * _canvasSize.width()) {
                            _paintArea.setWidth(clearArea.height() * _canvasSize.width() / _canvasSize.height());
                            _paintArea.setHeight(clearArea.height());
                            _paintArea.moveLeft(clearArea.left() + (clearArea.width() - _paintArea.width()) / 2);
                            _paintArea.moveTop(clearArea.top());
                        } else {
                            _paintArea.setWidth(clearArea.width());
                            _paintArea.setHeight(clearArea.width() * _canvasSize.height() / _canvasSize.width());
                            _paintArea.moveLeft(clearArea.left());
                            _paintArea.moveTop(clearArea.top() + (clearArea.height() - _paintArea.height()) / 2);
                        }
                        _colorDisplayRenderer->setRenderingArea(_clearArea, _paintArea, window()->height() * window()->devicePixelRatio());
                        paintAreaChanged(_paintArea);
                    }
                }
            }

            /// cleanup resets the renderer.
            void cleanup() {
                _colorDisplayRenderer.reset();
            }

            /// triggerDraw triggers a draw.
            void triggerDraw() {
                if (window()) {
                    window()->update();
                }
            }

        private slots:

            /// handleWindowChanged must be triggered after a window transformation.
            void handleWindowChanged(QQuickWindow* window) {
                if (window) {
                    connect(window, &QQuickWindow::beforeSynchronizing, this, &ColorDisplay::sync, Qt::DirectConnection);
                    connect(window, &QQuickWindow::sceneGraphInvalidated, this, &ColorDisplay::cleanup, Qt::DirectConnection);
                    window->setClearBeforeRendering(false);
                }
            }

        protected:
            std::atomic_bool _ready;
            std::atomic_bool _rendererReady;
            QSize _canvasSize;
            QColor _backgroundColor;
            std::unique_ptr<ColorDisplayRenderer> _colorDisplayRenderer;
            QRectF _clearArea;
            QRectF _paintArea;
    };
}
