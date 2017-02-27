#pragma once

#include <QVector3D>
#include <QtQuick/qquickwindow.h>
#include <QtGui/QOpenGLFunctions>
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

    /// ColorDisplayRenderer handles openGL calls for a ColorDisplay.
    class ColorDisplayRenderer : public QObject, public QOpenGLFunctions {
        Q_OBJECT
        public:
            ColorDisplayRenderer(const QSize& canvasSize, const QColor& backgroundColor):
                _canvasSize(canvasSize),
                _colors(_canvasSize.width() * _canvasSize.height() * 3, 0),
                _duplicatedColors(_canvasSize.width() * _canvasSize.height() * 3),
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
                _accessingColors.clear(std::memory_order_release);
            }
            ColorDisplayRenderer(const ColorDisplayRenderer&) = delete;
            ColorDisplayRenderer(ColorDisplayRenderer&&) = default;
            ColorDisplayRenderer& operator=(const ColorDisplayRenderer&) = delete;
            ColorDisplayRenderer& operator=(ColorDisplayRenderer&&) = default;
            virtual ~ColorDisplayRenderer() {}

            virtual void setRenderingArea(const QRectF& clearArea, const QRectF& paintArea, const int& windowHeight) {
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
                initializeOpenGLFunctions();
                if (!_programSetup) {
                    _programSetup = true;

                    // Compile the vertex shader
                    const auto vertexShaderId = glCreateShader(GL_VERTEX_SHADER);
                    {
                        const auto vertexShader = std::string(
                            "#version 120\n"
                            "attribute vec2 coordinates;"
                            "attribute vec3 color;"
                            "varying vec3 fragmentColor;"
                            "uniform float width;"
                            "uniform float height;"
                            "void main() {"
                            "    gl_Position = vec4("
                            "        coordinates.x / (width - 1.0) * 2.0 - 1.0,"
                            "        coordinates.y / (height - 1.0) * 2.0 - 1.0,"
                            "        0.0,"
                            "        1.0"
                            "    );"
                            "    fragmentColor = color;"
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
                            "varying vec3 fragmentColor;"
                            "void main() {"
                            "    gl_FragColor = vec4(fragmentColor, 1.0);"
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
                    _colorLocation = glGetAttribLocation(_programId, "color");
                    glDeleteShader(vertexShaderId);
                    glDeleteShader(fragmentShaderId);
                    checkProgramError(_programId);

                    // Set the uniform values
                    glUniform1f(glGetUniformLocation(_programId, "width"), static_cast<GLfloat>(_canvasSize.width()));
                    glUniform1f(glGetUniformLocation(_programId, "height"), static_cast<GLfloat>(_canvasSize.height()));

                    // Additional OpenGL settings
                    glDisable(GL_DEPTH_TEST);
                    checkOpenGLError();
                } else {

                    // Copy the events to minimise the strain on the event pipeline
                    while (_accessingColors.test_and_set(std::memory_order_acquire)) {}
                    std::copy(_colors.begin(), _colors.end(), _duplicatedColors.begin());
                    _accessingColors.clear(std::memory_order_release);

                    // Resize the rendering area
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
                    glEnable(GL_SCISSOR_TEST);
                    glScissor(
                        static_cast<GLint>(_paintArea.left()),
                        static_cast<GLint>(_paintArea.top()),
                        static_cast<GLsizei>(_paintArea.width()),
                        static_cast<GLsizei>(_paintArea.height())
                    );

                    // Send varying data to the GPU
                    glEnableVertexAttribArray(_coordinatesLocation);
                    glEnableVertexAttribArray(_colorLocation);
                    glVertexAttribPointer(_coordinatesLocation, 2, GL_FLOAT, GL_FALSE, 0, _coordinates.data());
                    glVertexAttribPointer(_colorLocation, 3, GL_FLOAT, GL_FALSE, 0, _duplicatedColors.data());
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
            GLuint _programId;
            bool _programSetup;
            GLuint _coordinatesLocation;
            GLuint _colorLocation;
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
                if (_rendererReady.load(std::memory_order_relaxed)) {
                    _colorDisplayRenderer->push<Event>(event);
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
