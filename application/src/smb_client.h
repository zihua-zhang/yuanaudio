#pragma once
#include "path_repair.h"
#include <QUrl>
#include <memory>
#include <optional>

// All operations are synchronous and belong on one background worker. Paths
// are logical /share/... names, preserving source Unicode exactly. No secrets
// or raw server diagnostics are logged or persisted.
class SmbClient {
public:
    SmbClient();
    ~SmbClient();
    SmbClient(const SmbClient &) = delete;
    SmbClient &operator=(const SmbClient &) = delete;
    bool login(QString host, QString share, QString username, QString password, QString *error);
    QList<NasRepairFile> list(QString logicalPath, QString *error);
    std::optional<NasRepairFile> stat(QString logicalPath, QString *error);
    bool createHardLink(QString source, QString destination, QString *error);
    bool download(QString source, QString localTemp, QByteArray *sha256, QString *error);
    bool lastDownloadStorageFailure() const;
    bool sessionUnavailable() const;
    quint32 lastNtStatus() const;
    void setTimeoutMs(int milliseconds);
    static std::optional<QByteArray> relativePath(const QString &logicalPath,
        const QString &share, bool allowRoot, QString *error = nullptr);
    static std::optional<QUrl> endpoint(const QString &host, QString *error = nullptr);
private:
    friend class SmbClientTests;
    static bool readVersionMatches(const NasRepairFile &, const NasRepairFile &);
    struct Impl;
    std::unique_ptr<Impl> d_;
};
