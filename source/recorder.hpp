#pragma once

#include <QtQuick/qquickwindow.h>
#include <QtGui/QOpenGLFunctions>
#include <QtQuick/QQuickItem>
#include <QtGui/QOpenGLContext>
#include <QImage>

#include <memory>
#include <atomic>
#include <condition_variable>

/// chameleon provides Qt components for event stream display.
namespace chameleon {

    /// RecorderRenderer handles openGL calls for a recorder.
    class RecorderRenderer : public QObject, public QOpenGLFunctions {
        Q_OBJECT
        public:
            RecorderRenderer() :
                _programSetup(false),
                _renderingRequired(false),
                _beforeRenderingDone(false)
            {
            }
            RecorderRenderer(const RecorderRenderer&) = delete;
            RecorderRenderer(RecorderRenderer&&) = default;
            RecorderRenderer& operator=(const RecorderRenderer&) = delete;
            RecorderRenderer& operator=(RecorderRenderer&&) = default;
            virtual ~RecorderRenderer() {}

            /// setRenderingArea defines the rendering area.
            virtual void setRenderingArea(QRect renderingArea, int windowHeight) {
                _recordArea = std::move(renderingArea);
                _recordArea.moveTop(windowHeight - _recordArea.top() - _recordArea.height());
            }

            /// writeTo waits for a complete render, takes a screenshots and writes it to a file.
            virtual void writeTo(QString filename) {
                _renderingRequired.store(true, std::memory_order_release);
                auto lock = std::unique_lock<std::mutex>(_pixelsMutex);
                _pixelsUpdated.wait(lock);
                QImage(
                    _pixels.data(),
                    _imageWidth,
                    _imageHeight,
                    4 * _imageWidth,
                    QImage::Format_RGBA8888_Premultiplied
                ).mirrored().save(filename);
                lock.unlock();
            }

        public slots:

            /// beforeRenderingCallback is called when a rendering starts.
            void beforeRenderingCallback() {
                if (_renderingRequired.load(std::memory_order_acquire)) {
                    _renderingRequired.store(false, std::memory_order_release);
                    _beforeRenderingDone = true;
                }
            }

            /// afterRenderingCallback is called when a rendering ends.
            void afterRenderingCallback() {
                initializeOpenGLFunctions();
                if (!_programSetup) {
                    _programSetup = true;
                    _programId = glCreateProgram();
                    glLinkProgram(_programId);
                    glUseProgram(_programId);
                } else {
                    if (_beforeRenderingDone) {
                        _beforeRenderingDone = false;

                        glUseProgram(_programId);
                        glEnable(GL_SCISSOR_TEST);

                        {
                            const std::lock_guard<std::mutex> lock(_pixelsMutex);
                            _pixels.reserve(_recordArea.width() * _recordArea.height() * 4);
                            glReadPixels(
                                static_cast<GLint>(_recordArea.left()),
                                static_cast<GLint>(_recordArea.top()),
                                static_cast<GLsizei>(_recordArea.width()),
                                static_cast<GLsizei>(_recordArea.height()),
                                GL_RGBA,
                                GL_UNSIGNED_BYTE,
                                _pixels.data()
                            );
                            _imageWidth = _recordArea.width();
                            _imageHeight = _recordArea.height();
                        }
                        _pixelsUpdated.notify_one();
                        glDisable(GL_SCISSOR_TEST);
                    }
                }
            }

        protected:
            QRect _recordArea;
            bool _programSetup;
            GLuint _programId;
            std::atomic_bool _renderingRequired;
            bool _beforeRenderingDone;
            std::vector<unsigned char> _pixels;
            std::mutex _pixelsMutex;
            std::condition_variable _pixelsUpdated;
            std::size_t _imageWidth;
            std::size_t _imageHeight;
    };

    /// Recorder takes screenshots of the window.
    class Recorder : public QQuickItem {
        Q_OBJECT
        Q_PROPERTY(unsigned int interval READ interval WRITE setInterval)
        Q_PROPERTY(QString directory READ directory WRITE setDirectory)
        Q_PROPERTY(int initialTimestamp READ initialTimestamp WRITE setInitialTimestamp)
        public:
            Recorder() :
                _intervalSet(false),
                _directorySet(false),
                _shotIndex(0)
            {
                connect(this, &QQuickItem::windowChanged, this, &Recorder::handleWindowChanged);
                _accessingSettings.clear(std::memory_order_release);
            }
            Recorder(const Recorder&) = delete;
            Recorder(Recorder&&) = default;
            Recorder& operator=(const Recorder&) = delete;
            Recorder& operator=(Recorder&&) = default;
            virtual ~Recorder() {}

            /// setInterval defines the screenshots interval.
            virtual void setInterval(unsigned int interval) {
                while (_accessingSettings.test_and_set(std::memory_order_acquire)) {}
                _interval = interval;
                _intervalSet = true;
                _accessingSettings.clear(std::memory_order_release);
            }

            /// interval returns the interval between two frames in microseconds.
            virtual unsigned int interval() const {
                return _interval;
            }

            /// setDirectory defines the directory for saving the screenshots.
            virtual void setDirectory(QString directory) {
                while (_accessingSettings.test_and_set(std::memory_order_acquire)) {}
                _directory = directory;
                _directorySet = true;
                _accessingSettings.clear(std::memory_order_release);
            }

            /// directory returns the current directory for saving the screenshots.
            virtual QString directory() const {
                return _directory;
            }

            /// setInitialTimestamp defines the initial timestamp.
            virtual void setInitialTimestamp(int initialTimestamp) {
                while (_accessingSettings.test_and_set(std::memory_order_acquire)) {}
                _initialTimestamp = initialTimestamp;
                _previousShotTimestamp = initialTimestamp;
                _initialTimestampSet = true;
                _accessingSettings.clear(std::memory_order_release);
            }

            /// initialTimestamp returns the initial timestamp.
            virtual int initialTimestamp() const {
                return _initialTimestamp;
            }

            /// push updates the recorder timestamp.
            /// push must be called before pushing the associated event to the other displays.
            virtual void push(int64_t timestamp) {
                while (_accessingSettings.test_and_set(std::memory_order_acquire)) {}
                if (_intervalSet && _directorySet && _initialTimestampSet && timestamp >= _previousShotTimestamp + static_cast<int64_t>(_interval)) {
                    _recorderRenderer->writeTo(_directory + "/" + QString::number(_shotIndex) + QString(".png"));
                    ++_shotIndex;
                    _previousShotTimestamp = timestamp;
                }
                _accessingSettings.clear(std::memory_order_release);
            }

        public slots:

            /// sync addapts the renderer to external changes.
            void sync() {
                if (!_recorderRenderer) {
                    _recorderRenderer = std::unique_ptr<RecorderRenderer>(new RecorderRenderer());
                    connect(
                        window(),
                        &QQuickWindow::beforeRendering,
                        _recorderRenderer.get(),
                        &RecorderRenderer::beforeRenderingCallback,
                        Qt::DirectConnection
                    );
                    connect(
                        window(),
                        &QQuickWindow::afterRendering,
                        _recorderRenderer.get(),
                        &RecorderRenderer::afterRenderingCallback,
                        Qt::DirectConnection
                    );
                }
                _recorderRenderer->setRenderingArea(QRect(x(), y(), width(), height()), window()->height());
            }

            /// cleanup resets the renderer.
            void cleanup() {
                _recorderRenderer.reset();
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
                    connect(window, &QQuickWindow::beforeSynchronizing, this, &Recorder::sync, Qt::DirectConnection);
                    connect(window, &QQuickWindow::sceneGraphInvalidated, this, &Recorder::cleanup, Qt::DirectConnection);
                    window->setClearBeforeRendering(false);
                }
            }

        protected:
            uint64_t _interval;
            bool _intervalSet;
            QString _directory;
            bool _directorySet;
            int _initialTimestamp;
            bool _initialTimestampSet;
            std::atomic_flag _accessingSettings;
            int64_t _previousShotTimestamp;
            std::size_t _shotIndex;
            std::unique_ptr<RecorderRenderer> _recorderRenderer;
    };
}
