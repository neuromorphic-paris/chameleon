#pragma once

#include <QtQuick/QtQuick>
#include <array>
#include <atomic>
#include <cmath>
#include <unordered_map>

/// chameleon provides Qt components for event stream display.
namespace chameleon {

    /// centroid_display displays multiple centroids as circles.
    class centroid_display : public QQuickPaintedItem {
        Q_OBJECT
        Q_PROPERTY(QSize canvas_size READ canvas_size WRITE set_canvas_size)
        Q_PROPERTY(QColor stroke_color READ stroke_color WRITE set_stroke_color)
        Q_PROPERTY(qreal stroke_thickness READ stroke_thickness WRITE set_stroke_thickness)
        Q_PROPERTY(QColor fill_color READ fill_color WRITE set_fill_color)
        Q_PROPERTY(qreal radius READ radius WRITE set_radius)
        public:
        centroid_display(QQuickItem* parent = nullptr) :
            QQuickPaintedItem(parent),
            _brush(Qt::transparent, Qt::SolidPattern) {
            _pen.setColor(Qt::black);
            _pen.setWidthF(1);
            _accessing_centroids.clear(std::memory_order_release);
        }
        centroid_display(const centroid_display&) = delete;
        centroid_display(centroid_display&&) = default;
        centroid_display& operator=(const centroid_display&) = delete;
        centroid_display& operator=(centroid_display&&) = default;
        virtual ~centroid_display() {}

        /// set_canvas_size defines the display coordinates.
        virtual void set_canvas_size(QSize canvas_size) {
            _canvas_size = canvas_size;
        }

        /// canvas_size returns the currently used canvas_size.
        virtual QSize canvas_size() const {
            return _canvas_size;
        }

        /// set_stroke_color defines the stroke color for the centroids.
        virtual void set_stroke_color(QColor color) {
            _pen.setColor(color);
        }

        /// stroke_color returns the currently used stroke color.
        virtual QColor stroke_color() const {
            return _pen.color();
        }

        /// set_stroke_thickness defines the stroke thickness for the centroids.
        virtual void set_stroke_thickness(qreal thickness) {
            _pen.setWidthF(thickness);
        }

        /// stroke_thickness returns the currently used stroke thickness.
        virtual qreal stroke_thickness() const {
            return _pen.widthF();
        }

        /// set_fill_color defines the fill color for the centroids.
        virtual void set_fill_color(QColor color) {
            _brush.setColor(color);
        }

        /// fill_color returns the currently used fill color.
        virtual QColor fill_color() const {
            return _brush.color();
        }

        /// set_radius defines the radius level for gaussian representation.
        virtual void set_radius(qreal radius) {
            _radius = radius;
        }

        /// radius returns the currently used radius level.
        virtual qreal radius() const {
            return _radius;
        }

        /// insert displays a centroid, which can be updated later on using its id.
        template <typename Centroid>
        void insert(std::size_t id, Centroid centroid) {
            while (_accessing_centroids.test_and_set(std::memory_order_acquire)) {
            }
            auto id_and_centroid_and_took_place = _id_to_centroid.insert(
                {id, managed_centroid{centroid.x, centroid.y}});
            if (!id_and_centroid_and_took_place.second) {
                _accessing_centroids.clear(std::memory_order_release);
                throw std::logic_error("the given centroid id was already inserted");
            }
            _accessing_centroids.clear(std::memory_order_release);
        }

        /// update modifies the parameters of an existing centroid.
        template <typename Centroid>
        void update(std::size_t id, Centroid centroid) {
            while (_accessing_centroids.test_and_set(std::memory_order_acquire)) {
            }
            auto id_and_centroid_candidate = _id_to_centroid.find(id);
            if (id_and_centroid_candidate == _id_to_centroid.end()) {
                _accessing_centroids.clear(std::memory_order_release);
                throw std::logic_error("the given centroid id was not registered with insert");
            }
            id_and_centroid_candidate->second.x = centroid.x;
            id_and_centroid_candidate->second.y = centroid.y;
            _accessing_centroids.clear(std::memory_order_release);
        }

        /// erase removes an existing centroid.
        virtual void erase(std::size_t id) {
            while (_accessing_centroids.test_and_set(std::memory_order_acquire)) {
            }
            if (_id_to_centroid.erase(id) == 0) {
                _accessing_centroids.clear(std::memory_order_release);
                throw std::logic_error("the given centroid id was not registered with insert");
            }
            _accessing_centroids.clear(std::memory_order_release);
        }

        /// paint is called by the render thread when drawing is required.
        virtual void paint(QPainter* painter) override {
            painter->setPen(_pen);
            painter->setBrush(_brush);
            painter->setRenderHint(QPainter::Antialiasing);
            while (_accessing_centroids.test_and_set(std::memory_order_acquire)) {
            }
            for (auto id_and_centroid : _id_to_centroid) {
                painter->resetTransform();
                painter->setWindow(0, 0, _canvas_size.width(), _canvas_size.height());
                painter->translate(id_and_centroid.second.x, _canvas_size.height() - 1 - id_and_centroid.second.y);
                painter->drawEllipse(QPointF(), _radius, _radius);
            }
            _accessing_centroids.clear(std::memory_order_release);
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
        /// managed_centroid represents a circular centroid.
        struct managed_centroid {
            float x;
            float y;
        };

        QSize _canvas_size;
        QPen _pen;
        QBrush _brush;
        qreal _radius;
        std::unordered_map<std::size_t, managed_centroid> _id_to_centroid;
        std::atomic_flag _accessing_centroids;
    };
}
