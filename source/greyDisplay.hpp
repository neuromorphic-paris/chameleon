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

    /// GreyDisplayRenderer handles openGL calls for a GreyDisplay.
    class GreyDisplayRenderer : public QObject, public QOpenGLFunctions_3_3_Core {
        Q_OBJECT
        public:
            GreyDisplayRenderer(const QSize& canvasSize, const QColor& backgroundColor):
                _canvasSize(canvasSize),
                _backgroundColor(backgroundColor),
                _exposures(_canvasSize.width() * _canvasSize.height(), 0),
                _duplicatedExposures(_canvasSize.width() * _canvasSize.height()),
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
                _accessingExposures.clear(std::memory_order_release);
            }
            GreyDisplayRenderer(const GreyDisplayRenderer&) = delete;
            GreyDisplayRenderer(GreyDisplayRenderer&&) = default;
            GreyDisplayRenderer& operator=(const GreyDisplayRenderer&) = delete;
            GreyDisplayRenderer& operator=(GreyDisplayRenderer&&) = default;
            virtual ~GreyDisplayRenderer() {
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
                const auto index = static_cast<std::size_t>(event.x) + static_cast<std::size_t>(event.y) * _canvasSize.width();
                while (_accessingExposures.test_and_set(std::memory_order_acquire)) {}
                _exposures[index] = static_cast<float>(event.exposure);
                _accessingExposures.clear(std::memory_order_release);
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
                            "in float exposure;\n"
                            "out float fragmentExposure;\n"
                            "uniform float width;\n"
                            "uniform float height;\n"
                            "void main() {\n"
                            "    gl_Position = vec4(\n"
                            "        coordinates.x / (width - 1.0) * 2.0 - 1.0,\n"
                            "        coordinates.y / (height - 1.0) * 2.0 - 1.0,\n"
                            "        0.0,\n"
                            "        1.0\n"
                            "    );\n"
                            "    fragmentExposure = exposure;\n"
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
                            "in float fragmentExposure;\n"
                            "out vec4 color;\n"
                            "void main() {\n"
                            "    color = vec4(fragmentExposure, fragmentExposure, fragmentExposure, 1.0);\n"
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
                        _duplicatedExposures.size() * sizeof(decltype(_duplicatedExposures)::value_type),
                        _duplicatedExposures.data(),
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
                    glEnableVertexAttribArray(glGetAttribLocation(_programId, "exposure"));
                    glVertexAttribPointer(glGetAttribLocation(_programId, "exposure"), 1, GL_FLOAT, GL_FALSE, 0, 0);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, std::get<2>(_vertexBuffersIds));
                    glBindVertexArray(0);

                    // set the uniform values
                    glUniform1f(glGetUniformLocation(_programId, "width"), static_cast<GLfloat>(_canvasSize.width()));
                    glUniform1f(glGetUniformLocation(_programId, "height"), static_cast<GLfloat>(_canvasSize.height()));
                } else {

                    // copy the events to minimise the strain on the event pipeline
                    while (_accessingExposures.test_and_set(std::memory_order_acquire)) {}
                    std::copy(_exposures.begin(), _exposures.end(), _duplicatedExposures.begin());
                    _accessingExposures.clear(std::memory_order_release);

                    // send data to the GPU
                    glUseProgram(_programId);
                    glBindBuffer(GL_ARRAY_BUFFER, std::get<1>(_vertexBuffersIds));
                    glBufferData(
                        GL_ARRAY_BUFFER,
                        _duplicatedExposures.size() * sizeof(decltype(_duplicatedExposures)::value_type),
                        nullptr,
                        GL_DYNAMIC_DRAW
                    );
                    glBufferSubData(
                        GL_ARRAY_BUFFER,
                        0,
                        _duplicatedExposures.size() * sizeof(decltype(_duplicatedExposures)::value_type),
                        _duplicatedExposures.data()
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
                    glDrawElements(GL_TRIANGLE_STRIP, _indices.size(), GL_UNSIGNED_INT, nullptr);
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
            std::vector<float> _exposures;
            std::vector<float> _duplicatedExposures;
            std::atomic_flag _accessingExposures;
            QRectF _clearArea;
            QRectF _paintArea;
            bool _programSetup;
            GLuint _programId;
            GLuint _vertexArrayId;
            std::array<GLuint, 3> _vertexBuffersIds;
    };

    /// GreyDisplay displays a stream of events without tone-mapping.
    class GreyDisplay : public QQuickItem {
        Q_OBJECT
        Q_INTERFACES(QQmlParserStatus)
        Q_PROPERTY(QSize canvasSize READ canvasSize WRITE setCanvasSize)
        Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor)
        Q_PROPERTY(QRectF paintArea READ paintArea)
        public:
            GreyDisplay() :
                _ready(false),
                _rendererReady(false),
                _backgroundColor(Qt::black)
            {
                connect(this, &QQuickItem::windowChanged, this, &GreyDisplay::handleWindowChanged);
            }
            GreyDisplay(const GreyDisplay&) = delete;
            GreyDisplay(GreyDisplay&&) = default;
            GreyDisplay& operator=(const GreyDisplay&) = delete;
            GreyDisplay& operator=(GreyDisplay&&) = default;
            virtual ~GreyDisplay() {}

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
                    _greyDisplayRenderer->push<Event>(event);
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
                    if (!_greyDisplayRenderer) {
                        _greyDisplayRenderer = std::unique_ptr<GreyDisplayRenderer>(
                            new GreyDisplayRenderer(_canvasSize, _backgroundColor)
                        );
                        connect(
                            window(),
                            &QQuickWindow::beforeRendering,
                            _greyDisplayRenderer.get(),
                            &GreyDisplayRenderer::paint,
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
                        _greyDisplayRenderer->setRenderingArea(_clearArea, _paintArea, window()->height() * window()->devicePixelRatio());
                        paintAreaChanged(_paintArea);
                    }
                }
            }

            /// cleanup resets the renderer.
            void cleanup() {
                _greyDisplayRenderer.reset();
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
                    connect(window, &QQuickWindow::beforeSynchronizing, this, &GreyDisplay::sync, Qt::DirectConnection);
                    connect(window, &QQuickWindow::sceneGraphInvalidated, this, &GreyDisplay::cleanup, Qt::DirectConnection);
                    window->setClearBeforeRendering(false);
                }
            }

        protected:
            std::atomic_bool _ready;
            std::atomic_bool _rendererReady;
            QSize _canvasSize;
            QColor _backgroundColor;
            std::unique_ptr<GreyDisplayRenderer> _greyDisplayRenderer;
            QRectF _clearArea;
            QRectF _paintArea;
    };
}
