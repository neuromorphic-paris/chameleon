#pragma once

#include <QtQuick/QtQuick>
#include <QtQuick/qquickwindow.h>
#include <atomic>
#include <cmath>
#include <memory>
#include <iostream>

/// chameleon provides Qt components for event stream display.
namespace chameleon {

    /// grid_display displays a grids as circles.
    class grid_display : public QQuickPaintedItem {
        Q_OBJECT
        Q_PROPERTY(QSize canvas_size READ canvas_size WRITE set_canvas_size)
        Q_PROPERTY(QColor stroke_color READ stroke_color WRITE set_stroke_color)
        Q_PROPERTY(qreal offset_x READ offset_x WRITE set_offset_x)
        Q_PROPERTY(qreal offset_y READ offset_y WRITE set_offset_y)
        Q_PROPERTY(qreal pitch READ pitch WRITE set_pitch)
        public:
        grid_display(QQuickItem* parent = nullptr) :
            QQuickPaintedItem(parent),
            _brush(Qt::transparent, Qt::SolidPattern),
            _stroke_color(Qt::white),
            _offset_x(0.0f),
            _offset_y(0.0f),
            _pitch(20.0f) {
            _pen.setColor(Qt::black);
            _pen.setWidthF(1);

            if (_offset_x > _pitch) {
                _offset_x -= _pitch;
            }
            if (_offset_y > _pitch) {
                _offset_y -= _pitch;
            }
        }
        grid_display(const grid_display&) = delete;
        grid_display(grid_display&&) = default;
        grid_display& operator=(const grid_display&) = delete;
        grid_display& operator=(grid_display&&) = default;
        virtual ~grid_display() {}

        /// set_canvas_size defines the display coordinates.
        virtual void set_canvas_size(QSize canvas_size) {
            _canvas_size = canvas_size;
            // setImplicitWidth(canvas_size.width());
            // setImplicitHeight(canvas_size.height());
        }

        /// canvas_size returns the currently used canvas_size.
        virtual QSize canvas_size() const {
            return _canvas_size;
        }

        /// set_stroke_color defines the stroke color for the grids.
        virtual void set_stroke_color(QColor stroke_color) {
            _stroke_color = stroke_color;
        }

        /// stroke_color returns the currently used stroke color.
        virtual QColor stroke_color() const {
            return _stroke_color;
        }

        /// set_offset_x defines the x offsetfor the grids.
        virtual void set_offset_x(qreal offset_x) {
            _offset_x = offset_x;
        }

        /// offset_x returns the current x offset.
        virtual qreal offset_x() const {
            return _offset_x;
        }

        /// set_offset_y defines the y offsetfor the grids.
        virtual void set_offset_y(qreal offset_y) {
            _offset_y = offset_y;
        }

        /// offset_y returns the current y offset.
        virtual qreal offset_y() const {
            return _offset_y;
        }

        /// set_pitch defines the pitch level for gaussian representation.
        virtual void set_pitch(qreal pitch) {
            _pitch = pitch;
        }

        /// pitch returns the currently used pitch level.
        virtual qreal pitch() const {
            return _pitch;
        }

        /// paint is called by the render thread when drawing is required.
        virtual void paint(QPainter* painter) override {
            _pen.setColor(_stroke_color);
            painter->setPen(_pen);
            painter->setBrush(_brush);
            painter->setRenderHint(QPainter::Antialiasing);
            painter->resetTransform();

            auto clear_area =
                QRectF(0, 0, width() * window()->devicePixelRatio(), height() * window()->devicePixelRatio());
            if (clear_area != _clear_area) {
                _clear_area = std::move(clear_area);
                if (clear_area.width() * _canvas_size.height() > clear_area.height() * _canvas_size.width()) {
                    _paint_area.setWidth(clear_area.height() * _canvas_size.width() / _canvas_size.height());
                    _paint_area.setHeight(clear_area.height());
                    _paint_area.moveLeft(clear_area.left() + (clear_area.width() - _paint_area.width()) / 2);
                    _paint_area.moveTop(clear_area.top());
                } else {
                    _paint_area.setWidth(clear_area.width());
                    _paint_area.setHeight(clear_area.width() * _canvas_size.height() / _canvas_size.width());
                    _paint_area.moveLeft(clear_area.left());
                    _paint_area.moveTop(clear_area.top() + (clear_area.height() - _paint_area.height()) / 2);
                }
            }
            
            painter->setWindow(_clear_area.left(), _clear_area.top(),
                               _clear_area.width(), _clear_area.height());

            const float xscale = _paint_area.width() / _canvas_size.width();
            const float yscale = _paint_area.height() / _canvas_size.height();

            // draw the boarder
            painter->drawRect(_paint_area.left() + 1,
                              _paint_area.top() + 1,
                              _paint_area.width() - 2,
                              _paint_area.height() - 2);

            // draw horizontal lines
            const int x1 = static_cast<int>(_paint_area.left());
            const int x2 = static_cast<int>(_paint_area.right());
            for (float y = _offset_y * yscale; y < _paint_area.bottom(); y += _pitch * yscale) {
                painter->drawLine(x1, y, x2, y);
            }

            // draw vertical lines
            const int y1 = static_cast<int>(_paint_area.top());
            const int y2 = static_cast<int>(_paint_area.bottom());
            for (float x = _paint_area.left() + _offset_x * xscale; x < _paint_area.right(); x += _pitch * xscale) {
                painter->drawLine(x, y1, x, y2);
            }
        }

        /// paint_area returns the paint area in window coordinates.
        virtual QRectF paint_area() const {
            return _paint_area;
        }

        public slots:

        /// trigger_draw requests a window refresh.
        void trigger_draw() {
            QQuickItem::update();
            if (window()) {
                window()->update();
            }
        }

        protected:
        QSize _canvas_size;
        QColor _stroke_color;
        qreal _offset_x;
        qreal _offset_y;
        qreal _pitch;
        QPen _pen;
        QBrush _brush;
        QRectF _clear_area;
        QRectF _paint_area;
    };
}
