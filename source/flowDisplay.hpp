 #pragma once

#include <QVector3D>
#include <QtQuick/qquickwindow.h>
#include <QtGui/QOpenGLFunctions_3_3_Core>
#include <QtQuick/QQuickItem>
#include <QtGui/QOpenGLContext>

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

    /// FlowDisplayRenderer handles openGL calls for a FlowDisplay.
    class FlowDisplayRenderer : public QObject, public QOpenGLFunctions_3_3_Core {
        Q_OBJECT
        public:
            FlowDisplayRenderer(const QSize& canvasSize, const float& lengthToSpeedRatio, const float& decay):
                _canvasSize(canvasSize),
                _lengthToSpeedRatio(lengthToSpeedRatio),
                _decay(decay),
                _duplicatedTimestampsAndFlows(_canvasSize.width() * _canvasSize.height() * 3),
                _programSetup(false)
            {
                _indices.reserve(_canvasSize.width() * _canvasSize.height());
                _coordinates.reserve(_canvasSize.width() * _canvasSize.height() * 2);
                _timestampsAndFlows.reserve(_canvasSize.width() * _canvasSize.height() * 3);
                for (qint32 y = 0; y < _canvasSize.height(); ++y) {
                    for (qint32 x = 0; x < _canvasSize.width(); ++x) {
                        _indices.push_back(x + y * _canvasSize.width());
                        _coordinates.push_back(static_cast<float>(x));
                        _coordinates.push_back(static_cast<float>(y));
                        _timestampsAndFlows.push_back(-std::numeric_limits<float>::infinity());
                        _timestampsAndFlows.push_back(static_cast<float>(0.0));
                        _timestampsAndFlows.push_back(static_cast<float>(0.0));
                    }
                }


                _accessingFlows.clear(std::memory_order_release);
            }
            FlowDisplayRenderer(const FlowDisplayRenderer&) = delete;
            FlowDisplayRenderer(FlowDisplayRenderer&&) = default;
            FlowDisplayRenderer& operator=(const FlowDisplayRenderer&) = delete;
            FlowDisplayRenderer& operator=(FlowDisplayRenderer&&) = default;
            virtual ~FlowDisplayRenderer() {
                glDeleteBuffers(2, _vertexBuffersIds.data());
                glDeleteVertexArrays(1, &_vertexArrayId);
            }

            /// setRenderingArea defines the rendering area.
            virtual void setRenderingArea(const QRectF& paintArea, const int& windowHeight) {
                _paintArea = paintArea;
                _paintArea.moveTop(windowHeight - _paintArea.top() - _paintArea.height());
            }

            /// push adds an event to the display.
            template<typename Event>
            void push(Event event) {
                const auto index = (static_cast<std::size_t>(event.x) + static_cast<std::size_t>(event.y) * _canvasSize.width()) * 3;
                while (_accessingFlows.test_and_set(std::memory_order_acquire)) {}
                _currentTimestamp = event.timestamp;
                _timestampsAndFlows[index] = static_cast<float>(event.timestamp);
                _timestampsAndFlows[index + 1] = static_cast<float>(event.vx);
                _timestampsAndFlows[index + 2] = static_cast<float>(event.vy);
                _accessingFlows.clear(std::memory_order_release);
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
                            "in vec3 timestampAndFlow;\n"
                            "out vec3 geometryTimestampAndFlow;"
                            "uniform float width;\n"
                            "uniform float height;\n"
                            "void main() {\n"
                            "    gl_Position = vec4(\n"
                            "        coordinates.x,\n"
                            "        coordinates.y,\n"
                            "        0.0,\n"
                            "        1.0\n"
                            "    );\n"
                            "    geometryTimestampAndFlow = timestampAndFlow;\n"
                            "}"
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
                            "#define flowDisplayPi 3.1415926535897932384626433832795\n"
                            "layout (points) in;\n"
                            "layout (line_strip, max_vertices = 5) out;\n"
                            "in vec3 geometryTimestampAndFlow[];\n"
                            "out vec4 fragmentColor;\n"
                            "uniform float width;\n"
                            "uniform float height;\n"
                            "uniform float lengthToSpeedRatio;\n"
                            "uniform float decay;\n"
                            "uniform float currentTimestamp;\n"
                            "const vec3 colorTable[7] = vec3[](\n"
                            "    vec3(1.0, 1.0, 0.0),\n"
                            "    vec3(0.0, 1.0, 0.0),\n"
                            "    vec3(0.0, 1.0, 1.0),\n"
                            "    vec3(0.0, 0.0, 1.0),\n"
                            "    vec3(1.0, 0.0, 1.0),\n"
                            "    vec3(1.0, 0.0, 0.0),\n"
                            "    vec3(1.0, 1.0, 0.0)\n"
                            ");\n"
                            "void main() {\n"
                            "    if (geometryTimestampAndFlow[0].x > currentTimestamp) {\n"
                            "        return;\n"
                            "    }\n"
                            "    vec2 speedVector = vec2(geometryTimestampAndFlow[0].y, geometryTimestampAndFlow[0].z) * lengthToSpeedRatio;\n"
                            "    float speed = length(speedVector);\n"
                            "    if (speed == 0) {\n"
                            "        return;\n"
                            "    }\n"
                            "    float alpha = exp(-(currentTimestamp - geometryTimestampAndFlow[0].x) / decay);\n"
                            "    float floatIndex = clamp(atan(speedVector.y, speedVector.x) / (2 * flowDisplayPi) + 0.5, 0.0, 1.0) * 6.0;\n"
                            "    int integerIndex = int(floatIndex);\n"
                            "    if (floatIndex == integerIndex) {\n"
                            "        fragmentColor = vec4(colorTable[integerIndex], alpha);\n"
                            "    } else {\n"
                            "        fragmentColor = vec4(mix(colorTable[integerIndex], colorTable[integerIndex + 1], floatIndex - integerIndex), alpha);\n"
                            "    }\n"
                            "    vec2 origin = vec2(gl_in[0].gl_Position.x, gl_in[0].gl_Position.y);\n"
                            "    vec2 tip = origin + speedVector;\n"
                            "    vec2 arrowProjection = origin + (1.0 - 3.0 / speed) * speedVector;\n"
                            "    vec2 arrowHeight = 3.0 / speed * sqrt(\n"
                            "        (pow(speedVector.x, 2) + pow(speedVector.y, 2)) / (3 * pow(speedVector.x, 2) + pow(speedVector.y, 2))\n"
                            "    ) * vec2(speedVector.y, -speedVector.x);\n"
                            "    vec2 firstBranch = arrowProjection + arrowHeight;\n"
                            "    vec2 secondBranch = arrowProjection - arrowHeight;\n"
                            "    gl_Position = vec4(origin.x / (width - 1.0) * 2.0 - 1.0, origin.y / (height - 1.0) * 2.0 - 1.0, 0.0, 1.0);\n"
                            "    EmitVertex();\n"
                            "    gl_Position = vec4(tip.x / (width - 1.0) * 2.0 - 1.0, tip.y / (height - 1.0) * 2.0 - 1.0, 0.0, 1.0);\n"
                            "    EmitVertex();\n"
                            "    EndPrimitive();\n"
                            "    gl_Position = vec4(firstBranch.x / (width - 1.0) * 2.0 - 1.0, firstBranch.y / (height - 1.0) * 2.0 - 1.0, 0.0, 1.0);\n"
                            "    EmitVertex();\n"
                            "    gl_Position = vec4(tip.x / (width - 1.0) * 2.0 - 1.0, tip.y / (height - 1.0) * 2.0 - 1.0, 0.0, 1.0);\n"
                            "    EmitVertex();\n"
                            "    gl_Position = vec4(secondBranch.x / (width - 1.0) * 2.0 - 1.0, secondBranch.y / (height - 1.0) * 2.0 - 1.0, 0.0, 1.0);\n"
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
                            "in vec4 fragmentColor;"
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
                        _duplicatedTimestampsAndFlows.size() * sizeof(decltype(_duplicatedTimestampsAndFlows)::value_type),
                        _duplicatedTimestampsAndFlows.data(),
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
                    glEnableVertexAttribArray(glGetAttribLocation(_programId, "timestampAndFlow"));
                    glVertexAttribPointer(glGetAttribLocation(_programId, "timestampAndFlow"), 3, GL_FLOAT, GL_FALSE, 0, 0);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, std::get<2>(_vertexBuffersIds));
                    glBindVertexArray(0);

                    // set the uniform values
                    glUniform1f(glGetUniformLocation(_programId, "width"), static_cast<GLfloat>(_canvasSize.width()));
                    glUniform1f(glGetUniformLocation(_programId, "height"), static_cast<GLfloat>(_canvasSize.height()));
                    glUniform1f(glGetUniformLocation(_programId, "lengthToSpeedRatio"), static_cast<GLfloat>(_lengthToSpeedRatio));
                    glUniform1f(glGetUniformLocation(_programId, "decay"), static_cast<GLfloat>(_decay));
                    _currentTimestampLocation = glGetUniformLocation(_programId, "currentTimestamp");
                } else {

                    // copy the events to minimise the strain on the event pipeline
                    while (_accessingFlows.test_and_set(std::memory_order_acquire)) {}
                    _duplicatedCurrentTimestamp = _currentTimestamp;
                    std::copy(_timestampsAndFlows.begin(), _timestampsAndFlows.end(), _duplicatedTimestampsAndFlows.begin());
                    _accessingFlows.clear(std::memory_order_release);

                    // send data to the GPU
                    glUseProgram(_programId);
                    glBindBuffer(GL_ARRAY_BUFFER, std::get<1>(_vertexBuffersIds));
                    glBufferData(
                        GL_ARRAY_BUFFER,
                        _duplicatedTimestampsAndFlows.size() * sizeof(decltype(_duplicatedTimestampsAndFlows)::value_type),
                        nullptr,
                        GL_DYNAMIC_DRAW
                    );
                    glBufferSubData(
                        GL_ARRAY_BUFFER,
                        0,
                        _duplicatedTimestampsAndFlows.size() * sizeof(decltype(_duplicatedTimestampsAndFlows)::value_type),
                        _duplicatedTimestampsAndFlows.data()
                    );

                    // resize the rendering area
                    glUseProgram(_programId);
                    glViewport(
                        static_cast<GLint>(_paintArea.left()),
                        static_cast<GLint>(_paintArea.top()),
                        static_cast<GLsizei>(_paintArea.width()),
                        static_cast<GLsizei>(_paintArea.height())
                    );
                    glDisable(GL_DEPTH_TEST);
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

                    // send varying data to the GPU
                    glUniform1f(_currentTimestampLocation, static_cast<GLfloat>(_duplicatedCurrentTimestamp));
                    glBindVertexArray(_vertexArrayId);
                    glDrawElements(GL_POINTS, _indices.size(), GL_UNSIGNED_INT, nullptr);
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
            float _lengthToSpeedRatio;
            float _decay;
            float _currentTimestamp;
            float _duplicatedCurrentTimestamp;
            std::vector<GLuint> _indices;
            std::vector<float> _coordinates;
            std::vector<float> _timestampsAndFlows;
            std::vector<float> _duplicatedTimestampsAndFlows;
            std::atomic_flag _accessingFlows;
            QRectF _paintArea;
            bool _programSetup;
            GLuint _programId;
            GLuint _vertexArrayId;
            std::array<GLuint, 3> _vertexBuffersIds;
            GLuint _currentTimestampLocation;
    };

    /// FlowDisplay displays a stream of flow events.
    class FlowDisplay : public QQuickItem {
        Q_OBJECT
        Q_INTERFACES(QQmlParserStatus)
        Q_PROPERTY(QSize canvasSize READ canvasSize WRITE setCanvasSize)
        Q_PROPERTY(float lengthToSpeedRatio READ lengthToSpeedRatio WRITE setLengthToSpeedRatio)
        Q_PROPERTY(float decay READ decay WRITE setDecay)
        public:
            FlowDisplay() :
                _ready(false),
                _rendererReady(false),
                _lengthToSpeedRatio(50000),
                _decay(100000)
            {
                connect(this, &QQuickItem::windowChanged, this, &FlowDisplay::handleWindowChanged);
            }
            FlowDisplay(const FlowDisplay&) = delete;
            FlowDisplay(FlowDisplay&&) = default;
            FlowDisplay& operator=(const FlowDisplay&) = delete;
            FlowDisplay& operator=(FlowDisplay&&) = default;
            virtual ~FlowDisplay() {}

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

            /// setLengthToSpeedRatio defines the length in pixels of the arrow representing a one-pixel-per-microsecond speed.
            /// The length to speed ratio will be passed to the openGL renderer, therefore it should only be set during qml construction.
            virtual void setLengthToSpeedRatio(float lengthToSpeedRatio) {
                if (_ready.load(std::memory_order_acquire)) {
                    throw std::logic_error("lengthToSpeedRatio can only be set during qml construction");
                }
                _lengthToSpeedRatio = lengthToSpeedRatio;
            }

            /// lengthToSpeedRatio returns the currently used lengthToSpeedRatio.
            virtual float lengthToSpeedRatio() const {
                return _lengthToSpeedRatio;
            }

            /// setDecay defines the flow decay.
            /// The decay will be passed to the openGL renderer, therefore it should only be set during qml construction.
            virtual void setDecay(float decay) {
                if (_ready.load(std::memory_order_acquire)) {
                    throw std::logic_error("decay can only be set during qml construction");
                }
                _decay = decay;
            }

            /// decay returns the currently used decay.
            virtual float decay() const {
                return _decay;
            }

            /// paintArea returns the paint area in window coordinates.
            virtual QRectF paintArea() const {
                return _paintArea;
            }

            /// push adds an event to the display.
            template<typename Event>
            void push(Event event) {
                if (_rendererReady.load(std::memory_order_relaxed)) {
                    _flowDisplayRenderer->push<Event>(event);
                }
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
                    if (!_flowDisplayRenderer) {
                        _flowDisplayRenderer = std::unique_ptr<FlowDisplayRenderer>(
                            new FlowDisplayRenderer(_canvasSize, _lengthToSpeedRatio, _decay)
                        );
                        connect(
                            window(),
                            &QQuickWindow::beforeRendering,
                            _flowDisplayRenderer.get(),
                            &FlowDisplayRenderer::paint,
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
                        _flowDisplayRenderer->setRenderingArea(_paintArea, window()->height() * window()->devicePixelRatio());
                        paintAreaChanged(_paintArea);
                    }
                }
            }

            /// cleanup resets the renderer.
            void cleanup() {
                _flowDisplayRenderer.reset();
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
                    connect(window, &QQuickWindow::beforeSynchronizing, this, &FlowDisplay::sync, Qt::DirectConnection);
                    connect(window, &QQuickWindow::sceneGraphInvalidated, this, &FlowDisplay::cleanup, Qt::DirectConnection);
                    window->setClearBeforeRendering(false);
                }
            }

        protected:
            std::atomic_bool _ready;
            std::atomic_bool _rendererReady;
            QSize _canvasSize;
            float _lengthToSpeedRatio;
            float _decay;
            std::unique_ptr<FlowDisplayRenderer> _flowDisplayRenderer;
            QRectF _clearArea;
            QRectF _paintArea;
    };
}
