#pragma once
#include "path_repair.h"
#include "source_repair.h"
#include <QObject>
#include <QThreadPool>
#include <QHash>
#include <memory>

class SourceRepairConnection : public PathRepairBackend {
public:
    virtual bool login(const QString &host, const QString &share, const QString &account,
                       const QString &password, QString *error) = 0;
};
struct SourceRepairSession {
    PathRepairOptions options;
    QString host, share, account, password;
};
// Parent-process coordinator. Credentials never enter decoder/player configs.
class SourceRepairCoordinator : public QObject {
public:
    using Factory = std::function<std::unique_ptr<SourceRepairConnection>()>;
    SourceRepairCoordinator(QString profile, QString catalog, QObject *parent = nullptr, Factory factory = {});
    ~SourceRepairCoordinator() override;
    void configure(SourceRepairSession session);
    void setLibraries(const QList<Library> &libraries);
    void setPaused(bool paused);
    void repair(const Track &track, SourceRepairCompletion completion);
private:
    struct State;
    QString profile_, catalog_;
    Factory factory_;
    QThreadPool pool_;
    std::shared_ptr<State> state_;
    QHash<QString, QList<SourceRepairCompletion>> pending_;
    bool paused_ = false;
    quint64 epoch_ = 0;
};
