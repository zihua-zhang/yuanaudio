#include "workers.h"
#include "cache.h"
#include "decoder.h"
#include "index.h"
#include "media_source.h"
#include <QAudioOutput>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaPlayer>
#include <QRandomGenerator>
#include <QStorageInfo>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <memory>
#include <thread>

static void output(const QJsonObject &message) {
    QFile out;
    if (out.open(stdout, QIODevice::WriteOnly)) {
        out.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + "\n");
        out.flush();
    }
}
static int failure(const QString &message, const QString &kind = {}) {
    output({{"event", "error"}, {"message", message}, {"kind", kind}});
    return 1;
}
static QJsonObject inputConfig() {
    QFile in;
    in.open(stdin, QIODevice::ReadOnly);
    return QJsonDocument::fromJson(in.readLine(1024 * 1024)).object();
}
static int registerLibrary(const QJsonObject &config) {
    QFileInfo info(config["root"].toString());
    if (!info.isDir() || !info.isReadable())
        return failure("音乐目录不可读，请确认共享已挂载。");
    const auto root = QDir::cleanPath(info.absoluteFilePath());
    QString cache, sharedId, error;
    if (!config["cache"].toString().isEmpty() &&
        !prepareSharedCache(root, config["cache"].toString(), &cache, &sharedId, &error))
        return failure(error);
    const auto id = sharedId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : sharedId;
    output({{"event", "registered"},
            {"id", id},
            {"root", root},
            {"name", info.fileName().isEmpty() ? root : info.fileName()},
            {"cache", cache},
            {"sharedId", sharedId}});
    return 0;
}
static int attachCache(const QJsonObject &config) {
    Index db(config["database"].toString());
    QString error;
    if (!db.open(&error))
        return failure(error);
    const auto libraryId = config["library"].toString();
    std::optional<Library> library;
    for (const auto &candidate : db.libraries())
        if (candidate.id == libraryId)
            library = candidate;
    if (!library)
        return failure("素材库不存在，现有索引与个人资料保持不变。");
    QString cache, sharedId;
    if (!prepareSharedCache(library->root, config["cache"].toString(), &cache, &sharedId, &error) ||
        !db.attachSharedCache(libraryId, cache, sharedId, &error))
        return failure(error);
    output({{"event", "cache-attached"}, {"id", libraryId}, {"cache", cache}, {"sharedId", sharedId}});
    return 0;
}
static int scanLibrary(const QJsonObject &config) {
    const auto root = config["root"].toString(), library = config["library"].toString();
    if (!QFileInfo(root).isDir() || !QFileInfo(root).isReadable())
        return failure("共享目录不可访问，原索引保留。");
    Index db(config["database"].toString());
    QString error;
    if (!db.open(&error))
        return failure(error);
    bool exists = false;
    for (const auto &l : db.libraries())
        if (l.id == library)
            exists = true;
    if (!exists && !db.saveLibrary({library, QFileInfo(root).fileName(), root, ""}, false))
        return failure(db.lastError());
    const auto catalogExcluded = db.exclusions(library, &error);
    if (!catalogExcluded)
        return failure(error);
    const QSet<QString> extensions = {"wav", "wave", "mp3", "flac", "aif", "aiff",
                                      "m4a", "aac",  "ogg", "opus", "wma"};
    QStringList excluded;
    auto directoryIdentity = [](const QString &path) {
        const QFileInfo info(path);
        const auto canonical = info.canonicalFilePath();
        return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
    };
    for (const auto &v : config["excluded"].toArray())
        excluded << directoryIdentity(v.toString());
    // Also load exclusions from the library configuration, including clients that invoke
    // the worker directly. Dedicated cache subdirectories must never be traversed as audio.
    for (const auto &lib : db.libraries())
        if (!lib.sharedCache.isEmpty())
            excluded << directoryIdentity(lib.sharedCache);
    auto isExcluded = [&](const QString &path) {
        if (catalogExcluded->excludesFolder(QDir(root).relativeFilePath(path)))
            return true;
#ifdef Q_OS_WIN
        constexpr auto comparison = Qt::CaseInsensitive;
#else
        constexpr auto comparison = Qt::CaseSensitive;
#endif
        const auto normalized = excluded.isEmpty() ? path : directoryIdentity(path);
        for (const auto &exclude : excluded)
            if (normalized.compare(exclude, comparison) == 0 ||
                normalized.startsWith(exclude + '/', comparison))
                return true;
        return false;
    };
    const auto controlPath = config["control"].toString();
    QElapsedTimer controlClock, paceClock;
    controlClock.start();
    paceClock.start();
    bool paused = false, reportedPause = false;
    int entriesPerSecond = config["entriesPerSecond"].toInt(0);
    qint64 inspected = 0;
    auto permitNext = [&] {
        do {
            if (!controlPath.isEmpty() && (controlClock.elapsed() >= 200 || inspected == 0)) {
                QFile control(controlPath);
                if (control.open(QIODevice::ReadOnly) && control.size() < 4096) {
                    const auto policy = QJsonDocument::fromJson(control.readAll());
                    if (policy.isObject()) {
                        paused = policy.object()["paused"].toBool();
                        entriesPerSecond = qBound(0, policy.object()["entriesPerSecond"].toInt(400), 2000);
                    }
                }
                controlClock.restart();
            }
            if (paused != reportedPause) {
                reportedPause = paused;
                output({{"event", "deferred"}, {"paused", paused}});
                paceClock.restart();
                inspected = 0;
            }
            if (paused)
                QThread::msleep(100);
        } while (paused);
        if (entriesPerSecond > 0) {
            const auto due = inspected * 1000 / entriesPerSecond - paceClock.elapsed();
            if (due > 0)
                QThread::msleep(static_cast<unsigned long>(qMin(qint64(100), due)));
        }
        ++inspected;
    };
    QList<Track> batch;
    QStringList directories;
    qint64 count = 0;
    bool filesChanged = false;
    QElapsedTimer sinceReport;
    sinceReport.start();
    auto commit = [&] {
        if (batch.isEmpty() && directories.isEmpty())
            return true;
        if (!directories.isEmpty() && !db.upsertFolders(library, directories, &error))
            return false;
        bool batchChanged = false;
        if (!db.upsert(batch, &error, &batchChanged))
            return false;
        filesChanged |= batchChanged;
        output({{"event", "progress"},
                {"count", count},
                {"filesChanged", filesChanged},
                {"folder", batch.isEmpty() ? directories.last() : batch.last().folder}});
        batch.clear();
        directories.clear();
        sinceReport.restart();
        return true;
    };
    QStringList pendingDirectories{root};
    while (!pendingDirectories.isEmpty()) {
        permitNext();
        const auto directory = pendingDirectories.takeLast();
        if (isExcluded(directory))
            continue;
        QDirIterator it(directory, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        while (true) {
            permitNext();
            if (!it.hasNext())
                break;
            it.next();
            const auto info = it.fileInfo();
            if (info.isDir()) {
                if (isExcluded(info.absoluteFilePath()))
                    continue;
                pendingDirectories << info.absoluteFilePath();
                directories << QDir(root).relativeFilePath(info.absoluteFilePath());
                if (directories.size() >= 200 || sinceReport.elapsed() > 750)
                    if (!commit())
                        return failure(error);
                continue;
            }
            if (!extensions.contains(info.suffix().toLower()))
                continue;
            const auto relative = QDir(root).relativeFilePath(info.absoluteFilePath());
            if (catalogExcluded->excludesTrack(relative))
                continue;
            Track t;
            t.libraryId = library;
            t.relativePath = relative;
            t.name = info.fileName();
            t.folder = QFileInfo(t.relativePath).path();
            t.size = info.size();
            t.modified = info.lastModified().toMSecsSinceEpoch();
            batch << t;
            ++count;
            if (batch.size() >= 200 || sinceReport.elapsed() > 750) {
                if (!commit())
                    return failure(error);
            }
        }
    }
    if (!commit())
        return failure(error);
    output({{"event", "complete"}, {"count", count}});
    return 0;
}
static QByteArray fingerprint(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto bytes = file.size();
    const auto head = file.read(qMin(qint64(65536), bytes));
    if (head.size() != qMin(qint64(65536), bytes))
        return {};
    hash.addData(head);
    if (file.size() > 65536) {
        if (!file.seek(qMax(qint64(65536), file.size() - 65536)))
            return {};
        const auto tail = file.read(65536);
        if (tail.size() != qMin(qint64(65536), bytes - 65536))
            return {};
        hash.addData(tail);
    }
    return hash.result().toHex();
}
static bool hasCompatibleSource(const QJsonObject &config) {
    return !config["compatibleSha256"].toString().isEmpty() ||
           !config["compatiblePath"].toString().isEmpty();
}
static Track compatibleTrack(const QJsonObject &config) {
    Track track;
    track.compatiblePath = config["path"].toString();
    track.compatibleSha256 = config["compatibleSha256"].toString().toLatin1();
    track.compatibleSize = config["compatibleSize"].toInteger(-1);
    track.compatibleModified = config["compatibleModified"].toInteger(-1);
    return track;
}
static int buildWave(const QJsonObject &config) {
    const auto path = config["path"].toString(), local = config["local"].toString();
    const bool compatible = hasCompatibleSource(config);
    const auto media = compatibleTrack(config);
    const auto verificationProgress = [](qint64 bytes) {
        output({{"event", "reading"}, {"phase", "verification"}, {"bytes", bytes}});
    };
    QString mediaError;
    // This guard precedes every cache/publication fast path, including publish_existing.
    // The copy's mtime is independent of the indexed original version.
    if (compatible && (path.isEmpty() || !config.contains("indexedSize") ||
                       !config.contains("indexedModified") ||
                       verifiedMediaPath(media, &mediaError, verificationProgress).isEmpty()))
        return failure(mediaError.isEmpty() ? QString("兼容文件缺少原曲索引版本，无法核验。") : mediaError,
                       "compatible_unavailable");
    // Compatible views must not be published under the identity of an original on
    // another machine. Their provenance is local until separately verified there.
    const auto shared = compatible ? QString() : config["shared"].toString();
    const auto sharedLibrary = config["sharedLibrary"].toString().isEmpty()
                                   ? config["library"].toString()
                                   : config["sharedLibrary"].toString();
    if (!compatible && config["operation"].toString() == "publish_existing") {
        if (shared.isEmpty() || !config.contains("indexedSize") || !config.contains("indexedModified"))
            return failure("尚未配置共享缓存或缺少索引版本信息。", "shared_cache_write");
        const auto size = config["indexedSize"].toInteger(), stamp = config["indexedModified"].toInteger();
        const auto version =
            waveKey(config["library"].toString(), config["relative"].toString(), size, stamp);
        WaveData existing;
        bool approximate = false;
        QJsonObject existingDiagnostics;
        if (!loadWaveView(local, version, &existing, nullptr, &approximate, &existingDiagnostics))
            return failure("本机已有波形暂不可读取，可稍后重试同步。", "local_cache_missing");
        QString error;
        if (!approximate && !publishPortableWave(shared, sharedLibrary, config["relative"].toString(), size,
                                                 stamp, existing, false, &error))
            return failure("共享波形同步未完成：" + error, "shared_cache_write");
        output({{"event", "wave"},
                {"complete", true},
                {"approximate", approximate},
                {"stored", true},
                {"published", !approximate},
                {"failureKind", approximate ? existingDiagnostics["stopKind"].toString(
                                    existingDiagnostics.isEmpty() ? "reference" : "decode_warning") : QString()},
                {"source", approximate ? existingDiagnostics["message"].toString("参考波形仅保留本机")
                                        : "共享波形已同步"},
                {"data", QString::fromLatin1(encodeWave(existing).toBase64())}});
        return 0;
    }
    // Probe the exact indexed spelling. Do not rename or normalize NAS entries;
    // a successful directory enumeration is not evidence that the file can open.
    auto accessFailure = [&] {
        QFile sourceProbe(path);
        QString detail;
        if (!sourceProbe.open(QIODevice::ReadOnly))
            detail = sourceProbe.errorString();
        else if (sourceProbe.read(1).isEmpty() && sourceProbe.error() != QFileDevice::NoError)
            detail = sourceProbe.errorString();
        else
            detail = "未能取得可用的文件属性或内容指纹";
        return failure("路径访问失败（尚未解码）：" + detail + " · " + path, "source_unavailable");
    };
    QFileInfo before(path);
    if (!before.isFile())
        return accessFailure();
    const auto stamp = before.lastModified().toMSecsSinceEpoch(), size = before.size();
    const auto signature = compatible ? media.compatibleSha256.toLower() : fingerprint(path);
    if (signature.isEmpty())
        return accessFailure();
    const auto key =
        waveKey(config["library"].toString(),
                config["relative"].toString() + "|" + QString::fromLatin1(signature), size, stamp);
    const auto sharedKey = waveKey(
        sharedLibrary, config["relative"].toString() + "|" + QString::fromLatin1(signature), size, stamp);
    const auto versionSize = compatible ? config["indexedSize"].toInteger() : size;
    const auto versionModified = compatible ? config["indexedModified"].toInteger() : stamp;
    const auto versionKey = waveKey(config["library"].toString(), config["relative"].toString(),
                                    versionSize, versionModified);
    const bool matchesIndex =
        compatible || ((!config.contains("indexedSize") || config["indexedSize"].toInteger() == size) &&
                       (!config.contains("indexedModified") || config["indexedModified"].toInteger() == stamp));
    WaveData cached;
    bool stored = false;
    QJsonObject diagnostics;
    auto sourceStable = [&] {
        if (compatible)
            return !verifiedMediaPath(media, nullptr, verificationProgress).isEmpty();
        const QFileInfo current(path);
        return current.isFile() && current.size() == size &&
               current.lastModified().toMSecsSinceEpoch() == stamp && fingerprint(path) == signature;
    };
    auto deliver = [&](const WaveData &data, const QString &source, bool complete, bool approximate = false) {
        output({{"event", "wave"},
                {"source", source},
                {"complete", complete},
                {"approximate", approximate},
                {"diagnostics", diagnostics},
                {"stored", stored && matchesIndex},
                {"failureKind", !matchesIndex         ? "source_changed"
                                : complete && !stored ? "cache_write"
                                : approximate         ? diagnostics["stopKind"].toString("decode_warning")
                                                      : QString()},
                {"data", QString::fromLatin1(encodeWave(data).toBase64())}});
    };
    if (!matchesIndex)
        output({{"event", "warning"},
                {"message", "原文件版本已变化；请刷新素材库后重新准备，当前项暂不计入全库缓存成功数。"}});
    if (loadWave(local, key, &cached)) {
        if (!sourceStable())
            return failure("音乐在读取过程中发生变化，请刷新后重试。", "source_changed");
        stored = saveWaveView(local, versionKey, cached);
        // Fingerprinted complete objects are accurate; metadata-only/approximate views are never shared.
        WaveData alreadyShared;
        const bool sharedFailed = !shared.isEmpty() && !loadWave(shared, sharedKey, &alreadyShared) &&
                                  !publishWave(shared, sharedKey, cached);
        deliver(cached, "本机波形缓存", true);
        if (sharedFailed)
            return failure("共享缓存未能保存，本机波形仍可使用；可稍后重试同步。", "shared_cache_write");
        return 0;
    }
    auto useShared = [&]() -> std::optional<int> {
        if (shared.isEmpty())
            return std::nullopt;
        const bool fingerprinted = loadWave(shared, sharedKey, &cached);
        if (!fingerprinted &&
            (!matchesIndex ||
             !loadPortableWave(shared, sharedLibrary, config["relative"].toString(), size, stamp, &cached)))
            return std::nullopt;
        if (!sourceStable())
            return failure("音乐在等待过程中发生变化，请刷新后重试。", "source_changed");
        stored = saveWaveView(local, versionKey, cached);
        if (fingerprinted)
            publishWave(local, key, cached);
        deliver(cached, fingerprinted ? "共享波形缓存" : "共享波形直读缓存", true);
        return 0;
    };
    std::unique_ptr<SharedWaveLease> lease;
    QElapsedTimer renewClock;
    if (!shared.isEmpty()) {
        lease = std::make_unique<SharedWaveLease>(shared, sharedKey);
        QElapsedTimer wait, reported;
        wait.start();
        const bool background = config["background"].toBool(config["readBytesPerSecond"].toInteger() > 0);
        const int waitLimit = background ? 60000 : 2500;
        while (true) {
            if (const auto result = useShared())
                return *result;
            const auto state = lease->acquireOrObserve();
            if (state == SharedWaveLease::State::Owned) {
                // A prior owner can publish and release between our cache read and claim.
                if (const auto result = useShared())
                    return *result;
                renewClock.start();
                break;
            }
            if (state == SharedWaveLease::State::Unavailable || state == SharedWaveLease::State::Expired ||
                wait.elapsed() >= waitLimit) {
                // The peer may publish and release between our failed claim and lease observation.
                if (const auto result = useShared())
                    return *result;
                output({{"event", "warning"}, {"message", "共享波形暂未就绪，将由本机继续准备。"}});
                break;
            }
            if (!reported.isValid() || reported.elapsed() >= 500) {
                output({{"event", "waiting"},
                        {"kind", "shared_wave"},
                        {"elapsedMs", wait.elapsed()},
                        {"limitMs", waitLimit},
                        {"message", "其他电脑正在准备这首波形"}});
                reported.restart();
            }
            QThread::msleep(
                static_cast<unsigned long>(qBound(qint64(1), waitLimit - wait.elapsed(), qint64(250))));
        }
    }
    auto keepLease = [&] {
        if (lease && lease->owned() && renewClock.elapsed() >= 750) {
            lease->renew();
            renewClock.restart();
        }
    };
    WaveData wave;
    QString error;
    QDir().mkpath(local);
    const QStorageInfo disk(local);
    if (disk.isValid() && disk.bytesAvailable() >= 0 && disk.bytesAvailable() < 2LL * 1024 * 1024 * 1024)
        return failure("本机缓存盘剩余空间不足 2 GiB，已停止新增缓存。", "storage_full");
    int damagedPackets = 0;
    QString failureKind;
    const bool decoded = decodeAudioPeaks(
            path, &wave, &error, &damagedPackets,
            [&](const WaveData &partial) {
                keepLease();
                deliver(partial, "正在生成波形", false);
            },
            config["readBytesPerSecond"].toInteger(),
            [](qint64 duration) { output({{"event", "duration"}, {"milliseconds", duration}}); },
            [&](qint64 bytes) {
                keepLease();
                output({{"event", "reading"}, {"bytes", bytes}});
            },
            &failureKind, &diagnostics);
    output({{"event", "decode-diagnostics"}, {"diagnostics", diagnostics}});
    if (!decoded)
        return failure(error, failureKind);
    QFileInfo after(path);
    const bool interruptedRead = diagnostics["stopKind"].toString() == "source_unavailable";
    // A disconnected source cannot be re-fingerprinted. Preserve its already decoded
    // samples as a labelled local reference, but never publish them as verified audio.
    if ((compatible && !sourceStable()) ||
        (after.isFile() && (after.size() != size || after.lastModified().toMSecsSinceEpoch() != stamp)) ||
        (!compatible && !interruptedRead && fingerprint(path) != signature))
        return failure("音乐在生成过程中发生变化，请重新选择。", "source_changed");
    if (damagedPackets) {
        const auto seconds = QString::number(diagnostics["decodedMs"].toInteger() / 1000.0, 'f', 2);
        QString message = diagnostics["stoppedEarly"].toBool()
            ? QString("部分参考波形 · 已保留 %1 秒；%2").arg(seconds, diagnostics["stopReason"].toString())
            : QString("已解出可用音频（有异常） · %1 秒 · 已读至文件结尾；波形位置仍供参考").arg(seconds);
        diagnostics["message"] = message;
        stored = saveWaveView(local, versionKey, wave, true, diagnostics);
        deliver(wave, message, true, true);
        output({{"event", "warning"}, {"message", message + "。未完成整曲听审，不发布为共享完整波形。"}});
        if (diagnostics["stoppedEarly"].toBool())
            return failure(message, failureKind.isEmpty() ? QString("invalid_audio") : failureKind);
        return 0;
    }
    if (!publishWave(local, key, wave, &error))
        output({{"event", "warning"}, {"message", "本机缓存保存失败：" + error}});
    stored = saveWaveView(local, versionKey, wave);
    if (!stored)
        output({{"event", "warning"}, {"message", "本机直读缓存未能保存；全库准备会记为失败，可稍后重试。"}});
    deliver(wave, "波形已生成", true);
    if (!shared.isEmpty() && !loadWave(shared, sharedKey, &cached) &&
        !publishWave(shared, sharedKey, wave, &error))
        return failure("共享缓存未能保存，本机波形仍可使用；可稍后重试同步。", "shared_cache_write");
    return 0;
}

static int runPlayer(QCoreApplication &app) {
    QAudioOutput audio;
    audio.setVolume(0.65f);
    std::unique_ptr<QMediaPlayer> player;
    QJsonObject currentLoad;
    bool mediaFailed = false;
    double pendingRatio = -1;
    bool playWhenReady = false;
    qint64 generation = 0;
    auto applyCue = [&] {
        if (!player || pendingRatio < 0 || player->duration() <= 0 || !player->isSeekable())
            return;
        const auto status = player->mediaStatus();
        if (status == QMediaPlayer::LoadingMedia || status == QMediaPlayer::NoMedia ||
            status == QMediaPlayer::InvalidMedia)
            return;
        const auto position =
            qBound(qint64(0), qint64(pendingRatio * player->duration()), player->duration() - 1);
        pendingRatio = -1; // clear before setPosition: Qt may emit signals synchronously
        player->setPosition(position);
        if (playWhenReady)
            player->play();
    };
    QTimer timer;
    timer.setInterval(100);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        output({{"event", "position"},
                {"generation", generation},
                {"position", player ? player->position() : 0},
                {"duration", player ? player->duration() : 0}});
    });
    std::function<void(const QJsonObject &)> handle;
    handle = [&](const QJsonObject &m) {
        const auto op = m["op"].toString();
        if (op == "load") {
            timer.stop();
            player.reset();
            currentLoad = m;
            mediaFailed = false;
            generation = m["generation"].toInteger();
            pendingRatio = m.contains("startRatio") ? qBound(0.0, m["startRatio"].toDouble(), 1.0) : -1;
            playWhenReady = m["autoplay"].toBool();
            player = std::make_unique<QMediaPlayer>();
            player->setAudioOutput(&audio);
            if (hasCompatibleSource(m)) {
                QString error;
                const auto verificationProgress = [current = generation](qint64 bytes) {
                    output({{"event", "verifying"}, {"generation", current}, {"bytes", bytes}});
                };
                if (m["path"].toString().isEmpty() ||
                    verifiedMediaPath(compatibleTrack(m), &error, verificationProgress).isEmpty()) {
                    mediaFailed = true;
                    output({{"event", "error"}, {"generation", generation},
                            {"kind", "compatible_unavailable"},
                            {"message", error.isEmpty() ? QString("兼容文件路径无效，请重新修复。") : error}});
                    return;
                }
            }
            QObject::connect(
                player.get(), &QMediaPlayer::playbackStateChanged, &app,
                [current = generation, instance = player.get(), &timer](QMediaPlayer::PlaybackState s) {
                    if (s == QMediaPlayer::PlayingState)
                        timer.start();
                    else
                        timer.stop();
                    output({{"event", "state"},
                            {"generation", current},
                            {"position", instance->position()},
                            {"playing", s == QMediaPlayer::PlayingState}});
                });
            auto reportPausedPosition = [current = generation, instance = player.get()] {
                // Paused seek/load changes report once; idle playback has no periodic wakeups.
                if (instance->playbackState() != QMediaPlayer::PlayingState)
                    output({{"event", "position"},
                            {"generation", current},
                            {"position", instance->position()},
                            {"duration", instance->duration()}});
            };
            QObject::connect(player.get(), &QMediaPlayer::positionChanged, &app, reportPausedPosition);
            QObject::connect(player.get(), &QMediaPlayer::durationChanged, &app, reportPausedPosition);
            auto ready = [&, current = generation] {
                if (current == generation)
                    applyCue();
            };
            QObject::connect(player.get(), &QMediaPlayer::durationChanged, &app, ready);
            QObject::connect(player.get(), &QMediaPlayer::seekableChanged, &app, ready);
            QObject::connect(player.get(), &QMediaPlayer::mediaStatusChanged, &app, ready);
            QObject::connect(player.get(), &QMediaPlayer::mediaStatusChanged, &app,
                             [current = generation](QMediaPlayer::MediaStatus status) {
                                 if (status == QMediaPlayer::LoadedMedia ||
                                     status == QMediaPlayer::BufferedMedia)
                                     output({{"event", "ready"}, {"generation", current}});
                             });
            QObject::connect(
                player.get(), &QMediaPlayer::errorOccurred, &app,
                [current = generation, &mediaFailed, &timer](QMediaPlayer::Error, const QString &error) {
                    timer.stop();
                    mediaFailed = true;
                    output({{"event", "error"}, {"generation", current}, {"message", error}});
                });
            audio.setVolume(float(m["volume"].toDouble(0.65)));
            player->setSource(QUrl::fromLocalFile(m["path"].toString()));
            if (playWhenReady && pendingRatio < 0)
                player->play();
        } else if (player && m["generation"].toInteger() == generation) {
            if ((op == "play" || op == "cue") && mediaFailed) {
                auto retry = currentLoad;
                retry["autoplay"] = true;
                retry["volume"] = audio.volume();
                if (op == "cue")
                    retry["startRatio"] = m["ratio"];
                handle(retry);
            } else if (op == "cue") {
                pendingRatio = qBound(0.0, m["ratio"].toDouble(), 1.0);
                playWhenReady = true;
                applyCue();
            } else if (op == "play") {
                playWhenReady = true;
                if (pendingRatio >= 0)
                    applyCue();
                else
                    player->play();
            } else if (op == "pause") {
                playWhenReady = false;
                player->pause();
            } else if (op == "seek")
                player->setPosition(m["position"].toInteger());
            else if (op == "volume")
                audio.setVolume(float(m["value"].toDouble()));
        }
    };
    std::thread reader([&] {
        QFile in;
        in.open(stdin, QIODevice::ReadOnly);
        while (true) {
            const auto line = in.readLine(65536);
            if (line.isEmpty())
                break;
            const auto m = QJsonDocument::fromJson(line).object();
            QMetaObject::invokeMethod(&app, [handle, m] { handle(m); }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
    });
    const int result = app.exec();
    reader.join();
    return result;
}
int runWorker(const QString &kind, QCoreApplication &app) {
    if (kind == "player")
        return runPlayer(app);
    if (kind == "waves") {
        QThread::currentThread()->setPriority(QThread::LowPriority);
        QFile input;
        input.open(stdin, QIODevice::ReadOnly);
        while (true) {
            const auto line = input.readLine(1024 * 1024);
            if (line.isEmpty())
                return 0;
            const auto config = QJsonDocument::fromJson(line).object();
            const int result = buildWave(config);
            output({{"event", "job-finished"}, {"success", result == 0}});
        }
    }
    const auto config = inputConfig();
    if (kind == "register")
        return registerLibrary(config);
    if (kind == "attach-cache")
        return attachCache(config);
    if (kind == "scan") {
        QThread::currentThread()->setPriority(QThread::LowPriority);
        return scanLibrary(config);
    }
    if (kind == "wave")
        return buildWave(config);
    return failure("未知后台任务");
}
