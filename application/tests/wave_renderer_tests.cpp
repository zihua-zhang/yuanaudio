#include "wave_renderer.h"
#include "theme.h"
#include <QApplication>
#include <QImage>
#include <QtTest>
#include <cmath>

static WaveData fixture() {
    WaveData wave;
    wave.sampleRate = 48000;
    wave.channels = 2;
    wave.frames = 256 * 127;
    for (int i = 0; i < 127; ++i)
        wave.buckets << QVector<Peak>{{float(-.2 - .7 * std::abs(std::sin(i * .31))), .2f},
                                      {-.1f, float(.1 + .8 * std::abs(std::cos(i * .41)))}};
    return wave;
}
// Independent copy of the original direct envelope drawing. Cursor styling can evolve,
// but the cached envelope must still match direct rendering outside its narrow overlay.
static void legacyPaint(QPainter *p, const QRect &r, const WaveData &wave, double fraction, bool /*approximate*/,
                        int playX) {
    const int width = qMax(1, int(r.width() * fraction));
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
        p->setPen(x <= playX ? theme::current().played : theme::current().wave);
        p->drawLine(r.left() + x, int(middle - high * amplitude), r.left() + x,
                    int(middle - low * amplitude));
    }
    if (playX >= 0) {
        p->setPen(QPen(theme::current().played, 2));
        p->drawLine(r.left() + playX, r.top(), r.left() + playX, r.bottom());
    }
}
static bool sameEnvelope(const QImage &actual, const QImage &expected, int cursor = 71, int offsetX = 0) {
    if (actual.size() != expected.size())
        return false;
    const qreal dpr = actual.devicePixelRatio();
    for (int y = 0; y < actual.height(); ++y)
        for (int x = 0; x < actual.width(); ++x) {
            // Allow only the cursor's 6 logical-pixel band to differ from the old style.
            if (cursor >= 0 && x >= std::floor((offsetX + cursor - 3) * dpr) &&
                x <= std::ceil((offsetX + cursor + 3) * dpr))
                continue;
            if (actual.pixel(x, y) != expected.pixel(x, y))
                return false;
        }
    return true;
}
static QImage render(WaveRenderer *renderer, const QString &source, const WaveData &wave,
                     QSize size = {173, 38}, qreal dpr = 1, double fraction = 1, bool approximate = false,
                     int playX = 71, QPoint offset = {}) {
    QImage image(QSize(qCeil((size.width() + offset.x()) * dpr), qCeil((size.height() + offset.y()) * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::white);
    QPainter painter(&image);
    const QRect rect(offset, size);
    painter.setClipRect(rect);
    if (renderer)
        renderer->paint(&painter, rect, source, wave, fraction, approximate, playX);
    else
        legacyPaint(&painter, rect, wave, fraction, approximate, playX);
    return image;
}
class WaveRendererTests : public QObject {
    Q_OBJECT
  private slots:
    void cachedEnvelopeMatchesLegacyAcrossChannelsPartialProgressAndDpr() {
        const auto wave = fixture();
        WaveRenderer renderer;
        for (qreal dpr : {1.0, 1.25, 1.5, 2.0})
            for (double fraction : {.4, 1.0})
                for (int cursor : {-1, 0, 71, 172})
                    for (const QPoint offset : {QPoint(), QPoint(8, 7), QPoint(8, 59)}) {
                        const auto actual =
                            render(&renderer, "source", wave, {173, 38}, dpr, fraction, false, cursor, offset);
                        const auto expected =
                            render(nullptr, "source", wave, {173, 38}, dpr, fraction, false, cursor, offset);
                        QVERIFY2(sameEnvelope(actual, expected, cursor, offset.x()),
                                 qPrintable(QString("DPR %1, fraction %2, cursor %3")
                                                .arg(dpr)
                                                .arg(fraction)
                                                .arg(cursor)));
                    }
    }
    void cursorStylingMatchesUncachedRenderingAcrossDprAndStaysInsideWaveform() {
        const auto wave = fixture();
        WaveRenderer cached;
        WaveRenderer uncached(0);
        for (qreal dpr : {1.0, 1.25, 1.5, 2.0})
            for (int cursor : {0, 71, 172}) {
                const auto actual = render(&cached, "cursor", wave, {173, 38}, dpr, 1, false, cursor, {8, 7});
                const auto direct = render(&uncached, "cursor", wave, {173, 38}, dpr, 1, false, cursor, {8, 7});
                QVERIFY2(actual == direct, qPrintable(QString("Cached/direct cursor mismatch: DPR %1, x %2")
                                                          .arg(dpr).arg(cursor)));
                for (int y = 0; y < actual.height(); ++y)
                    for (int x = 0; x < actual.width(); ++x)
                        if (x < std::floor(8 * dpr) || y < std::floor(7 * dpr))
                            QCOMPARE(actual.pixelColor(x, y), QColor(Qt::white));
            }
        QCOMPARE(cached.statistics().builds, quint64(4));
        QCOMPARE(cached.statistics().hits, quint64(8));
        QCOMPARE(uncached.statistics().bypasses, quint64(12));
    }
    void referenceWaveOnlyAddsCompactStartMarker() {
        const auto wave = fixture();
        WaveRenderer renderer;
        for (double fraction : {.4, 1.0})
            for (int cursor : {-1, 71, 172}) {
                const auto regular = render(&renderer, "regular", wave, {173, 38}, 1, fraction, false, cursor);
                const auto reference = render(&renderer, "reference", wave, {173, 38}, 1, fraction, true, cursor);
                int markerPixels = 0;
                for (int y = 0; y < reference.height(); ++y)
                    for (int x = 0; x < reference.width(); ++x) {
                        if (reference.pixel(x, y) != regular.pixel(x, y)) {
                            QVERIFY2(x <= 17 && y <= 17,
                                     qPrintable(QString("Reference marker leaked to (%1, %2)").arg(x).arg(y)));
                            ++markerPixels;
                        }
                    }
                QVERIFY(markerPixels > 10);
            }
    }
    void playbackCursorReusesEnvelopeAndSourceInvalidationIsSelective() {
        auto wave = fixture();
        WaveRenderer renderer;
        render(&renderer, "a", wave);
        render(&renderer, "b", wave);
        for (int cursor = 0; cursor < 150; ++cursor)
            render(&renderer, "a", wave, {173, 38}, 1, 1, false, cursor);
        QCOMPARE(renderer.statistics().builds, quint64(2));
        QCOMPARE(renderer.statistics().hits, quint64(150));
        renderer.invalidate("a");
        render(&renderer, "b", wave);
        for (auto &bucket : wave.buckets)
            for (auto &peak : bucket)
                peak = {-.1f, .1f};
        QVERIFY(sameEnvelope(render(&renderer, "a", wave), render(nullptr, "a", wave)));
        QCOMPARE(renderer.statistics().builds, quint64(3));
        QCOMPARE(renderer.statistics().hits, quint64(151));
    }
    void geometryFractionAndSourceShapeCannotReuseStaleRaster() {
        auto wave = fixture();
        WaveRenderer renderer;
        render(&renderer, "source", wave);
        render(&renderer, "source", wave, {174, 38});
        render(&renderer, "source", wave, {173, 39});
        render(&renderer, "source", wave, {173, 38}, 2);
        render(&renderer, "source", wave, {173, 38}, 1, .5);
        render(&renderer, "source", wave, {173, 38}, 1, 1, true);
        wave.frames += 256;
        wave.buckets << QVector<Peak>{{-1, 1}, {-1, 1}};
        QVERIFY(sameEnvelope(render(&renderer, "source", wave), render(nullptr, "source", wave)));
        QCOMPARE(renderer.statistics().builds, quint64(7));
    }
    void memoryBudgetEvictsOldSourcesAndOversizedDrawingBypassesAllocation() {
        auto wave = fixture();
        WaveRenderer renderer(65536);
        for (int i = 0; i < 20; ++i) {
            render(&renderer, QString::number(i), wave);
            QVERIFY(renderer.statistics().bytes <= 65536);
            QVERIFY(renderer.statistics().entries <= 1);
        }
        QCOMPARE(renderer.statistics().builds, quint64(20));
        QVERIFY(sameEnvelope(render(&renderer, "oversized", wave, {1024, 128}),
                             render(nullptr, "oversized", wave, {1024, 128})));
        QCOMPARE(renderer.statistics().bypasses, quint64(1));
        QVERIFY(renderer.statistics().bytes <= 65536);
        QCOMPARE(renderer.statistics().builds, quint64(20));
    }
    void appearanceChangeRebuildsWaveformOnceAndKeepsCursorCheap() {
        const auto wave = fixture();
        WaveRenderer renderer;
        qApp->setProperty("sourceAudioDarkMode", false);
        const auto light = render(&renderer, "theme", wave);
        QCOMPARE(renderer.statistics().builds, quint64(1));
        qApp->setProperty("sourceAudioDarkMode", true);
        const auto dark = render(&renderer, "theme", wave);
        QVERIFY(light != dark);
        QCOMPARE(renderer.statistics().builds, quint64(2));
        render(&renderer, "theme", wave, {173, 38}, 1, 1, false, 90);
        QCOMPARE(renderer.statistics().builds, quint64(2));
        qApp->setProperty("sourceAudioDarkMode", false);
    }
};
QTEST_MAIN(WaveRendererTests)
#include "wave_renderer_tests.moc"
