#pragma once

#include <QQmlParserStatus>
#include <QtGui/QOpenGLFunctions_3_3_Core>
#include <QtGui/QOpenGLContext>
#include <QtQuick/QQuickItem>
#include <QtQuick/qquickwindow.h>

#include <memory>
#include <atomic>
#include <limits>
#include <array>
#include <vector>
#include <stdexcept>

/// chameleon provides Qt components for event stream display.
namespace chameleon {

    /// ChangeDetectionDisplayRenderer handles openGL calls for a display.
    class ChangeDetectionDisplayRenderer : public QObject, public QOpenGLFunctions_3_3_Core {
        Q_OBJECT
        public:
            ChangeDetectionDisplayRenderer(
                const QSize& canvasSize,
                const float& decay,
                const QColor& increaseColor,
                const QColor& idleColor,
                const QColor& decreaseColor,
                const QColor& backgroundColor
            ) :
                _canvasSize(canvasSize),
                _decay(decay),
                _increaseColor(increaseColor),
                _idleColor(idleColor),
                _decreaseColor(decreaseColor),
                _backgroundColor(backgroundColor),
                _currentTimestamp(0),
                _duplicatedTimestampsAndAreIncreases(_canvasSize.width() * _canvasSize.height() * 2),
                _programSetup(false)
            {
                _indices.reserve(static_cast<std::size_t>(
                    2 * (_canvasSize.width() * _canvasSize.height() - _canvasSize.width() + _canvasSize.height() - 2)
                ));
                for (qint32 y = 0; y < _canvasSize.height() - 1; ++y) {
                    if (y > 0) {
                        _indices.push_back(y * _canvasSize.width());
                    }

                    for (qint32 x = 0; x < _canvasSize.width(); ++x) {
                        _indices.push_back(x + y * _canvasSize.width());
                        _indices.push_back(x + (y + 1) * _canvasSize.width());
                    }

                    if (y < _canvasSize.height() - 2) {
                        _indices.push_back(_canvasSize.width() - 1 + (y + 1) * _canvasSize.width());
                    }
                }
                _coordinates.reserve(_canvasSize.width() * _canvasSize.height() * 2);
                _timestampsAndAreIncreases.reserve(_canvasSize.width() * _canvasSize.height() * 2);
                for (qint32 y = 0; y < _canvasSize.height(); ++y) {
                    for (qint32 x = 0; x < _canvasSize.width(); ++x) {
                        _coordinates.push_back(static_cast<float>(x));
                        _coordinates.push_back(static_cast<float>(y));
                        _timestampsAndAreIncreases.push_back(-std::numeric_limits<float>::infinity());
                        _timestampsAndAreIncreases.push_back(static_cast<float>(1.0));
                    }
                }
                _accessingTimestampsAndAreIncreases.clear(std::memory_order_release);
            }
            ChangeDetectionDisplayRenderer(const ChangeDetectionDisplayRenderer&) = delete;
            ChangeDetectionDisplayRenderer(ChangeDetectionDisplayRenderer&&) = default;
            ChangeDetectionDisplayRenderer& operator=(const ChangeDetectionDisplayRenderer&) = delete;
            ChangeDetectionDisplayRenderer& operator=(ChangeDetectionDisplayRenderer&&) = default;
            virtual ~ChangeDetectionDisplayRenderer() {
                glDeleteBuffers(2, _vertexBuffersIds.data());
                glDeleteVertexArrays(1, &_vertexArrayId);
            }

            /// setRenderingArea defines the rendering area.
            virtual void setRenderingArea(const QRectF& clearArea, const QRectF& paintArea, const int& windowHeight) {
                _clearArea = clearArea;
                _clearArea.moveTop(windowHeight - _clearArea.top() - _clearArea.height());
                _paintArea = paintArea;
                _paintArea.moveTop(windowHeight - _paintArea.top() - _paintArea.height());
            }

            /// push adds an event to the display.
            template<typename Event>
            void push(Event event) {
                const auto index = (static_cast<std::size_t>(event.x) + static_cast<std::size_t>(event.y) * _canvasSize.width()) * 2;
                while (_accessingTimestampsAndAreIncreases.test_and_set(std::memory_order_acquire)) {}
                _currentTimestamp = event.timestamp;
                _timestampsAndAreIncreases[index] = static_cast<float>(event.timestamp);
                _timestampsAndAreIncreases[index + 1] = event.isIncrease ? 1.0 : 0.0;
                _accessingTimestampsAndAreIncreases.clear(std::memory_order_release);
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
                            "in vec2 timestampAndIsIncrease;\n"
                            "out vec4 fragmentColor;\n"
                            "uniform float width;\n"
                            "uniform float height;\n"
                            "uniform float decay;\n"
                            "uniform float currentTimestamp;\n"
                            "uniform vec4 increaseColor;\n"
                            "uniform vec4 idleColor;\n"
                            "uniform vec4 decreaseColor;\n"
                            "void main() {\n"
                            "    gl_Position = vec4(\n"
                            "        coordinates.x / (width - 1.0) * 2.0 - 1.0,\n"
                            "        coordinates.y / (height - 1.0) * 2.0 - 1.0,\n"
                            "        0.0,\n"
                            "        1.0\n"
                            "    );\n"
                            "    if (timestampAndIsIncrease.x > currentTimestamp) {\n"
                            "        fragmentColor = idleColor;\n"
                            "    } else {\n"
                            "        float lambda = exp(-(currentTimestamp - timestampAndIsIncrease.x) / decay);\n"
                            "        fragmentColor =\n"
                            "            lambda * (timestampAndIsIncrease.y > 0.5 ? increaseColor : decreaseColor)\n"
                            "            + (1 - lambda) * idleColor\n"
                            "        ;\n"
                            "    }\n"
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

                    // compile the fragment shader
                    const auto fragmentShaderId = glCreateShader(GL_FRAGMENT_SHADER);
                    {
                        const auto fragmentShader = std::string(
                            "#version 330 core\n"
                            "in vec4 fragmentColor;\n"
                            "out vec4 color\n;"
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
                    glAttachShader(_programId, fragmentShaderId);
                    glLinkProgram(_programId);
                    glDeleteShader(vertexShaderId);
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
                        _duplicatedTimestampsAndAreIncreases.size() * sizeof(decltype(_duplicatedTimestampsAndAreIncreases)::value_type),
                        _duplicatedTimestampsAndAreIncreases.data(),
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
                    glEnableVertexAttribArray(glGetAttribLocation(_programId, "timestampAndIsIncrease"));
                    glVertexAttribPointer(glGetAttribLocation(_programId, "timestampAndIsIncrease"), 2, GL_FLOAT, GL_FALSE, 0, 0);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, std::get<2>(_vertexBuffersIds));
                    glBindVertexArray(0);

                    // set the uniform values
                    glUniform1f(glGetUniformLocation(_programId, "width"), static_cast<GLfloat>(_canvasSize.width()));
                    glUniform1f(glGetUniformLocation(_programId, "height"), static_cast<GLfloat>(_canvasSize.height()));
                    glUniform1f(glGetUniformLocation(_programId, "decay"), static_cast<GLfloat>(_decay));
                    glUniform4f(
                        glGetUniformLocation(_programId, "increaseColor"),
                        static_cast<GLfloat>(_increaseColor.redF()),
                        static_cast<GLfloat>(_increaseColor.greenF()),
                        static_cast<GLfloat>(_increaseColor.blueF()),
                        static_cast<GLfloat>(_increaseColor.alphaF())
                    );
                    glUniform4f(
                        glGetUniformLocation(_programId, "idleColor"),
                        static_cast<GLfloat>(_idleColor.redF()),
                        static_cast<GLfloat>(_idleColor.greenF()),
                        static_cast<GLfloat>(_idleColor.blueF()),
                        static_cast<GLfloat>(_idleColor.alphaF())
                    );
                    glUniform4f(
                        glGetUniformLocation(_programId, "decreaseColor"),
                        static_cast<GLfloat>(_decreaseColor.redF()),
                        static_cast<GLfloat>(_decreaseColor.greenF()),
                        static_cast<GLfloat>(_decreaseColor.blueF()),
                        static_cast<GLfloat>(_decreaseColor.alphaF())
                    );
                    _currentTimestampLocation = glGetUniformLocation(_programId, "currentTimestamp");
                } else {

                    // copy the events to minimise the strain on the event pipeline
                    while (_accessingTimestampsAndAreIncreases.test_and_set(std::memory_order_acquire)) {}
                    _duplicatedCurrentTimestamp = _currentTimestamp;
                    std::copy(_timestampsAndAreIncreases.begin(), _timestampsAndAreIncreases.end(), _duplicatedTimestampsAndAreIncreases.begin());
                    _accessingTimestampsAndAreIncreases.clear(std::memory_order_release);

                    // send data to the GPU
                    glUseProgram(_programId);
                    glBindBuffer(GL_ARRAY_BUFFER, std::get<1>(_vertexBuffersIds));
                    glBufferData(
                        GL_ARRAY_BUFFER,
                        _duplicatedTimestampsAndAreIncreases.size() * sizeof(decltype(_duplicatedTimestampsAndAreIncreases)::value_type),
                        nullptr,
                        GL_DYNAMIC_DRAW
                    );
                    glBufferSubData(
                        GL_ARRAY_BUFFER,
                        0,
                        _duplicatedTimestampsAndAreIncreases.size() * sizeof(decltype(_duplicatedTimestampsAndAreIncreases)::value_type),
                        _duplicatedTimestampsAndAreIncreases.data()
                    );

                    // resize the rendering area
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
                    glUniform1f(_currentTimestampLocation, static_cast<GLfloat>(_duplicatedCurrentTimestamp));
                    glBindVertexArray(_vertexArrayId);
                    glDrawElements(GL_TRIANGLE_STRIP, static_cast<GLsizei>(_indices.size()), GL_UNSIGNED_INT, nullptr);
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
                    auto errorMessage = std::vector<char>(messageLength);
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
            float _decay;
            QColor _increaseColor;
            QColor _idleColor;
            QColor _decreaseColor;
            QColor _backgroundColor;
            float _currentTimestamp;
            float _duplicatedCurrentTimestamp;
            std::vector<GLuint> _indices;
            std::vector<float> _coordinates;
            std::vector<float> _timestampsAndAreIncreases;
            std::vector<float> _duplicatedTimestampsAndAreIncreases;
            std::atomic_flag _accessingTimestampsAndAreIncreases;
            QRectF _clearArea;
            QRectF _paintArea;
            bool _programSetup;
            GLuint _programId;
            GLuint _vertexArrayId;
            std::array<GLuint, 3> _vertexBuffersIds;
            GLuint _currentTimestampLocation;
    };

    /// ChangeDetectionDisplay displays a stream of events.
    class ChangeDetectionDisplay : public QQuickItem {
        Q_OBJECT
        Q_INTERFACES(QQmlParserStatus)
        Q_PROPERTY(QSize canvasSize READ canvasSize WRITE setCanvasSize)
        Q_PROPERTY(float decay READ decay WRITE setDecay)
        Q_PROPERTY(QColor increaseColor READ increaseColor WRITE setIncreaseColor)
        Q_PROPERTY(QColor idleColor READ idleColor WRITE setIdleColor)
        Q_PROPERTY(QColor decreaseColor READ decreaseColor WRITE setDecreaseColor)
        Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor)
        Q_PROPERTY(QRectF paintArea READ paintArea)
        public:
            ChangeDetectionDisplay() :
                _ready(false),
                _rendererReady(false),
                _decay(1e5),
                _increaseColor(Qt::white),
                _idleColor(Qt::darkGray),
                _decreaseColor(Qt::black),
                _backgroundColor(Qt::black)
            {
                connect(this, &QQuickItem::windowChanged, this, &ChangeDetectionDisplay::handleWindowChanged);
            }
            ChangeDetectionDisplay(const ChangeDetectionDisplay&) = delete;
            ChangeDetectionDisplay(ChangeDetectionDisplay&&) = default;
            ChangeDetectionDisplay& operator=(const ChangeDetectionDisplay&) = delete;
            ChangeDetectionDisplay& operator=(ChangeDetectionDisplay&&) = default;
            virtual ~ChangeDetectionDisplay() {}

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

            /// setDecay defines the pixel decay.
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

            /// setIncreaseColor defines the color used to represent increasing light.
            /// The increase color will be passed to the openGL renderer, therefore it should only be set during qml construction.
            virtual void setIncreaseColor(QColor increaseColor) {
                if (_ready.load(std::memory_order_acquire)) {
                    throw std::logic_error("increaseColor can only be set during qml construction");
                }
                _increaseColor = increaseColor;
            }

            /// increaseColor returns the currently used increaseColor.
            virtual QColor increaseColor() const {
                return _increaseColor;
            }

            /// setIdleColor defines the color used to represent idle pixels.
            /// The idle color will be passed to the openGL renderer, therefore it should only be set during qml construction.
            virtual void setIdleColor(QColor idleColor) {
                if (_ready.load(std::memory_order_acquire)) {
                    throw std::logic_error("idleColor can only be set during qml construction");
                }
                _idleColor = idleColor;
            }

            /// idleColor returns the currently used idleColor.
            virtual QColor idleColor() const {
                return _idleColor;
            }

            /// setDecreaseColor defines the color used to represent decreasing light.
            /// The decrease color will be passed to the openGL renderer, therefore it should only be set during qml construction.
            virtual void setDecreaseColor(QColor decreaseColor) {
                if (_ready.load(std::memory_order_acquire)) {
                    throw std::logic_error("decreaseColor can only be set during qml construction");
                }
                _decreaseColor = decreaseColor;
            }

            /// decreaseColor returns the currently used decreaseColor.
            virtual QColor decreaseColor() const {
                return _decreaseColor;
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
                _changeDetectionDisplayRenderer->push<Event>(event);
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
                    if (!_changeDetectionDisplayRenderer) {
                        _changeDetectionDisplayRenderer = std::unique_ptr<ChangeDetectionDisplayRenderer>(
                            new ChangeDetectionDisplayRenderer(_canvasSize, _decay, _increaseColor, _idleColor, _decreaseColor, _backgroundColor)
                        );
                        connect(
                            window(),
                            &QQuickWindow::beforeRendering,
                            _changeDetectionDisplayRenderer.get(),
                            &ChangeDetectionDisplayRenderer::paint,
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
                        _changeDetectionDisplayRenderer->setRenderingArea(_clearArea, _paintArea, window()->height() * window()->devicePixelRatio());
                        paintAreaChanged(_paintArea);
                    }
                }
            }

            /// cleanup resets the renderer.
            void cleanup() {
                _changeDetectionDisplayRenderer.reset();
            }

            /// triggerDraw triggers a draw.
            void triggerDraw() {
                if (window()) {
                    window()->update();
                }
            }

        private slots:

            /// handleWindowChanged must be triggered after a window change.
            void handleWindowChanged(QQuickWindow* window) {
                if (window) {
                    connect(window, &QQuickWindow::beforeSynchronizing, this, &ChangeDetectionDisplay::sync, Qt::DirectConnection);
                    connect(window, &QQuickWindow::sceneGraphInvalidated, this, &ChangeDetectionDisplay::cleanup, Qt::DirectConnection);
                    window->setClearBeforeRendering(false);
                }
            }

        protected:
            std::atomic_bool _ready;
            std::atomic_bool _rendererReady;
            QSize _canvasSize;
            float _decay;
            QColor _increaseColor;
            QColor _idleColor;
            QColor _decreaseColor;
            QColor _backgroundColor;
            std::unique_ptr<ChangeDetectionDisplayRenderer> _changeDetectionDisplayRenderer;
            QRectF _clearArea;
            QRectF _paintArea;
    };
}
