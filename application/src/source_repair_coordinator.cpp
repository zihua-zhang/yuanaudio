#include "source_repair_coordinator.h"
#include "media_source.h"
#include "smb_client.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QtConcurrent>

namespace {
class NativeConnection final : public SourceRepairConnection {
    SmbClient client_;
public:
    bool login(const QString &host, const QString &share, const QString &account,
               const QString &password, QString *error) override {
        return client_.login(host, share, account, password, error);
    }
    bool storageFailure() const override { return client_.lastDownloadStorageFailure(); }
    bool sessionUnavailable() const override { return client_.sessionUnavailable(); }
    QList<NasRepairFile> list(const QString &path, QString *error) override { return client_.list(path, error); }
    std::optional<NasRepairFile> stat(const QString &path, QString *error) override { return client_.stat(path, error); }
    bool createHardLink(const QString &source, const QString &destination, QString *error) override {
        return client_.createHardLink(source, destination, error);
    }
    bool download(const QString &path, const QString &destination, QByteArray *sha, QString *error) override {
        client_.setTimeoutMs(180000);
        return client_.download(path, destination, sha, error);
    }
};
bool within(const QString &path, const QString &root) {
    const auto cleanRoot = QDir::cleanPath(root), cleanPath = QDir::cleanPath(path);
    return !root.isEmpty() && cleanRoot != "/" && QDir::isAbsolutePath(cleanRoot) &&
           (cleanPath == cleanRoot || cleanPath.startsWith(cleanRoot + '/'));
}
bool sameVersion(const Track &a, const Track &b) {
    return a.id == b.id && a.libraryId == b.libraryId && a.root == b.root &&
           a.relativePath == b.relativePath && a.size == b.size && a.modified == b.modified;
}
}
struct SourceRepairCoordinator::State {
    SourceRepairSession session;
    std::atomic_bool paused{false};
    std::atomic<quint64> epoch{0};
    // Accessed only by the serial worker pool; the UI never waits on a NAS lock.
    std::unique_ptr<SourceRepairConnection> connection;
    bool connected = false;
};
SourceRepairCoordinator::SourceRepairCoordinator(QString p,QString c,QObject *parent,Factory f)
    :QObject(parent),profile_(std::move(p)),catalog_(std::move(c)),factory_(std::move(f)),state_(std::make_shared<State>()) {
    pool_.setMaxThreadCount(1);
    if (!factory_) factory_ = [] { return std::make_unique<NativeConnection>(); };
}
SourceRepairCoordinator::~SourceRepairCoordinator() {
    state_->paused.store(true);
    ++state_->epoch;
    pool_.waitForDone();
}
void SourceRepairCoordinator::configure(SourceRepairSession session) {
    state_->paused.store(true);
    ++state_->epoch;
    ++epoch_;
    state_ = std::make_shared<State>();
    state_->session = std::move(session);
    state_->paused.store(paused_);
}
void SourceRepairCoordinator::setLibraries(const QList<Library> &libraries) {
    state_->session.options.libraries = libraries;
}
void SourceRepairCoordinator::setPaused(bool paused) {
    if (paused_ == paused) return;
    paused_ = paused;
    // A new epoch also cancels queued work when pause is followed quickly by resume.
    ++epoch_;
    ++state_->epoch;
    state_->paused.store(paused);
}
void SourceRepairCoordinator::repair(const Track &track, SourceRepairCompletion completion) {
    const auto generation = epoch_;
    const auto key = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(QJsonArray{qint64(generation), track.id, track.libraryId, track.root,
            track.relativePath, track.size, track.modified}).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
    if (pending_.contains(key)) { pending_[key].append(std::move(completion)); return; }
    pending_[key].append(std::move(completion));
    const auto state = state_;
    const auto ticket = state->epoch.load();
    auto session = state->session;
    const auto factory = factory_;
    const auto catalog = catalog_, profile = profile_;
    auto *watcher = new QFutureWatcher<SourceRepairReply>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, key, generation] {
        auto reply = watcher->result();
        watcher->deleteLater();
        if (generation != epoch_ || paused_) {
            reply = {}; reply.paused = true; reply.error = "路径处理已暂停，继续时将重试当前文件";
        }
        const auto callbacks = pending_.take(key);
        for (const auto &callback : callbacks) callback(reply);
    });
    watcher->setFuture(QtConcurrent::run(&pool_, [state, ticket, session, factory, catalog, profile, track]() mutable {
        SourceRepairReply reply;
        auto stopped = [&] {
            if (!state->paused.load() && state->epoch.load() == ticket) return false;
            reply.paused = true; reply.error = "路径处理已暂停"; return true;
        };
        if (stopped()) return reply;
        Index index(catalog);
        if (!index.open(&reply.error)) { reply.paused = true; return reply; }
        const auto current = index.trackByIdentity(track.libraryId, track.relativePath);
        if (!current || !sameVersion(track, *current)) {
            reply.error = "索引中的原件版本已改变，请定点刷新后重试"; return reply;
        }
        if (!current->compatiblePath.isEmpty() &&
            !verifiedMediaPath(*current, &reply.error, {}, QDir(profile).filePath("media-verification.sqlite")).isEmpty()) {
            reply.mapped = *current; return reply;
        }
        reply.error.clear();
        auto options = session.options;
        if (!within(track.root, options.mountRoot)) {
            const bool nasPath = track.root.startsWith("/Volumes/");
            reply.paused = reply.needsConnection = nasPath;
            reply.error = nasPath ? "请连接该素材库对应的 NAS 后继续自动处理" : "原件不可访问；该路径未配置 NAS 自动处理";
            return reply;
        }
        if (session.host.isEmpty() || session.share.isEmpty() || session.account.isEmpty() || session.password.isEmpty()) {
            reply.paused = reply.needsConnection = true;
            reply.error = "请连接 NAS，自动处理将优先建立硬链接，必要时缓存到本机"; return reply;
        }
        options.indexPath = catalog;
        options.libraries = index.libraries();
        options.preferHardLinks = true;
        options.serverRoot = "/" + session.share;
        QString endpointError;
        const auto endpoint = SmbClient::endpoint(session.host, &endpointError);
        if (!endpoint) { reply.error = endpointError; reply.paused = reply.needsConnection = true; return reply; }
        options.serverIdentity = endpoint->host().toLower() + ':' + QString::number(endpoint->port(445)) + '/' + session.share;
        QStringList protectedRoots;
        for (const auto &library : options.libraries) { protectedRoots << library.root; if (!library.sharedCache.isEmpty()) protectedRoots << library.sharedCache; }
        protectedRoots.sort();
        const QJsonArray identity{options.serverIdentity, options.mountRoot, options.serverRoot, options.outputRoot,
                                 options.localCacheRoot, catalog, QJsonArray::fromStringList(protectedRoots)};
        const auto digest = QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex().left(24);
        options.checkpointPath = QDir(profile).filePath("auto-path-repair-v2-" + QString::fromLatin1(digest) + ".json");
        if (stopped()) return reply;
        if (!state->connection) state->connection = factory();
        if (!state->connection) { reply.error = "无法建立 NAS 连接"; reply.paused = true; return reply; }
        if (!state->connected) {
            state->connected = state->connection->login(session.host, session.share, session.account, session.password, &reply.error);
            if (!state->connected) { reply.paused = reply.needsConnection = true; return reply; }
        }
        if (stopped()) return reply;
        const auto result = runPathRepair(options, {*current}, *state->connection, &state->paused, {});
        if (state->connection->sessionUnavailable()) state->connected = false;
        if (!result.repairedTracks.isEmpty()) reply.mapped = result.repairedTracks.first();
        reply.paused = result.paused || !result.error.isEmpty();
        reply.error = result.error.isEmpty() ? result.lastError : result.error;
        if (!reply.mapped.id && reply.error.isEmpty()) reply.error = "原件路径未能处理，已保留原记录";
        return reply;
    }));
}

