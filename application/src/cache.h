#pragma once
#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QJsonObject>
#include <QVector>

struct Peak {
    float minimum = 0, maximum = 0;
};
struct WaveData {
    int sampleRate = 0, channels = 0;
    qint64 frames = 0, stride = 256;
    QVector<QVector<Peak>> buckets;
};
class PeakBuilder {
  public:
    PeakBuilder(int sampleRate, int channels);
    void append(const float *interleaved, qint64 frameCount);
    WaveData finish();

  private:
    WaveData data_;
    QVector<Peak> pending_;
    qint64 pendingFrames_ = 0;
    void flush();
};
QByteArray encodeWave(const WaveData &wave);
bool decodeWave(const QByteArray &bytes, WaveData *wave);
QString waveKey(const QString &libraryId, const QString &relativePath, qint64 size, qint64 modified);
bool publishWave(const QString &cacheRoot, const QString &key, const WaveData &wave,
                 QString *error = nullptr);
bool loadWave(const QString &cacheRoot, const QString &key, WaveData *wave);
// Local display copies keyed by indexed source version. Reading these never touches the source.
bool saveWaveView(const QString &cacheRoot, const QString &versionKey, const WaveData &wave,
                  bool approximate = false, const QJsonObject &diagnostics = {});
bool loadWaveView(const QString &cacheRoot, const QString &versionKey, WaveData *wave,
                  qint64 *verified = nullptr, bool *approximate = nullptr, QJsonObject *diagnostics = nullptr);
bool invalidateWaveViews(const QString &cacheRoot);
QString ensureLibraryIdentity(const QString &cacheRoot, QString *error = nullptr);
// Validates a dedicated cache (a child of the music root is allowed and must be excluded by scanning).
bool prepareSharedCache(const QString &musicRoot, const QString &cacheRoot, QString *canonicalCache,
                        QString *sharedId, QString *error = nullptr);
// Portable display views preserve indexed identity; they are never fingerprinted full artifacts.
bool publishPortableWave(const QString &root, const QString &sharedId, const QString &relative, qint64 size,
                         qint64 modified, const WaveData &wave, bool approximate, QString *error = nullptr);
bool loadPortableWave(const QString &root, const QString &sharedId, const QString &relative, qint64 size,
                      qint64 modified, WaveData *wave);

class SharedWaveLease {
  public:
    enum class State { Owned, Waiting, Expired, Unavailable };
    SharedWaveLease(const QString &root, const QString &key, int staleAfterMs = 10000);
    ~SharedWaveLease();
    SharedWaveLease(const SharedWaveLease &) = delete;
    SharedWaveLease &operator=(const SharedWaveLease &) = delete;
    State acquireOrObserve();
    bool renew();
    bool owned() const {
        return owned_;
    }

  private:
    QString path_, key_, token_;
    QByteArray observed_;
    QElapsedTimer unchanged_, heartbeat_;
    int staleAfterMs_;
    qint64 sequence_ = 0;
    bool owned_ = false;
    QByteArray payload() const;
};
