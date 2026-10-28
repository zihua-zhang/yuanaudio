#pragma once
#include "cache.h"
#include <QCache>
#include <QPainter>
#include <QPixmap>

// GUI-thread raster cache. Playback only changes the clip and cursor, never the envelope.
class WaveRenderer {
  public:
    static constexpr int DefaultBudget = 16 * 1024 * 1024;
    struct Statistics {
        quint64 hits = 0, builds = 0, bypasses = 0;
        int bytes = 0, entries = 0;
    };
    explicit WaveRenderer(int budgetBytes = DefaultBudget);
    void paint(QPainter *painter, const QRect &rect, const QString &source, const WaveData &wave,
               double fraction, bool approximate, int playX);
    void invalidate(const QString &source);
    void clear() { cache_.clear(); }
    Statistics statistics() const;

  private:
    struct Raster {
        QPixmap base, played;
    };
    QCache<QString, Raster> cache_;
    quint64 hits_ = 0, builds_ = 0, bypasses_ = 0;
};
