#pragma once

#include <QCache>
#include <QColor>
#include <QIcon>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <cmath>

namespace ui {
enum class Icon {
    Search, Folder, Library, Music, Star, Plus, Refresh, Settings, ChevronLeft,
    ChevronRight, Play, Pause, Volume, Tag, Workspace, StarFilled
};

namespace detail {
// GUI-thread only. Both caches are bounded; a large library does not create an icon per row.
inline QCache<QString, QPixmap> &iconRasters() {
    static QCache<QString, QPixmap> cache(2 * 1024 * 1024);
    return cache;
}

inline void drawIcon(QPainter *painter, const QRectF &rect, Icon kind, QColor color, QIcon::Mode mode) {
    if (rect.isEmpty())
        return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const qreal side = qMin(rect.width(), rect.height());
    painter->translate(rect.center() - QPointF(side / 2, side / 2));
    painter->scale(side / 20, side / 20);
    if (mode == QIcon::Disabled)
        color.setAlphaF(color.alphaF() * .4);
    painter->setPen(QPen(color, 1.45, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    QPainterPath path;
    switch (kind) {
    case Icon::Search:
        painter->drawEllipse(QRectF(3, 3, 10.5, 10.5));
        painter->drawLine(QPointF(12, 12), QPointF(17, 17));
        break;
    case Icon::Folder:
        path.moveTo(2.5, 6.5);
        path.lineTo(2.5, 5);
        path.quadTo(2.5, 3.5, 4, 3.5);
        path.lineTo(7.2, 3.5);
        path.lineTo(9, 5.5);
        path.lineTo(16, 5.5);
        path.quadTo(17.5, 5.5, 17.5, 7);
        path.lineTo(17.5, 15);
        path.quadTo(17.5, 16.5, 16, 16.5);
        path.lineTo(4, 16.5);
        path.quadTo(2.5, 16.5, 2.5, 15);
        path.closeSubpath();
        painter->drawPath(path);
        painter->drawLine(QPointF(2.5, 7), QPointF(17.5, 7));
        break;
    case Icon::Library:
        painter->drawRoundedRect(QRectF(2.5, 3.5, 4, 13), 1, 1);
        painter->drawRoundedRect(QRectF(8.5, 3.5, 3.5, 13), 1, 1);
        painter->drawLine(QPointF(14, 4), QPointF(17.5, 16));
        painter->drawLine(QPointF(3, 7), QPointF(6, 7));
        break;
    case Icon::Music:
        path.moveTo(7.5, 14.5);
        path.lineTo(7.5, 5.5);
        path.lineTo(16, 3.5);
        path.lineTo(16, 12.5);
        painter->drawPath(path);
        painter->drawLine(QPointF(7.5, 8.5), QPointF(16, 6.5));
        painter->drawEllipse(QRectF(2.5, 12, 5, 4));
        painter->drawEllipse(QRectF(11, 10, 5, 4));
        break;
    case Icon::Star:
    case Icon::StarFilled:
        for (int i = 0; i < 10; ++i) {
            const qreal angle = -1.5707963267948966 + i * .3141592653589793 * 2;
            const qreal radius = i % 2 == 0 ? 7.5 : 3.6;
            const QPointF point(10 + std::cos(angle) * radius, 10 + std::sin(angle) * radius);
            if (i == 0)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        path.closeSubpath();
        if (kind == Icon::StarFilled)
            painter->setBrush(color);
        painter->drawPath(path);
        break;
    case Icon::Plus:
        painter->drawLine(QPointF(10, 4), QPointF(10, 16));
        painter->drawLine(QPointF(4, 10), QPointF(16, 10));
        break;
    case Icon::Refresh:
        path.moveTo(16.3, 7);
        path.cubicTo(14.8, 3.5, 10.4, 2.5, 7.1, 4.2);
        path.cubicTo(3.7, 6, 2.8, 10, 4.6, 13.3);
        path.cubicTo(6.4, 16.7, 10.9, 17.6, 14.1, 15.5);
        painter->drawPath(path);
        path = {};
        path.moveTo(16.5, 3);
        path.lineTo(16.5, 7.5);
        path.lineTo(12, 7.5);
        painter->drawPath(path);
        break;
    case Icon::Settings:
        for (int i = 0; i < 12; ++i) {
            const qreal angle = i * .5235987755982989;
            const qreal radius = i % 2 ? 6 : 8;
            const QPointF point(10 + std::cos(angle) * radius, 10 + std::sin(angle) * radius);
            if (i == 0)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        path.closeSubpath();
        painter->drawPath(path);
        painter->drawEllipse(QRectF(7.5, 7.5, 5, 5));
        break;
    case Icon::ChevronLeft:
    case Icon::ChevronRight: {
        const qreal direction = kind == Icon::ChevronLeft ? 1 : -1;
        path.moveTo(10 + 2.5 * direction, 5);
        path.lineTo(10 - 2.5 * direction, 10);
        path.lineTo(10 + 2.5 * direction, 15);
        painter->drawPath(path);
        break;
    }
    case Icon::Play:
        path.moveTo(7, 4.5);
        path.lineTo(15, 10);
        path.lineTo(7, 15.5);
        path.closeSubpath();
        painter->setPen(Qt::NoPen);
        painter->fillPath(path, color);
        break;
    case Icon::Pause:
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawRoundedRect(QRectF(5.5, 4.5, 3, 11), .8, .8);
        painter->drawRoundedRect(QRectF(11.5, 4.5, 3, 11), .8, .8);
        break;
    case Icon::Volume:
        path.moveTo(3, 7.5);
        path.lineTo(6, 7.5);
        path.lineTo(10, 4);
        path.lineTo(10, 16);
        path.lineTo(6, 12.5);
        path.lineTo(3, 12.5);
        path.closeSubpath();
        painter->drawPath(path);
        path = {};
        path.moveTo(13, 7);
        path.quadTo(16, 10, 13, 13);
        path.moveTo(15.5, 4.5);
        path.quadTo(20, 10, 15.5, 15.5);
        painter->drawPath(path);
        break;
    case Icon::Tag:
        path.moveTo(3, 3.5);
        path.lineTo(9, 3.5);
        path.lineTo(17, 11.5);
        path.lineTo(11, 17.5);
        path.lineTo(3, 9.5);
        path.closeSubpath();
        painter->drawPath(path);
        painter->drawEllipse(QRectF(5.5, 6, 1.5, 1.5));
        break;
    case Icon::Workspace:
        painter->drawRoundedRect(QRectF(2.5, 3.5, 15, 13), 2, 2);
        painter->drawLine(QPointF(7, 4), QPointF(7, 16));
        painter->drawLine(QPointF(10, 7.5), QPointF(14.5, 7.5));
        painter->drawLine(QPointF(10, 11), QPointF(13, 11));
        break;
    }
    painter->restore();
}

class IconEngine final : public QIconEngine {
  public:
    IconEngine(Icon kind, QColor color, int size) : kind_(kind), color_(color), size_(size) {}
    QIconEngine *clone() const override { return new IconEngine(kind_, color_, size_); }
    bool isNull() override { return false; }
    QString key() const override { return QStringLiteral("YuanAudioLineIcon"); }
    QList<QSize> availableSizes(QIcon::Mode, QIcon::State) override { return {QSize(size_, size_)}; }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        drawIcon(painter, rect, kind_, color_, mode);
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1);
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State, qreal scale) override {
        if (size.isEmpty() || !std::isfinite(scale) || scale <= 0)
            return {};
        const auto cacheKey = QStringLiteral("%1/%2/%3/%4/%5/%6")
                                  .arg(int(kind_)).arg(color_.rgba()).arg(size.width()).arg(size.height())
                                  .arg(scale, 0, 'g', 12).arg(int(mode));
        auto &cache = iconRasters();
        if (const auto *cached = cache.object(cacheKey))
            return *cached;
        QPixmap pixmap(QSize(int(std::ceil(size.width() * scale)), int(std::ceil(size.height() * scale))));
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        {
            QPainter painter(&pixmap);
            drawIcon(&painter, QRectF(QPointF(), QSizeF(size)), kind_, color_, mode);
        }
        const qint64 cost = qint64(pixmap.width()) * pixmap.height() * 4 + 256;
        if (cost <= cache.maxCost())
            cache.insert(cacheKey, new QPixmap(pixmap), int(cost));
        return pixmap;
    }

  private:
    Icon kind_;
    QColor color_;
    int size_;
};
} // namespace detail

inline QIcon icon(Icon kind, const QColor &color, int size = 20) {
    static QCache<QString, QIcon> cache(128);
    size = qBound(8, size, 64);
    const auto key = QStringLiteral("%1/%2/%3").arg(int(kind)).arg(color.rgba()).arg(size);
    if (const auto *cached = cache.object(key))
        return *cached;
    QIcon result(new detail::IconEngine(kind, color, size));
    cache.insert(key, new QIcon(result));
    return result;
}
} // namespace ui
