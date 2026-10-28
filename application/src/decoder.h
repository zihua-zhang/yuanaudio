#pragma once
#include "cache.h"
#include <QJsonObject>
#include <functional>

// Runs in the disposable wave worker, never on the GUI thread.
bool decodeAudioPeaks(const QString &path, WaveData *result, QString *error, int *damagedPackets,
                      const std::function<void(const WaveData &)> &preview = {},
                      qint64 readBytesPerSecond = 0, const std::function<void(qint64)> &durationReady = {},
                      const std::function<void(qint64)> &readProgress = {}, QString *failureKind = nullptr,
                      QJsonObject *diagnostics = nullptr);
