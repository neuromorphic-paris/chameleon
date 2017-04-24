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

    /// LogarithmicDisplayRenderer handles openGL calls for a display.
    class LogarithmicDisplayRenderer : public QObject, public QOpenGLFunctions_3_3_Core {
        Q_OBJECT
        public:
            LogarithmicDisplayRenderer(
                const QSize& canvasSize,
                const float& discardRatio,
                const std::size_t& colormap,
                const QColor& backgroundColor
            ):
                _canvasSize(std::move(canvasSize)),
                _discardRatio(discardRatio),
                _colormap(colormap),
                _backgroundColor(backgroundColor),
                _timeDeltas(_canvasSize.width() * _canvasSize.height(), std::numeric_limits<float>::infinity()),
                _duplicatedTimeDeltas(_canvasSize.width() * _canvasSize.height()),
                _discardsChanged(false),
                _automaticCalibration(true),
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
                for (qint32 y = 0; y < _canvasSize.height(); ++y) {
                    for (qint32 x = 0; x < _canvasSize.width(); ++x) {
                        _coordinates.push_back(static_cast<float>(x));
                        _coordinates.push_back(static_cast<float>(y));
                    }
                }
                _accessingTimeDeltas.clear(std::memory_order_release);
                _accessingDiscards.clear(std::memory_order_release);
            }
            LogarithmicDisplayRenderer(const LogarithmicDisplayRenderer&) = delete;
            LogarithmicDisplayRenderer(LogarithmicDisplayRenderer&&) = default;
            LogarithmicDisplayRenderer& operator=(const LogarithmicDisplayRenderer&) = delete;
            LogarithmicDisplayRenderer& operator=(LogarithmicDisplayRenderer&&) = default;
            virtual ~LogarithmicDisplayRenderer() {
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

            /// setDiscards defines the discards.
            /// If both the black and white discards are zero (default), the discards are computed automatically.
            virtual void setDiscards(const QVector2D& discards) {
                while (_accessingDiscards.test_and_set(std::memory_order_acquire)) {}
                if (_automaticCalibration) {
                    if (discards.x() != 0 || discards.y() != 0) {
                        _automaticCalibration = false;
                        _discardsChanged = true;
                        _discards = discards;
                    }
                } else {
                    if (discards != _discards) {
                        _automaticCalibration = (discards.x() == 0 && discards.y() == 0);
                        _discardsChanged = true;
                        _discards = discards;
                    }
                }
                _accessingDiscards.clear(std::memory_order_release);
            }

            /// push adds an event to the display.
            template<typename Event>
            void push(Event event) {
                const auto index = static_cast<std::size_t>(event.x) + static_cast<std::size_t>(event.y) * _canvasSize.width();
                while (_accessingTimeDeltas.test_and_set(std::memory_order_acquire)) {}
                _timeDeltas[index] = static_cast<float>(event.timeDelta);
                _accessingTimeDeltas.clear(std::memory_order_release);
            }

        signals:

            /// discardsChanged notifies a change in the discards.
            void discardsChanged(QVector2D discards);

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
                        auto vertexShader = std::string(
                            "#version 330 core\n"
                            "in vec2 coordinates;\n"
                            "in float timeDelta;\n"
                            "out vec4 fragmentColor;\n"
                            "uniform float width;\n"
                            "uniform float height;\n"
                            "uniform float slope;\n"
                            "uniform float intercept;\n"
                        );
                        switch (_colormap) {
                            case 0:
                                vertexShader.append(
                                    "void main() {\n"
                                    "    gl_Position = vec4(\n"
                                    "        coordinates.x / (width - 1.0) * 2.0 - 1.0,\n"
                                    "        coordinates.y / (height - 1.0) * 2.0 - 1.0,\n"
                                    "        0.0,\n"
                                    "        1.0\n"
                                    "    );\n"
                                    "    float exposure = clamp(slope * log(timeDelta) + intercept, 0.0, 1.0);\n"
                                    "    fragmentColor = vec4(exposure, exposure, exposure, 1.0);\n"
                                    "}\n"
                                );
                                break;
                            case 1:
                                vertexShader.append(
                                    "const vec4 colorTable[6] = vec4[](\n"
                                    "    vec4(0.0, 0.0, 0.0, 1.0),\n"
                                    "    vec4(0.5, 0.0, 0.0, 1.0),\n"
                                    "    vec4(1.0, 0.0, 0.0, 1.0),\n"
                                    "    vec4(1.0, 0.5, 0.0, 1.0),\n"
                                    "    vec4(1.0, 1.0, 0.0, 1.0),\n"
                                    "    vec4(1.0, 1.0, 1.0, 1.0)\n"
                                    ");\n"
                                    "void main() {\n"
                                    "    gl_Position = vec4(\n"
                                    "        coordinates.x / (width - 1.0) * 2.0 - 1.0,\n"
                                    "        coordinates.y / (height - 1.0) * 2.0 - 1.0,\n"
                                    "        0.0,\n"
                                    "        1.0\n"
                                    "    );\n"
                                    "    float floatIndex = clamp(slope * log(timeDelta) + intercept, 0.0, 1.0) * 5;\n"
                                    "    int integerIndex = int(floatIndex);\n"
                                    "    if (floatIndex == integerIndex) {\n"
                                    "        fragmentColor = colorTable[integerIndex];\n"
                                    "    } else {\n"
                                    "        fragmentColor = mix(colorTable[integerIndex], colorTable[integerIndex + 1], floatIndex - integerIndex);\n"
                                    "    }\n"
                                    "}\n"
                                );
                                break;
                            case 2:
                                vertexShader.append(
                                    "const vec4 colorTable[4] = vec4[](\n"
                                    "    vec4(0.0, 0.0, 1.0, 1.0),\n"
                                    "    vec4(0.0, 1.0, 1.0, 1.0),\n"
                                    "    vec4(1.0, 1.0, 0.0, 1.0),\n"
                                    "    vec4(1.0, 0.0, 0.0, 1.0)\n"
                                    ");\n"
                                    "void main() {\n"
                                    "    gl_Position = vec4(\n"
                                    "        coordinates.x / (width - 1.0) * 2.0 - 1.0,\n"
                                    "        coordinates.y / (height - 1.0) * 2.0 - 1.0,\n"
                                    "        0.0,\n"
                                    "        1.0\n"
                                    "    );\n"
                                    "    float floatIndex = clamp(slope * log(timeDelta) + intercept, 0.0, 1.0) * 3;\n"
                                    "    int integerIndex = int(floatIndex);\n"
                                    "    if (floatIndex == integerIndex) {\n"
                                    "        fragmentColor = colorTable[integerIndex];\n"
                                    "    } else {\n"
                                    "        fragmentColor = mix(colorTable[integerIndex], colorTable[integerIndex + 1], floatIndex - integerIndex);\n"
                                    "    }\n"
                                    "}\n"
                                );
                                break;
                            default:
                                throw std::logic_error("Unknown colormap id");
                        }
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
                        _duplicatedTimeDeltas.size() * sizeof(decltype(_duplicatedTimeDeltas)::value_type),
                        _duplicatedTimeDeltas.data(),
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
                    glEnableVertexAttribArray(glGetAttribLocation(_programId, "timeDelta"));
                    glVertexAttribPointer(glGetAttribLocation(_programId, "timeDelta"), 1, GL_FLOAT, GL_FALSE, 0, 0);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, std::get<2>(_vertexBuffersIds));
                    glBindVertexArray(0);

                    // set the uniform values
                    glUniform1f(glGetUniformLocation(_programId, "width"), static_cast<GLfloat>(_canvasSize.width()));
                    glUniform1f(glGetUniformLocation(_programId, "height"), static_cast<GLfloat>(_canvasSize.height()));
                    _slopeLocation = glGetUniformLocation(_programId, "slope");
                    _interceptLocation = glGetUniformLocation(_programId, "intercept");
                } else {

                    // copy the events to minimise the strain on the event pipeline
                    while (_accessingTimeDeltas.test_and_set(std::memory_order_acquire)) {}
                    std::copy(_timeDeltas.begin(), _timeDeltas.end(), _duplicatedTimeDeltas.begin());
                    _accessingTimeDeltas.clear(std::memory_order_release);

                    // send data to the GPU
                    glUseProgram(_programId);
                    glBindBuffer(GL_ARRAY_BUFFER, std::get<1>(_vertexBuffersIds));
                    glBufferData(
                        GL_ARRAY_BUFFER,
                        _duplicatedTimeDeltas.size() * sizeof(decltype(_duplicatedTimeDeltas)::value_type),
                        nullptr,
                        GL_DYNAMIC_DRAW
                    );
                    glBufferSubData(
                        GL_ARRAY_BUFFER,
                        0,
                        _duplicatedTimeDeltas.size() * sizeof(decltype(_duplicatedTimeDeltas)::value_type),
                        _duplicatedTimeDeltas.data()
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

                    // retrieve the discards
                    // calculate the discards if automatic calibration is enabled (both discards are zero)
                    while (_accessingDiscards.test_and_set(std::memory_order_acquire)) {}
                    if (_automaticCalibration) {
                        auto previousDiscards = _discards;
                        auto sortedTimeDeltas = std::vector<float>();
                        sortedTimeDeltas.reserve(_duplicatedTimeDeltas.size());
                        for (auto&& timeDelta : _duplicatedTimeDeltas) {
                            if (std::isfinite(timeDelta)) {
                                sortedTimeDeltas.push_back(timeDelta);
                            }
                        }
                        if (!sortedTimeDeltas.empty()) {
                            std::sort(sortedTimeDeltas.begin(), sortedTimeDeltas.end());
                            auto blackDiscardCandidate = sortedTimeDeltas[static_cast<std::size_t>(
                                static_cast<float>(sortedTimeDeltas.size()) * (1.0 - _discardRatio)
                            )];
                            auto whiteDiscardCandidate = sortedTimeDeltas[static_cast<std::size_t>(
                                static_cast<float>(sortedTimeDeltas.size()) * _discardRatio + 0.5
                            )];

                            if (blackDiscardCandidate <= whiteDiscardCandidate) {
                                blackDiscardCandidate = sortedTimeDeltas.back();
                                whiteDiscardCandidate = sortedTimeDeltas.front();
                                if (blackDiscardCandidate > whiteDiscardCandidate) {
                                    _discards.setX(blackDiscardCandidate);
                                    _discards.setY(whiteDiscardCandidate);
                                }
                            } else {
                                _discards.setX(blackDiscardCandidate);
                                _discards.setY(whiteDiscardCandidate);
                            }
                        }
                        if (previousDiscards != _discards) {
                            _discardsChanged = true;
                        }
                    }
                    if (_discardsChanged) {
                        discardsChanged(_discards);
                    }
                    {
                        const auto delta = std::log(_discards.x() / _discards.y());
                        glUniform1f(_slopeLocation, static_cast<GLfloat>(-1.0 / delta));
                        glUniform1f(_interceptLocation, static_cast<GLfloat>(std::log(_discards.x()) / delta));
                    }
                    _accessingDiscards.clear(std::memory_order_release);

                    // send varying data to the GPU
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
            float _discardRatio;
            std::size_t _colormap;
            QColor _backgroundColor;
            std::vector<GLuint> _indices;
            std::vector<float> _coordinates;
            std::vector<float> _timeDeltas;
            std::vector<float> _duplicatedTimeDeltas;
            std::atomic_flag _accessingTimeDeltas;
            QRectF _clearArea;
            QRectF _paintArea;
            QVector2D _discards;
            std::atomic_flag _accessingDiscards;
            bool _discardsChanged;
            bool _automaticCalibration;
            bool _programSetup;
            GLuint _programId;
            GLuint _vertexArrayId;
            std::array<GLuint, 3> _vertexBuffersIds;
            GLuint _slopeLocation;
            GLuint _interceptLocation;
    };

    /// LogarithmicDisplay displays a stream of events.
    class LogarithmicDisplay : public QQuickItem {
        Q_OBJECT
        Q_INTERFACES(QQmlParserStatus)
        Q_PROPERTY(QSize canvasSize READ canvasSize WRITE setCanvasSize)
        Q_PROPERTY(QVector2D discards READ discards WRITE setDiscards NOTIFY discardsChanged)
        Q_PROPERTY(float discardRatio READ discardRatio WRITE setDiscardRatio)
        Q_PROPERTY(Colormap colormap READ colormap WRITE setColormap)
        Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor)
        Q_PROPERTY(QRectF paintArea READ paintArea)
        Q_ENUMS(Colormap)
        public:

            /// Colormap defines the colormap used by the display.
            enum Colormap {Grey, Heat, Jet};

            LogarithmicDisplay() :
                _ready(false),
                _rendererReady(false),
                _discards(QVector2D(0, 0)),
                _discardRatio(0.01),
                _colormap(Colormap::Grey),
                _backgroundColor(Qt::black)

            {
                connect(this, &QQuickItem::windowChanged, this, &LogarithmicDisplay::handleWindowChanged);
                _accessingRenderer.clear(std::memory_order_release);
            }
            LogarithmicDisplay(const LogarithmicDisplay&) = delete;
            LogarithmicDisplay(LogarithmicDisplay&&) = default;
            LogarithmicDisplay& operator=(const LogarithmicDisplay&) = delete;
            LogarithmicDisplay& operator=(LogarithmicDisplay&&) = default;
            virtual ~LogarithmicDisplay() {}

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

            /// setDiscards defines the discards.
            /// If both the black and white discards are zero (default), the discards are computed automatically.
            virtual void setDiscards(QVector2D discards) {
                while (_accessingRenderer.test_and_set(std::memory_order_acquire)) {}
                if (_rendererReady.load(std::memory_order_relaxed)) {
                    _logarithmicDisplayRenderer->setDiscards(discards);
                } else {
                    _discardsToLoad = discards;
                }
                _accessingRenderer.clear(std::memory_order_release);
            }

            /// discards returns the currently used discards.
            virtual QVector2D discards() const {
                return _discards;
            }

            /// setDiscardRatio defines the discards ratio.
            /// The discards ratio will be passed to the openGL renderer, therefore it should only be set during qml construction.
            virtual void setDiscardRatio(float discardRatio) {
                if (_ready.load(std::memory_order_acquire)) {
                    throw std::logic_error("discardRatio can only be set during qml construction");
                }
                _discardRatio = discardRatio;
            }

            /// discardRatio returns the currently used discardRatio.
            virtual float discardRatio() const {
                return _discardRatio;
            }

            /// setColormap defines the colormap.
            /// The colormap will be passed to the openGL renderer, therefore it should only be set during qml construction.
            virtual void setColormap(Colormap colormap) {
                if (_ready.load(std::memory_order_acquire)) {
                    throw std::logic_error("colormap can only be set during qml construction");
                }
                _colormap = colormap;
            }

            /// colormap returns the currently used colormap.
            virtual Colormap colormap() const {
                return _colormap;
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
                _logarithmicDisplayRenderer->push<Event>(event);
            }

            /// componentComplete is called when all the qml values are binded.
            virtual void componentComplete() {
                if (_canvasSize.width() <= 0 || _canvasSize.height() <= 0) {
                    throw std::logic_error("canvasSize cannot have a null component, make sure that it is set in qml");
                }
                _ready.store(true, std::memory_order_release);
            }

        signals:

            /// discardsChanged notifies a change in the discards.
            void discardsChanged(QVector2D discards);

            /// paintAreaChanged notifies a paint area change.
            void paintAreaChanged(QRectF paintArea);

        public slots:

            /// sync adapts the renderer to external changes.
            void sync() {
                if (_ready.load(std::memory_order_relaxed)) {
                    if (!_logarithmicDisplayRenderer) {
                        _logarithmicDisplayRenderer = std::unique_ptr<LogarithmicDisplayRenderer>(
                            new LogarithmicDisplayRenderer(_canvasSize, _discardRatio, static_cast<std::size_t>(_colormap), _backgroundColor)
                        );
                        connect(
                            window(),
                            &QQuickWindow::beforeRendering,
                            _logarithmicDisplayRenderer.get(),
                            &LogarithmicDisplayRenderer::paint,
                            Qt::DirectConnection
                        );
                        connect(
                            _logarithmicDisplayRenderer.get(),
                            &LogarithmicDisplayRenderer::discardsChanged,
                            this,
                            &LogarithmicDisplay::updateDiscards
                        );
                        while (_accessingRenderer.test_and_set(std::memory_order_acquire)) {}
                        _logarithmicDisplayRenderer->setDiscards(_discardsToLoad);
                        _rendererReady.store(true, std::memory_order_release);
                        _accessingRenderer.clear(std::memory_order_release);
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
                        _logarithmicDisplayRenderer->setRenderingArea(_clearArea, _paintArea, window()->height() * window()->devicePixelRatio());
                        paintAreaChanged(_paintArea);
                    }
                }
            }

            /// cleanup resets the renderer.
            void cleanup() {
                _logarithmicDisplayRenderer.reset();
            }

            /// triggerDraw triggers a draw.
            void triggerDraw() {
                if (window()) {
                    window()->update();
                }
            }

            /// updateDiscards updates the discards from a signal.
            void updateDiscards(QVector2D discards) {
                while (_accessingRenderer.test_and_set(std::memory_order_acquire)) {}
                _discards = discards;
                discardsChanged(_discards);
                _accessingRenderer.clear(std::memory_order_release);
            }

        private slots:

            /// handleWindowChanged must be triggered after a window transformation.
            void handleWindowChanged(QQuickWindow* window) {
                if (window) {
                    connect(window, &QQuickWindow::beforeSynchronizing, this, &LogarithmicDisplay::sync, Qt::DirectConnection);
                    connect(window, &QQuickWindow::sceneGraphInvalidated, this, &LogarithmicDisplay::cleanup, Qt::DirectConnection);
                    window->setClearBeforeRendering(false);
                }
            }

        protected:
            std::atomic_bool _ready;
            std::atomic_bool _rendererReady;
            std::atomic_flag _accessingRenderer;
            QSize _canvasSize;
            QVector2D _discards;
            QVector2D _discardsToLoad;
            float _discardRatio;
            Colormap _colormap;
            QColor _backgroundColor;
            std::unique_ptr<LogarithmicDisplayRenderer> _logarithmicDisplayRenderer;
            QRectF _clearArea;
            QRectF _paintArea;
    };
}
