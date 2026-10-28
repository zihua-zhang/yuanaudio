#include "wave_renderer.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <cstdio>

static void legacyPaint(QPainter *p, const QRect &r, const WaveData &wave, int playX) {
    const int width = r.width();
    const double middle = r.center().y(), amplitude = r.height() * .44;
    for (int x = 0; x < width; ++x) {
        const int first = x * wave.buckets.size() / width;
        const int end =
            qMin(int(wave.buckets.size()), qMax(first + 1, int((x + 1) * wave.buckets.size() / width)));
        float low = 1, high = -1;
        for (int b = first; b < end; ++b)
            for (const auto &peak : wave.buckets[b]) {
                low = qMin(low, peak.minimum);
                high = qMax(high, peak.maximum);
            }
        p->setPen(QColor(x <= playX ? "#007aff" : "#8a9bad"));
        p->drawLine(r.left() + x, int(middle - high * amplitude), r.left() + x,
                    int(middle - low * amplitude));
    }
    p->setPen(QPen(QColor("#007aff"), 2));
    p->drawLine(r.left() + playX, r.top(), r.left() + playX, r.bottom());
}
static QJsonObject report(QList<double> samples) {
    std::sort(samples.begin(), samples.end());
    double total = 0;
    for (double sample : samples)
        total += sample;
    return {{"samples", samples.size()},
            {"total_ms", total},
            {"p50_ms", samples[samples.size() / 2]},
            {"p95_ms", samples[int(samples.size() * .95) - 1]},
            {"max_ms", samples.last()}};
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    WaveData wave;
    wave.channels = 2;
    wave.sampleRate = 48000;
    wave.stride = 4096;
    wave.frames = wave.stride * 2048;
    wave.buckets.reserve(2048);
    for (int i = 0; i < 2048; ++i)
        wave.buckets << QVector<Peak>{{float(-.2 - .7 * std::abs(std::sin(i * .031))), .2f},
                                      {-.1f, float(.1 + .8 * std::abs(std::cos(i * .041)))}};
    const QRect rect(8, 7, 720, 38);
    const qreal dpr = 1.5;
    QImage image(QSize(1100, 80), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    QPainter painter(&image);
    WaveRenderer renderer;
    QList<double> legacy, cached;
    QElapsedTimer timer;
    timer.start();
    renderer.paint(&painter, rect, "synthetic", wave, 1, false, 0);
    const auto coldMs = timer.nsecsElapsed() / 1e6;
    // Interleave the two paths so transient machine load does not favor a sequential run.
    for (int sample = 0; sample < 1200; ++sample) {
        const int cursor = sample % rect.width();
        timer.restart();
        legacyPaint(&painter, rect, wave, cursor);
        legacy << timer.nsecsElapsed() / 1e6;
        timer.restart();
        renderer.paint(&painter, rect, "synthetic", wave, 1, false, cursor);
        cached << timer.nsecsElapsed() / 1e6;
    }
    const auto stats = renderer.statistics();
    const auto json =
        QJsonDocument(
            QJsonObject{
                {"scope",
                 "Synthetic one-row waveform envelope repaint; excludes I/O, decoding and playback latency"},
                {"buckets", wave.buckets.size()},
                {"channels", 2},
                {"logical_width", rect.width()},
                {"logical_height", rect.height()},
                {"dpr", dpr},
                {"cold_raster_ms", coldMs},
                {"legacy", report(legacy)},
                {"cached", report(cached)},
                {"cache_bytes", stats.bytes},
                {"cache_entries", stats.entries},
                {"raster_builds", qint64(stats.builds)},
                {"cache_hits", qint64(stats.hits)}})
            .toJson();
    fwrite(json.constData(), 1, json.size(), stdout);
    return 0;
}
