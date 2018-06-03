#pragma once

#include <QtQuick/QtQuick>
#include <array>
#include <atomic>
#include <cmath>
#include <unordered_map>

/// chameleon provides Qt components for event stream display.
namespace chameleon {

    /// blob_display displays gaussian blobs as ellipses.
    class blob_display : public QQuickPaintedItem {
        Q_OBJECT
        Q_PROPERTY(QSize canvas_size READ canvas_size WRITE set_canvas_size)
        Q_PROPERTY(QColor stroke_color READ stroke_color WRITE set_stroke_color)
        Q_PROPERTY(qreal stroke_thickness READ stroke_thickness WRITE set_stroke_thickness)
        Q_PROPERTY(QColor fill_color READ fill_color WRITE set_fill_color)
        Q_PROPERTY(qreal confidence READ confidence WRITE set_confidence)
        public:
        blob_display(QQuickItem* parent = nullptr) :
            QQuickPaintedItem(parent),
            _brush(Qt::transparent, Qt::SolidPattern),
            _confidence(1.96f) {
            _pen.setColor(Qt::black);
            _pen.setWidthF(1);
            _accessing_blobs.clear(std::memory_order_release);
        }
        blob_display(const blob_display&) = delete;
        blob_display(blob_display&&) = default;
        blob_display& operator=(const blob_display&) = delete;
        blob_display& operator=(blob_display&&) = default;
        virtual ~blob_display() {}

        /// set_canvas_size defines the display coordinates.
        virtual void set_canvas_size(QSize canvas_size) {
            _canvas_size = canvas_size;
        }

        /// canvas_size returns the currently used canvas_size.
        virtual QSize canvas_size() const {
            return _canvas_size;
        }

        /// set_stroke_color defines the stroke color for the blobs.
        virtual void set_stroke_color(QColor color) {
            _pen.setColor(color);
        }

        /// stroke_color returns the currently used stroke color.
        virtual QColor stroke_color() const {
            return _pen.color();
        }

        /// set_stroke_thickness defines the stroke thickness for the blobs.
        virtual void set_stroke_thickness(qreal thickness) {
            _pen.setWidthF(thickness);
        }

        /// stroke_thickness returns the currently used stroke thickness.
        virtual qreal stroke_thickness() const {
            return _pen.widthF();
        }

        /// set_fill_color defines the fill color for the blobs.
        virtual void set_fill_color(QColor color) {
            _brush.setColor(color);
        }

        /// fill_color returns the currently used fill color.
        virtual QColor fill_color() const {
            return _brush.color();
        }

        /// set_confidence defines the confidence level for gaussian representation.
        virtual void set_confidence(qreal confidence) {
            _confidence = confidence;
        }

        /// confidence returns the currently used confidence level.
        virtual qreal confidence() const {
            return _confidence;
        }

        /// insert displays a blob, which can be updated later on using its id.
        template <typename Blob>
        void insert(std::size_t id, Blob blob) {
            while (_accessing_blobs.test_and_set(std::memory_order_acquire)) {
            }
            auto id_and_blob_and_took_place = _id_to_blob.insert(
                {id, managed_blob{blob.x, blob.y, blob.sigma_x_squared, blob.sigma_xy, blob.sigma_y_squared}});
            if (!id_and_blob_and_took_place.second) {
                _accessing_blobs.clear(std::memory_order_release);
                throw std::logic_error("the given blob id was already inserted");
            }
            _accessing_blobs.clear(std::memory_order_release);
        }

        /// update modifies the parameters of an existing blob.
        template <typename Blob>
        void update(std::size_t id, Blob blob) {
            while (_accessing_blobs.test_and_set(std::memory_order_acquire)) {
            }
            auto id_and_blob_candidate = _id_to_blob.find(id);
            if (id_and_blob_candidate == _id_to_blob.end()) {
                _accessing_blobs.clear(std::memory_order_release);
                throw std::logic_error("the given blob id was not registered with insert");
            }
            id_and_blob_candidate->second.x = blob.x;
            id_and_blob_candidate->second.y = blob.y;
            id_and_blob_candidate->second.sigma_x_squared = blob.sigma_x_squared;
            id_and_blob_candidate->second.sigma_xy = blob.sigma_xy;
            id_and_blob_candidate->second.sigma_y_squared = blob.sigma_y_squared;
            _accessing_blobs.clear(std::memory_order_release);
        }

        /// erase removes an existing blob.
        virtual void erase(std::size_t id) {
            while (_accessing_blobs.test_and_set(std::memory_order_acquire)) {
            }
            if (_id_to_blob.erase(id) == 0) {
                _accessing_blobs.clear(std::memory_order_release);
                throw std::logic_error("the given blob id was not registered with insert");
            }
            _accessing_blobs.clear(std::memory_order_release);
        }

        /// paint is called by the render thread when drawing is required.
        virtual void paint(QPainter* painter) override {
            painter->setPen(_pen);
            painter->setBrush(_brush);
            painter->setRenderHint(QPainter::Antialiasing);
            while (_accessing_blobs.test_and_set(std::memory_order_acquire)) {
            }
            for (auto id_and_blob : _id_to_blob) {
                painter->resetTransform();
                painter->setWindow(0, 0, _canvas_size.width(), _canvas_size.height());
                painter->translate(id_and_blob.second.x, _canvas_size.height() - 1 - id_and_blob.second.y);
                const auto ellipse = blob_to_ellipse(id_and_blob.second, _confidence);
                painter->rotate(-ellipse.angle / M_PI * 180);
                painter->drawEllipse(QPointF(), ellipse.major_radius, ellipse.minor_radius);
            }
            _accessing_blobs.clear(std::memory_order_release);
        }

        public slots:
        /// trigger_draw requests a window refresh.
        void trigger_draw() {
            QQuickPaintedItem::update();
            if (window()) {
                window()->update();
            }
        }

        protected:
        /// managed_blob represents a gaussian blob.
        struct managed_blob {
            float x;
            float y;
            float sigma_x_squared;
            float sigma_xy;
            float sigma_y_squared;
        };

        /// ellipse represents an ellipse's parameter.
        struct ellipse {
            float major_radius;
            float minor_radius;
            float angle;
        };

        /// blob_to_ellipse calculates an ellipse's parameters from a blob.
        static ellipse blob_to_ellipse(managed_blob blob, float confidence) {
            const auto delta_square_root =
                std::sqrt(
                    std::pow(blob.sigma_x_squared - blob.sigma_y_squared, 2.0f) + 4 * std::pow(blob.sigma_xy, 2.0f))
                / 2;
            const auto first_order_coefficient = (blob.sigma_x_squared + blob.sigma_y_squared) / 2;
            return ellipse{
                confidence * std::sqrt(first_order_coefficient + delta_square_root),
                confidence * std::sqrt(first_order_coefficient - delta_square_root),
                blob.sigma_y_squared == blob.sigma_x_squared ?
                    static_cast<float>(M_PI) / 4 :
                    (std::atan(2 * blob.sigma_xy / (blob.sigma_x_squared - blob.sigma_y_squared))
                     + (blob.sigma_y_squared > blob.sigma_x_squared ? static_cast<float>(M_PI) : 0.0f))
                        / 2,
            };
        }

        QSize _canvas_size;
        QPen _pen;
        QBrush _brush;
        qreal _confidence;
        std::unordered_map<std::size_t, managed_blob> _id_to_blob;
        std::atomic_flag _accessing_blobs;
    };
}
