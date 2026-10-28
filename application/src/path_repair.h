#pragma once
#include "index.h"
#include <QJsonObject>
#include <atomic>
#include <functional>

struct NasRepairFile {
    QString path, name;
    qint64 size = 0, modified = 0; // milliseconds, including NAS seconds conversion
    bool directory = false;
};
class PathRepairBackend {
public:
    virtual ~PathRepairBackend() = default;
    virtual bool storageFailure() const { return false; }
    virtual QList<NasRepairFile> list(const QString &path, QString *error) = 0;
    // Creates a new destination only. SHA-256 is returned as raw bytes or hex.
    virtual bool download(const QString &path, const QString &destination,
                          QByteArray *sha256, QString *error) = 0;
};
struct PathRepairOptions {
    QString mountRoot, serverRoot, outputRoot, checkpointPath, indexPath;
    QList<Library> libraries;
};
struct PathRepairResult {
    int succeeded = 0, failed = 0, done = 0, total = 0;
    bool paused = false;
    QString error, lastError;
    QList<Track> repairedTracks;
};
QList<Track> pathRepairCandidates(const QString &indexPath, const QString &failurePath,
                                 const QString &library, QString *error);
PathRepairResult runPathRepair(const PathRepairOptions &, const QList<Track> &,
                              PathRepairBackend &, std::atomic_bool *pause,
                              const std::function<void(const QJsonObject &)> &progress);
