#include "wave_renderer.h"
#include "theme.h"
#include <QPainterPath>
#include <cmath>
#include <memory>

namespace {
QString sourcePrefix(const QString &source) {
    return QString::number(source.size()) + ':' + source + '|';
}
void drawEnvelope(QPainter *p, const QRect &rect, const WaveData &wave, int width, const QColor &color,
                  int lastColumn = -1) {
    const double middle = rect.center().y(), amplitude = rect.height() * .44;
    const int columns = lastColumn >= 0 ? qMin(width, lastColumn + 1) : width;
    p->setPen(color);
    for (int x = 0; x < columns; ++x) {
        const int first = qint64(x) * wave.buckets.size() / width;
        const int end =
            qMin(int(wave.buckets.size()), qMax(first + 1, int(qint64(x + 1) * wave.buckets.size() / width)));
        float low = 1, high = -1;
        for (int b = first; b < end; ++b)
            for (const auto &peak : wave.buckets[b]) {
                low = qMin(low, peak.minimum);
                high = qMax(high, peak.maximum);
            }
        p->drawLine(rect.left() + x, int(middle - high * amplitude), rect.left() + x,
                    int(middle - low * amplitude));
    }
}
} // namespace

WaveRenderer::WaveRenderer(int budgetBytes) : cache_(qMax(0, budgetBytes)) {}
void WaveRenderer::paint(QPainter *painter, const QRect &rect, const QString &source, const WaveData &wave,
                         double fraction, bool approximate, int playX) {
    if (!painter || rect.isEmpty() || wave.buckets.isEmpty())
        return;
    fraction = std::isfinite(fraction) ? qBound(0.0, fraction, 1.0) : 1.0;
    const int width = qMax(1, int(rect.width() * fraction));
    const qreal dpr = painter->device()->devicePixelRatioF();
    // Align cached pixels to the destination device grid, including 125%/150% displays.
    const QPointF origin(std::floor(rect.left() * dpr) / dpr, std::floor(rect.top() * dpr) / dpr);
    const QPointF phase = (QPointF(rect.topLeft()) - origin) * dpr;
    const double physicalWidth = std::ceil(rect.width() * dpr + phase.x());
    const double physicalHeight = std::ceil(rect.height() * dpr + phase.y());
    const auto prefix = sourcePrefix(source);
    const auto &colors = theme::current();
    const auto key = prefix + QString("%1/%2/%3/%4/%5/%6/%7/%8/%9/%10/%11/%12/%13")
                                  .arg(rect.width())
                                  .arg(rect.height())
                                  .arg(dpr, 0, 'g', 17)
                                  .arg(phase.x(), 0, 'g', 17)
                                  .arg(phase.y(), 0, 'g', 17)
                                  .arg(width)
                                  .arg(approximate)
                                  .arg(wave.sampleRate)
                                  .arg(wave.channels)
                                  .arg(wave.frames)
                                  .arg(wave.stride)
                                  .arg(wave.buckets.size())
                                  .arg(theme::isDark());
    auto *raster = cache_.object(key);
    painter->save();
    painter->setClipRect(rect, Qt::IntersectClip);
    if (raster) {
        ++hits_;
    } else {
        // Charge both 32-bit pixel planes plus conservative object/key overhead. A minimum
        // reservation also bounds the number of tiny entries without a second unbounded map.
        const double cost = qMax(16384.0, physicalWidth * physicalHeight * 8 + key.size() * 2 + 1024);
        if (cost > cache_.maxCost()) {
            ++bypasses_;
        } else {
            auto candidate = std::make_unique<Raster>();
            const QSize pixels{int(physicalWidth), int(physicalHeight)};
            candidate->base = QPixmap(pixels);
            candidate->played = QPixmap(pixels);
            candidate->base.setDevicePixelRatio(dpr);
            candidate->played.setDevicePixelRatio(dpr);
            candidate->base.fill(Qt::transparent);
            candidate->played.fill(Qt::transparent);
            if (!candidate->base.isNull() && !candidate->played.isNull()) {
                const QRect local(QPoint(), rect.size());
                for (int played = 0; played < 2; ++played) {
                    QPainter layer(played ? &candidate->played : &candidate->base);
                    layer.translate(phase / dpr);
                    drawEnvelope(&layer, local, wave, width, played ? colors.played : colors.wave);
                }
                raster = candidate.get();
                cache_.insert(key, candidate.release(), int(cost));
                ++builds_;
            } else {
                ++bypasses_;
            }
        }
    }
    if (raster) {
        painter->drawPixmap(origin, raster->base);
        if (playX >= 0) {
            painter->save();
            painter->setClipRect(QRect(rect.topLeft(), QSize(qMin(playX + 1, rect.width()), rect.height())),
                                 Qt::IntersectClip);
            painter->drawPixmap(origin, raster->played);
            painter->restore();
        }
    } else {
        drawEnvelope(painter, rect, wave, width, colors.wave);
        if (playX >= 0) {
            // Use the same layer boundary as the cached path. At fractional DPR, adjacent
            // one-pixel columns overlap; changing pen color per column would shift the
            // boundary and show through the antialiased playback cursor.
            painter->save();
            painter->setClipRect(QRect(rect.topLeft(), QSize(qMin(playX + 1, rect.width()), rect.height())),
                                 Qt::IntersectClip);
            drawEnvelope(painter, rect, wave, width, colors.played, playX + 2);
            painter->restore();
        }
    }
    if (playX >= 0) {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const qreal cursorX = rect.left() + playX;
        painter->setPen(QPen(colors.played, 1.2));
        painter->drawLine(QPointF(cursorX, rect.top() + 3), QPointF(cursorX, rect.bottom()));
        painter->setPen(Qt::NoPen);
        painter->setBrush(colors.played);
        painter->drawEllipse(QPointF(cursorX, rect.top() + 2.5), 2.2, 2.2);
        painter->restore();
    }
    if (approximate) {
        // Keep the same readable waveform as verified audio. Only this compact mark
        // signals that positions on the waveform may be approximate.
        const QRect mark(rect.left() + 2, rect.top() + 2, 14, 14);
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(colors.referenceOutline, 1));
        painter->setBrush(colors.referenceBackground);
        painter->drawEllipse(mark);
        // A drawn approximation sign stays crisp at fractional scaling without a font glyph.
        painter->setPen(QPen(colors.warningText, 1.1, Qt::SolidLine, Qt::RoundCap));
        painter->setBrush(Qt::NoBrush);
        QPainterPath symbol;
        for (qreal y : {5.5, 8.5}) {
            symbol.moveTo(mark.left() + 3.5, mark.top() + y);
            symbol.cubicTo(mark.left() + 5.5, mark.top() + y - 2,
                           mark.left() + 8, mark.top() + y + 2,
                           mark.left() + 10.5, mark.top() + y);
        }
        painter->drawPath(symbol);
    }
    painter->restore();
}
void WaveRenderer::invalidate(const QString &source) {
    const auto prefix = sourcePrefix(source);
    for (const auto &key : cache_.keys())
        if (key.startsWith(prefix))
            cache_.remove(key);
}
WaveRenderer::Statistics WaveRenderer::statistics() const {
    return {hits_, builds_, bypasses_, int(cache_.totalCost()), int(cache_.size())};
}
