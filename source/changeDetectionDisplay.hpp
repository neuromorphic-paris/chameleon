#pragma once

#include <QtQuick/qquickwindow.h>
#include <QtGui/QOpenGLFunctions>
#include <QtQuick/QQuickItem>
#include <QtGui/QOpenGLContext>

#include <memory>
#include <stdexcept>
#include <atomic>
#include <limits>
#include <vector>

/// chameleon provides Qt components for event stream display.
namespace chameleon {

    /// ChangeDetectionDisplayRenderer handles openGL calls for a display.
    class ChangeDetectionDisplayRenderer : public QObject, public QOpenGLFunctions {
        Q_OBJECT
        public:
            ChangeDetectionDisplayRenderer(QSize canvasSize, float decay, float initialTimestamp) :
                _canvasSize(std::move(canvasSize)),
                _decay(decay),
                _currentTimestamp(initialTimestamp),
                _duplicatedTimestampsAndAreIncreases(_canvasSize.width() * _canvasSize.height() * 2),
                _programSetup(false)
            {
                _indices.reserve(static_cast<std::size_t>(
                    2 * (_canvasSize.width() * _canvasSize.height() - _canvasSize.width() + _canvasSize.height() - 2)
                ));
                for (auto y = static_cast<qint32>(0); y < _canvasSize.height() - 1; ++y) {
                    if (y > 0) {
                        _indices.push_back(y * _canvasSize.width());
                    }

                    for (auto x = static_cast<qint32>(0); x < _canvasSize.width(); ++x) {
                        _indices.push_back(x + y * _canvasSize.width());
                        _indices.push_back(x + (y + 1) * _canvasSize.width());
                    }

                    if (y < _canvasSize.height() - 2) {
                        _indices.push_back(_canvasSize.width() - 1 + (y + 1) * _canvasSize.width());
                    }
                }
                _coordinates.reserve(_canvasSize.width() * _canvasSize.height() * 2);
                _timestampsAndAreIncreases.reserve(_duplicatedTimestampsAndAreIncreases.size());
                for (auto y = static_cast<qint32>(0); y < _canvasSize.height(); ++y) {
                    for (auto x = static_cast<qint32>(0); x < _canvasSize.width(); ++x) {
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
            virtual ~ChangeDetectionDisplayRenderer() {}

            /// setRenderingArea defines the rendering area.
            virtual void setRenderingArea(QRectF clearArea, QRectF paintArea, int windowHeight) {
                _clearArea = std::move(clearArea);
                _clearArea.moveTop(windowHeight - _clearArea.top() - _clearArea.height());
                _paintArea = std::move(paintArea);
                _paintArea.moveTop(windowHeight - _paintArea.top() - _paintArea.height());
            }

            /// push adds an event to the display.
            template<typename Event>
            void push(Event event) {
                const auto index = (static_cast<std::size_t>(event.x) + static_cast<std::size_t>(event.y) * _canvasSize.width()) * 2;
                while (_accessingTimestampsAndAreIncreases.test_and_set(std::memory_order_acquire)) {}
                _timestampsAndAreIncreases[index] = static_cast<float>(event.timestamp);
                _timestampsAndAreIncreases[index + 1] = event.isIncrease ? 1.0 : 0.0;
                _currentTimestamp = event.timestamp;
                _accessingTimestampsAndAreIncreases.clear(std::memory_order_release);
            }

        public slots:

            /// paint sends commands to the GPU.
            void paint() {
                initializeOpenGLFunctions();
                if (!_programSetup) {
                    _programSetup = true;

                    // Compile the vertex shader
                    const auto vertexShaderId = glCreateShader(GL_VERTEX_SHADER);
                    {
                        const auto vertexShader = std::string(
                            "#version 120\n"
                            "attribute vec2 coordinates;"
                            "attribute vec2 timestampAndIsIncrease;"
                            "varying float exposure;"
                            "uniform float width;"
                            "uniform float height;"
                            "uniform float decay;"
                            "uniform float currentTimestamp;"
                            "void main() {"
                            "    gl_Position = vec4("
                            "        coordinates.x / (width - 1.0) * 2.0 - 1.0,"
                            "        coordinates.y / (height - 1.0) * 2.0 - 1.0,"
                            "        0.0,"
                            "        1.0"
                            "    );"
                            "    if (timestampAndIsIncrease.x > currentTimestamp) {"
                            "        exposure = 0.5;"
                            "    } else {"
                            "        if (timestampAndIsIncrease.y > 0.5) {"
                            "            exposure = 0.5 * exp(-(currentTimestamp - timestampAndIsIncrease.x) / decay) + 0.5;"
                            "        } else {"
                            "            exposure = 0.5 - 0.5 * exp(-(currentTimestamp - timestampAndIsIncrease.x) / float(decay));"
                            "        }"
                            "    }"
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

                    // Compile the fragment shader
                    const auto fragmentShaderId = glCreateShader(GL_FRAGMENT_SHADER);
                    {
                        auto fragmentShader = std::string(
                            "#version 120\n"
                            "varying float exposure;"
                            "void main() {"
                            "    gl_FragColor = vec4(exposure, exposure, exposure, 1.0);"
                            "}"
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

                    // Create the shaders pipeline
                    _programId = glCreateProgram();
                    glAttachShader(_programId, vertexShaderId);
                    glAttachShader(_programId, fragmentShaderId);
                    glLinkProgram(_programId);
                    glUseProgram(_programId);
                    _coordinatesLocation = glGetAttribLocation(_programId, "coordinates");
                    _timestampAndIsIncreaseLocation = glGetAttribLocation(_programId, "timestampAndIsIncrease");
                    glDeleteShader(vertexShaderId);
                    glDeleteShader(fragmentShaderId);
                    checkProgramError(_programId);

                    // Set the uniform values
                    glUniform1f(glGetUniformLocation(_programId, "width"), static_cast<GLfloat>(_canvasSize.width()));
                    glUniform1f(glGetUniformLocation(_programId, "height"), static_cast<GLfloat>(_canvasSize.height()));
                    glUniform1f(glGetUniformLocation(_programId, "decay"), static_cast<GLfloat>(_decay));
                    _currentTimestampLocation = glGetUniformLocation(_programId, "currentTimestamp");

                    // Additional OpenGL settings
                    glDisable(GL_DEPTH_TEST);
                    checkOpenGLError();
                } else {

                    // Copy the events to minimise the strain on the event pipeline
                    while (_accessingTimestampsAndAreIncreases.test_and_set(std::memory_order_acquire)) {}
                    std::copy(_timestampsAndAreIncreases.begin(), _timestampsAndAreIncreases.end(), _duplicatedTimestampsAndAreIncreases.begin());
                    _duplicatedCurrentTimestamp = _currentTimestamp;
                    _accessingTimestampsAndAreIncreases.clear(std::memory_order_release);

                    // Resize the rendering area
                    glUseProgram(_programId);
                    glEnable(GL_SCISSOR_TEST);
                    glScissor(
                        static_cast<GLint>(_clearArea.left()),
                        static_cast<GLint>(_clearArea.top()),
                        static_cast<GLsizei>(_clearArea.width()),
                        static_cast<GLsizei>(_clearArea.height())
                    );
                    glClearColor(0.0, 0.0, 0.0, 1.0);
                    glClear(GL_COLOR_BUFFER_BIT);
                    glDisable(GL_SCISSOR_TEST);
                    glViewport(
                        static_cast<GLint>(_paintArea.left()),
                        static_cast<GLint>(_paintArea.top()),
                        static_cast<GLsizei>(_paintArea.width()),
                        static_cast<GLsizei>(_paintArea.height())
                    );
                    glEnable(GL_SCISSOR_TEST);
                    glScissor(
                        static_cast<GLint>(_paintArea.left()),
                        static_cast<GLint>(_paintArea.top()),
                        static_cast<GLsizei>(_paintArea.width()),
                        static_cast<GLsizei>(_paintArea.height())
                    );

                    // Send varying data to the GPU
                    glEnableVertexAttribArray(_coordinatesLocation);
                    glEnableVertexAttribArray(_timestampAndIsIncreaseLocation);
                    glVertexAttribPointer(_coordinatesLocation, 2, GL_FLOAT, GL_FALSE, 0, _coordinates.data());
                    glVertexAttribPointer(_timestampAndIsIncreaseLocation, 2, GL_FLOAT, GL_FALSE, 0, _duplicatedTimestampsAndAreIncreases.data());
                    glUniform1f(_currentTimestampLocation, static_cast<GLfloat>(_duplicatedCurrentTimestamp));
                    glDrawElements(GL_TRIANGLE_STRIP, _indices.size(),  GL_UNSIGNED_INT, _indices.data());
                    glDisable(GL_SCISSOR_TEST);
                }
            }

        protected:

            /// checkOpenGLError throws if openGL generated an error.
            virtual void checkOpenGLError() {
                switch (glGetError()) {
                    case GL_NO_ERROR:
                        break;
                    case GL_INVALID_ENUM:
                        throw std::runtime_error("OpenGL error: GL_INVALID_ENUM");
                    case GL_INVALID_VALUE:
                        throw std::runtime_error("OpenGL error: GL_INVALID_VALUE");
                    case GL_INVALID_OPERATION:
                        throw std::runtime_error("OpenGL error: GL_INVALID_OPERATION");
                    case GL_OUT_OF_MEMORY:
                        throw std::runtime_error("OpenGL error: GL_OUT_OF_MEMORY");
                }
            }

            /// checkShaderError checks for shader compilation errors.
            virtual void checkShaderError(GLuint shaderId) {
                auto status = static_cast<GLint>(0);
                glGetShaderiv(shaderId, GL_COMPILE_STATUS, &status);

                if (status != GL_TRUE) {
                    auto messageLength = static_cast<GLint>(0);
                    glGetShaderiv(shaderId, GL_INFO_LOG_LENGTH, &messageLength);
                    std::vector<char> errorMessage(messageLength);
                    glGetShaderInfoLog(shaderId, messageLength, nullptr, errorMessage.data());
                    throw std::runtime_error("Shader error: " + std::string(errorMessage.data()));
                }
            }

            /// checkShaderError checks for program errors.
            virtual void checkProgramError(GLuint programId) {
                auto status = static_cast<GLint>(0);
                glGetProgramiv(programId, GL_LINK_STATUS, &status);

                if (status != GL_TRUE) {
                    auto messageLength = static_cast<GLint>(0);
                    glGetProgramiv(programId, GL_INFO_LOG_LENGTH, &messageLength);
                    std::vector<char> errorMessage(messageLength);
                    glGetShaderInfoLog(programId, messageLength, nullptr, errorMessage.data());
                    throw std::runtime_error("Program error: " + std::string(errorMessage.data()));
                }
            }

            QSize _canvasSize;
            float _decay;
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
            GLuint _coordinatesLocation;
            GLuint _timestampAndIsIncreaseLocation;
            GLuint _currentTimestampLocation;
    };

    /// ChangeDetectionDisplay displays a stream of events.
    class ChangeDetectionDisplay : public QQuickItem {
        Q_OBJECT
        Q_PROPERTY(QSize canvasSize READ canvasSize WRITE setCanvasSize)
        Q_PROPERTY(float decay READ decay WRITE setDecay)
        Q_PROPERTY(float initialTimestamp READ initialTimestamp WRITE setInitialTimestamp)
        Q_PROPERTY(QRectF paintArea READ paintArea)
        public:
            ChangeDetectionDisplay() :
                _canvasSizeSet(false),
                _decaySet(false),
                _initialTimestampSet(false),
                _rendererReady(false)
            {
                connect(this, &QQuickItem::windowChanged, this, &ChangeDetectionDisplay::handleWindowChanged);
            }
            ChangeDetectionDisplay(const ChangeDetectionDisplay&) = delete;
            ChangeDetectionDisplay(ChangeDetectionDisplay&&) = default;
            ChangeDetectionDisplay& operator=(const ChangeDetectionDisplay&) = delete;
            ChangeDetectionDisplay& operator=(ChangeDetectionDisplay&&) = default;
            virtual ~ChangeDetectionDisplay() {}

            /// setCanvasSize defines the display coordinates.
            /// The size will be passed to the openGL renderer, therefore it should only be set once.
            virtual void setCanvasSize(QSize canvasSize) {
                if (!_canvasSizeSet.load(std::memory_order_relaxed)) {
                    _canvasSize = canvasSize;
                    _canvasSizeSet.store(true, std::memory_order_release);
                    setImplicitWidth(canvasSize.width());
                    setImplicitHeight(canvasSize.height());
                }
            }

            /// canvasSize returns the currently used canvasSize.
            virtual QSize canvasSize() const {
                return _canvasSize;
            }

            /// setDecay defines the pixel decay.
            /// The decay will be passed to the openGL renderer, therefore it should only be set once.
            virtual void setDecay(float decay) {
                if (!_decaySet.load(std::memory_order_relaxed)) {
                    _decay = decay;
                    _decaySet.store(true, std::memory_order_release);
                }
            }

            /// decay returns the currently used decay.
            virtual float decay() const {
                return _decay;
            }

            /// setInitialTimestamp defines the initial timestamp.
            /// The initial timestamp will be passed to the openGL renderer, therefore it should only be set once.
            virtual void setInitialTimestamp(float initialTimestamp) {
                if (!_initialTimestampSet.load(std::memory_order_relaxed)) {
                    _initialTimestamp = initialTimestamp;
                    _initialTimestampSet.store(true, std::memory_order_release);
                }
            }

            /// initialTimestamp returns the initial timestamp.
            virtual float initialTimestamp() const {
                return _initialTimestamp;
            }

            /// paintArea returns the paint area in window coordinates.
            virtual QRectF paintArea() const {
                return _paintArea;
            }

            /// push adds an event to the display.
            template<typename Event>
            void push(Event event) {
                if (_rendererReady.load(std::memory_order_relaxed)) {
                    _changeDetectionDisplayRenderer->push<Event>(event);
                }
            }

        signals:

            /// paintAreaChanged notifies a paint area change.
            void paintAreaChanged(QRectF paintArea);

        public slots:

            /// sync addapts the renderer to external changes.
            void sync() {
                if (
                    _canvasSizeSet.load(std::memory_order_acquire)
                    && _decaySet.load(std::memory_order_acquire)
                    && _initialTimestampSet.load(std::memory_order_acquire)
                ) {
                    if (!_changeDetectionDisplayRenderer) {
                        _changeDetectionDisplayRenderer = std::unique_ptr<ChangeDetectionDisplayRenderer>(
                            new ChangeDetectionDisplayRenderer(_canvasSize, _decay, _initialTimestamp)
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
                    auto clearArea = QRectF(0, 0, width(), height());
                    for (auto item = static_cast<QQuickItem*>(this); item; item = item->parentItem()) {
                        clearArea.moveLeft(clearArea.left() + item->x());
                        clearArea.moveTop(clearArea.top() + item->y());
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
                            _paintArea.setHeight(clearArea.width()  * _canvasSize.height() / _canvasSize.width());
                            _paintArea.moveLeft(clearArea.left());
                            _paintArea.moveTop(clearArea.top() + (clearArea.height() - _paintArea.height()) / 2);
                        }
                        _changeDetectionDisplayRenderer->setRenderingArea(_clearArea, _paintArea, window()->height());
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

            /// handleWindowChanged is triggered after a window change.
            void handleWindowChanged(QQuickWindow* window) {
                if (window) {
                    connect(window, &QQuickWindow::beforeSynchronizing, this, &ChangeDetectionDisplay::sync, Qt::DirectConnection);
                    connect(window, &QQuickWindow::sceneGraphInvalidated, this, &ChangeDetectionDisplay::cleanup, Qt::DirectConnection);
                    window->setClearBeforeRendering(false);
                }
            }

        protected:
            QSize _canvasSize;
            std::atomic_bool _canvasSizeSet;
            float _decay;
            std::atomic_bool _decaySet;
            float _initialTimestamp;
            std::atomic_bool _initialTimestampSet;
            std::unique_ptr<ChangeDetectionDisplayRenderer> _changeDetectionDisplayRenderer;
            std::atomic_bool _rendererReady;
            QRectF _clearArea;
            QRectF _paintArea;
    };
}
