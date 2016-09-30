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

    /// ColorDisplayRenderer handles openGL calls for a display.
    class ColorDisplayRenderer : public QObject, public QOpenGLFunctions {
        Q_OBJECT
        public:
            ColorDisplayRenderer(QSize canvasSize, float discardRatio):
                _canvasSize(std::move(canvasSize)),
                _discardRatio(discardRatio),
                _timeDeltas(_canvasSize.width() * _canvasSize.height() * 3, std::numeric_limits<float>::infinity()),
                _duplicatedTimeDeltas(_canvasSize.width() * _canvasSize.height() * 3),
                _discardsChanged(false),
                _automaticCalibration(true),
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
                for (auto y = static_cast<qint32>(0); y < _canvasSize.height(); ++y) {
                    for (auto x = static_cast<qint32>(0); x < _canvasSize.width(); ++x) {
                        _coordinates.push_back(static_cast<float>(x));
                        _coordinates.push_back(static_cast<float>(y));
                    }
                }
                _accessingTimeDeltas.clear(std::memory_order_release);
                _accessingSettings.clear(std::memory_order_release);
            }
            ColorDisplayRenderer(const ColorDisplayRenderer&) = delete;
            ColorDisplayRenderer(ColorDisplayRenderer&&) = default;
            ColorDisplayRenderer& operator=(const ColorDisplayRenderer&) = delete;
            ColorDisplayRenderer& operator=(ColorDisplayRenderer&&) = default;
            virtual ~ColorDisplayRenderer() {}

            /// setRenderingArea defines the rendering area.
            virtual void setRenderingArea(QRectF clearArea, QRectF paintArea, int windowHeight) {
                _clearArea = std::move(clearArea);
                _clearArea.moveTop(windowHeight - _clearArea.top() - _clearArea.height());
                _paintArea = std::move(paintArea);
                _paintArea.moveTop(windowHeight - _paintArea.top() - _paintArea.height());
            }

            /// setDiscards defines the discards.
            /// If both the black and white discards are zero (default), the discards are computed automatically.
            virtual void setDiscards(QVector2D discards) {
                while (_accessingSettings.test_and_set(std::memory_order_acquire)) {}
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
                _accessingSettings.clear(std::memory_order_release);
            }

            /// setWhiteTimeDeltas defines the time deltas associated with the camera white point.
            virtual void setWhiteTimeDeltas(QVector3D whiteTimeDeltas) {
                while (_accessingSettings.test_and_set(std::memory_order_acquire)) {}
                _whiteTimeDeltas = whiteTimeDeltas;
                _accessingSettings.clear(std::memory_order_relaxed);
            }

            /// push adds an event to the display.
            template<typename Event>
            void push(Event event) {
                const auto index = (static_cast<std::size_t>(event.x) + static_cast<std::size_t>(event.y) * _canvasSize.width()) * 3;
                while (_accessingTimeDeltas.test_and_set(std::memory_order_acquire)) {}
                _timeDeltas[index] = static_cast<float>(event.red);
                _timeDeltas[index + 1] = static_cast<float>(event.green);
                _timeDeltas[index + 2] = static_cast<float>(event.blue);
                _accessingTimeDeltas.clear(std::memory_order_release);
            }

        signals:

            /// discardsChanged notifies a change in the discards.
            void discardsChanged(QVector2D discards);

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
                            "attribute vec3 timeDeltas;"
                            "varying vec3 exposures;"
                            "uniform float width;"
                            "uniform float height;"
                            "uniform float slope;"
                            "uniform float intercept;"
                            "uniform vec3 whiteTimeDeltas;"
                            "void main() {"
                            "    gl_Position = vec4("
                            "        coordinates.x / (width - 1.0) * 2.0 - 1.0,"
                            "        coordinates.y / (height - 1.0) * 2.0 - 1.0,"
                            "        0.0,"
                            "        1.0"
                            "    );"
                            "    exposures = slope * log(timeDeltas / whiteTimeDeltas) + intercept;"
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
                            "varying vec3 exposures;"
                            "void main() {"
                            "    gl_FragColor = vec4(exposures, 1.0);"
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
                    _timeDeltasLocation = glGetAttribLocation(_programId, "timeDeltas");
                    _slopeLocation = glGetUniformLocation(_programId, "slope");
                    _interceptLocation = glGetUniformLocation(_programId, "intercept");
                    _whiteTimeDeltasLocation = glGetUniformLocation(_programId, "whiteTimeDeltas");
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
                    while (_accessingTimeDeltas.test_and_set(std::memory_order_acquire)) {}
                    std::copy(_timeDeltas.begin(), _timeDeltas.end(), _duplicatedTimeDeltas.begin());
                    _accessingTimeDeltas.clear(std::memory_order_release);

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

                    // Retrieve the discards
                    // Calculate the discards if automatic calibration is enabled (both discards are zero)
                    while (_accessingSettings.test_and_set(std::memory_order_acquire)) {}
                    if (_automaticCalibration) {
                        auto sortedTimeDeltasByChannel = std::array<std::vector<float>, 3>();
                        for (auto&& sortedTimeDeltas : sortedTimeDeltasByChannel) {
                            sortedTimeDeltas.reserve(_duplicatedTimeDeltas.size() / 3);
                        }
                        for (
                            auto timeDeltaIterator = _duplicatedTimeDeltas.begin();
                            timeDeltaIterator != _duplicatedTimeDeltas.end();
                            std::advance(timeDeltaIterator, 3)
                        ) {
                            for (auto channel = static_cast<unsigned char>(0); channel < 3; ++channel) {
                                const auto timeDelta = *std::next(timeDeltaIterator, channel);
                                if (std::isfinite(timeDelta)) {
                                    sortedTimeDeltasByChannel[channel].push_back(timeDelta);
                                }
                            }
                        }
                        _discards.setX(0);
                        _discards.setY(std::numeric_limits<float>::infinity());
                        for (
                            auto sortedTimeDeltasIterator = sortedTimeDeltasByChannel.begin();
                            sortedTimeDeltasIterator != sortedTimeDeltasByChannel.end();
                            ++sortedTimeDeltasIterator
                        ) {
                            if (!sortedTimeDeltasIterator->empty()) {
                                std::sort(sortedTimeDeltasIterator->begin(), sortedTimeDeltasIterator->end());
                            }
                            auto blackDiscardCandidate = (*sortedTimeDeltasIterator)[static_cast<std::size_t>(
                                static_cast<float>(sortedTimeDeltasIterator->size()) * (1.0 - _discardRatio)
                            )];
                            auto whiteDiscardCandidate = (*sortedTimeDeltasIterator)[static_cast<std::size_t>(
                                static_cast<float>(sortedTimeDeltasIterator->size()) * _discardRatio + 0.5
                            )];
                            if (blackDiscardCandidate <= whiteDiscardCandidate) {
                                blackDiscardCandidate = sortedTimeDeltasIterator->back();
                                whiteDiscardCandidate = sortedTimeDeltasIterator->front();
                                if (blackDiscardCandidate > whiteDiscardCandidate) {
                                    const auto normalisedBlackDiscardCandidate = blackDiscardCandidate
                                        / _whiteTimeDeltas[sortedTimeDeltasIterator - sortedTimeDeltasByChannel.begin()];
                                    const auto normalisedWhiteDiscardCandidate = whiteDiscardCandidate
                                        / _whiteTimeDeltas[sortedTimeDeltasIterator - sortedTimeDeltasByChannel.begin()];
                                    if (normalisedBlackDiscardCandidate > _discards.x()) {
                                        _discards.setX(normalisedBlackDiscardCandidate);
                                    }
                                    if (normalisedBlackDiscardCandidate < _discards.y()) {
                                        _discards.setY(normalisedWhiteDiscardCandidate);
                                    }
                                }
                            } else {
                                const auto normalisedBlackDiscardCandidate = blackDiscardCandidate
                                    / _whiteTimeDeltas[sortedTimeDeltasIterator - sortedTimeDeltasByChannel.begin()];
                                const auto normalisedWhiteDiscardCandidate = whiteDiscardCandidate
                                    / _whiteTimeDeltas[sortedTimeDeltasIterator - sortedTimeDeltasByChannel.begin()];
                                    if (normalisedBlackDiscardCandidate > _discards.x()) {
                                        _discards.setX(normalisedBlackDiscardCandidate);
                                    }
                                    if (normalisedBlackDiscardCandidate < _discards.y()) {
                                        _discards.setY(normalisedWhiteDiscardCandidate);
                                    }
                            }
                        }
                    }
                    if (_discardsChanged) {
                        discardsChanged(_discards);
                    }
                    {
                        const auto delta = std::log(_discards.x() / _discards.y());
                        glUniform1f(_slopeLocation, static_cast<GLfloat>(-1.0 / delta));
                        glUniform1f(_interceptLocation, static_cast<GLfloat>(std::log(_discards.x()) / delta));
                        glUniform3f(_whiteTimeDeltasLocation, _whiteTimeDeltas.x(), _whiteTimeDeltas.y(), _whiteTimeDeltas.z());
                    }
                    _accessingSettings.clear(std::memory_order_release);

                    // Send varying data to the GPU
                    glEnableVertexAttribArray(_coordinatesLocation);
                    glEnableVertexAttribArray(_timeDeltasLocation);
                    glVertexAttribPointer(_coordinatesLocation, 2, GL_FLOAT, GL_FALSE, 0, _coordinates.data());
                    glVertexAttribPointer(_timeDeltasLocation, 3, GL_FLOAT, GL_FALSE, 0, _duplicatedTimeDeltas.data());
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
            float _discardRatio;
            std::vector<GLuint> _indices;
            std::vector<float> _coordinates;
            std::vector<float> _timeDeltas;
            std::vector<float> _duplicatedTimeDeltas;
            std::atomic_flag _accessingTimeDeltas;
            QRectF _clearArea;
            QRectF _paintArea;
            QVector2D _discards;
            QVector3D _whiteTimeDeltas;
            std::atomic_flag _accessingSettings;
            bool _discardsChanged;
            bool _automaticCalibration;
            GLuint _programId;
            bool _programSetup;
            GLuint _coordinatesLocation;
            GLuint _timeDeltasLocation;
            GLuint _slopeLocation;
            GLuint _interceptLocation;
            GLuint _whiteTimeDeltasLocation;
    };

    /// ColorDisplay displays a stream of events.
    class ColorDisplay : public QQuickItem {
        Q_OBJECT
        Q_PROPERTY(QSize canvasSize READ canvasSize WRITE setCanvasSize)
        Q_PROPERTY(QVector2D discards READ discards WRITE setDiscards)
        Q_PROPERTY(QVector3D whiteTimeDeltas READ whiteTimeDeltas WRITE setWhiteTimeDeltas)
        Q_PROPERTY(float discardRatio READ discardRatio WRITE setDiscardRatio)
        Q_PROPERTY(QRectF paintArea READ paintArea)
        public:
            ColorDisplay() :
                _canvasSizeSet(false),
                _discardRatioSet(false),
                _accessingRenderer(false),
                _rendererReady(false),
                _whiteTimeDeltasToLoad(1.0, 1.0, 1.0)
            {
                connect(this, &QQuickItem::windowChanged, this, &ColorDisplay::handleWindowChanged);
                _accessingRenderer.clear(std::memory_order_release);
            }
            ColorDisplay(const ColorDisplay&) = delete;
            ColorDisplay(ColorDisplay&&) = default;
            ColorDisplay& operator=(const ColorDisplay&) = delete;
            ColorDisplay& operator=(ColorDisplay&&) = default;
            virtual ~ColorDisplay() {}

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

            /// setDiscards defines the discards.
            /// If both the black and white discards are zero (default), the discards are computed automatically.
            virtual void setDiscards(QVector2D discards) {
                while (_accessingRenderer.test_and_set(std::memory_order_acquire)) {}
                if (_rendererReady.load(std::memory_order_acquire)) {
                    _colorDisplayRenderer->setDiscards(discards);
                } else {
                    _discardsToLoad = discards;
                }
                _accessingRenderer.clear(std::memory_order_relaxed);
            }

            /// discards returns the currently used discards.
            virtual QVector2D discards() const {
                return _discards;
            }

            /// setWhiteTimeDeltas defines the time deltas associated with the camera white point.
            virtual void setWhiteTimeDeltas(QVector3D whiteTimeDeltas) {
                while (_accessingRenderer.test_and_set(std::memory_order_acquire)) {}
                if (_rendererReady.load(std::memory_order_acquire)) {
                    _colorDisplayRenderer->setWhiteTimeDeltas(whiteTimeDeltas);
                    _whiteTimeDeltas = whiteTimeDeltas;
                } else {
                    _whiteTimeDeltasToLoad = whiteTimeDeltas;
                }
                _accessingRenderer.clear(std::memory_order_relaxed);
            }

            /// whiteTimeDeltas returns the currently used whiteTimeDeltas.
            virtual QVector3D whiteTimeDeltas() const {
                return _whiteTimeDeltas;
            }

            /// setDiscardRatio defines the discards ratio.
            /// The discards ratio should be set only once.
            virtual void setDiscardRatio(float discardRatio) {
                if (!_discardRatioSet.load(std::memory_order_relaxed)) {
                    _discardRatio = discardRatio;
                    _discardRatioSet.store(true, std::memory_order_release);
                }
            }

            /// discardRatio returns the currently used discardRatio.
            virtual float discardRatio() const {
                return _discardRatio;
            }

            /// paintArea returns the paint area in window coordinates.
            virtual QRectF paintArea() const {
                return _paintArea;
            }

            /// push adds an event to the display.
            template<typename Event>
            void push(Event event) {
                if (_rendererReady.load(std::memory_order_acquire)) {
                    _colorDisplayRenderer->push<Event>(event);
                }
            }

        signals:

            /// discardsChanged notifies a change in the discards.
            void discardsChanged(QVector2D discards);

            /// paintAreaChanged notifies a paint area change.
            void paintAreaChanged(QRectF paintArea);

        public slots:

            /// sync adapts the renderer to external changes.
            void sync() {
                if (
                    _canvasSizeSet.load(std::memory_order_acquire)
                    && _discardRatioSet.load(std::memory_order_acquire)
                ) {
                    if (!_colorDisplayRenderer) {
                        _colorDisplayRenderer = std::unique_ptr<ColorDisplayRenderer>(
                            new ColorDisplayRenderer(_canvasSize, _discardRatio)
                        );
                        connect(
                            window(),
                            &QQuickWindow::beforeRendering,
                            _colorDisplayRenderer.get(),
                            &ColorDisplayRenderer::paint,
                            Qt::DirectConnection
                        );
                        connect(
                            _colorDisplayRenderer.get(),
                            &ColorDisplayRenderer::discardsChanged,
                            this,
                            &ColorDisplay::updateDiscards
                        );
                        while (_accessingRenderer.test_and_set(std::memory_order_acquire)) {}
                        _colorDisplayRenderer->setDiscards(_discardsToLoad);
                        _colorDisplayRenderer->setWhiteTimeDeltas(_whiteTimeDeltasToLoad);
                        _whiteTimeDeltas = _whiteTimeDeltasToLoad;
                        _rendererReady.store(true, std::memory_order_release);
                        _accessingRenderer.clear(std::memory_order_release);
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
                        _colorDisplayRenderer->setRenderingArea(_clearArea, _paintArea, window()->height());
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
                    connect(window, &QQuickWindow::beforeSynchronizing, this, &ColorDisplay::sync, Qt::DirectConnection);
                    connect(window, &QQuickWindow::sceneGraphInvalidated, this, &ColorDisplay::cleanup, Qt::DirectConnection);
                    window->setClearBeforeRendering(false);
                }
            }

        protected:
            QSize _canvasSize;
            std::atomic_bool _canvasSizeSet;
            QVector2D _discards;
            QVector3D _whiteTimeDeltas;
            std::unique_ptr<ColorDisplayRenderer> _colorDisplayRenderer;
            float _discardRatio;
            std::atomic_bool _discardRatioSet;
            std::atomic_flag _accessingRenderer;
            std::atomic_bool _rendererReady;
            QVector2D _discardsToLoad;
            QVector3D _whiteTimeDeltasToLoad;
            QRectF _clearArea;
            QRectF _paintArea;
    };
}
