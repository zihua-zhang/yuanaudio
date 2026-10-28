#include "cache.h"
#include "file_publication.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QThread>
#include <QUuid>
#include <cmath>

PeakBuilder::PeakBuilder(int rate, int channels) {
    data_.sampleRate = rate;
    data_.channels = qBound(1, channels, 32);
    pending_.fill({1, -1}, data_.channels);
}
void PeakBuilder::append(const float *samples, qint64 count) {
    for (qint64 f = 0; f < count; ++f) {
        for (int c = 0; c < data_.channels; ++c) {
            float v = samples[f * data_.channels + c];
            if (!std::isfinite(v))
                v = 0;
            v = qBound(-1.0f, v, 1.0f);
            pending_[c].minimum = qMin(pending_[c].minimum, v);
            pending_[c].maximum = qMax(pending_[c].maximum, v);
        }
        ++data_.frames;
        if (++pendingFrames_ >= data_.stride)
            flush();
    }
}
void PeakBuilder::flush() {
    if (!pendingFrames_)
        return;
    data_.buckets << pending_;
    pending_.fill({1, -1});
    pendingFrames_ = 0;
    if (data_.buckets.size() >= 2048) {
        QVector<QVector<Peak>> reduced;
        reduced.reserve(1024);
        for (int i = 0; i < data_.buckets.size(); i += 2) {
            auto merged = data_.buckets[i];
            for (int c = 0; c < data_.channels; ++c) {
                merged[c].minimum = qMin(merged[c].minimum, data_.buckets[i + 1][c].minimum);
                merged[c].maximum = qMax(merged[c].maximum, data_.buckets[i + 1][c].maximum);
            }
            reduced << merged;
        }
        data_.buckets = std::move(reduced);
        data_.stride *= 2;
    }
}
WaveData PeakBuilder::finish() {
    auto result = data_;
    if (pendingFrames_)
        result.buckets << pending_;
    return result;
}
QByteArray encodeWave(const WaveData &wave) {
    if (wave.buckets.isEmpty() || wave.channels < 1 || wave.channels > 32)
        return {};
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out << quint32(0x31564C41) << quint32(1) << qint32(wave.sampleRate) << qint32(wave.channels)
        << wave.frames << wave.stride << qint32(wave.buckets.size());
    for (const auto &bucket : wave.buckets) {
        if (bucket.size() != wave.channels)
            return {};
        for (const auto &p : bucket)
            out << qint16(std::lround(qBound(-1.0f, p.minimum, 1.0f) * 32767))
                << qint16(std::lround(qBound(-1.0f, p.maximum, 1.0f) * 32767));
    }
    return data + QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}
bool decodeWave(const QByteArray &bytes, WaveData *wave) {
    if (bytes.size() < 72 || bytes.size() > 300000)
        return false;
    const auto payload = bytes.first(bytes.size() - 32);
    if (QCryptographicHash::hash(payload, QCryptographicHash::Sha256) != bytes.last(32))
        return false;
    QDataStream in(payload);
    in.setByteOrder(QDataStream::LittleEndian);
    quint32 magic, version;
    qint32 rate, channels, count;
    WaveData decoded;
    in >> magic >> version >> rate >> channels >> decoded.frames >> decoded.stride >> count;
    if (magic != 0x31564C41 || version != 1 || rate < 1000 || rate > 768000 || channels < 1 ||
        channels > 32 || count < 1 || count > 2048 || decoded.stride < 256 ||
        decoded.stride > (qint64(1) << 40) || decoded.frames <= 0)
        return false;
    if (decoded.frames > qint64(rate) * 86400 * 7 || decoded.stride % 256 != 0 ||
        (decoded.stride & (decoded.stride - 1)) != 0)
        return false;
    if (bytes.size() != 36 + qint64(count) * channels * 4 + 32)
        return false;
    if ((decoded.frames + decoded.stride - 1) / decoded.stride != count)
        return false;
    decoded.sampleRate = rate;
    decoded.channels = channels;
    decoded.buckets.resize(count);
    for (auto &bucket : decoded.buckets) {
        bucket.resize(channels);
        for (auto &p : bucket) {
            qint16 low, high;
            in >> low >> high;
            if (low > high)
                return false;
            p = {low / 32767.0f, high / 32767.0f};
        }
    }
    if (in.status() != QDataStream::Ok)
        return false;
    *wave = std::move(decoded);
    return true;
}
QString waveKey(const QString &library, const QString &path, qint64 size, qint64 modified) {
    QByteArray input;
    QDataStream out(&input, QIODevice::WriteOnly);
    out << QString("envelope-v1-per-channel-2048") << library << path << size << modified;
    return QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}
static bool validKey(const QString &key) {
    return QRegularExpression("^[a-f0-9]{64}$").match(key).hasMatch();
}
static QString keyDir(const QString &root, const QString &key) {
    return QDir(root).filePath("peaks-v1/" + key.left(2) + "/" + key);
}
static QString viewPath(const QString &root, const QString &key) {
    return QDir(root).filePath("views-v1/" + key.left(2) + "/" + key + ".view");
}
static bool ensureCacheDirectory(const QString &path) {
    if (QDir().mkpath(path) || QFileInfo(path).isDir())
        return true;
    // Recursive mkpath can lose a simultaneous parent creation on Windows before
    // the other process has created the leaf. Ensure each parent independently;
    // checking only the final leaf mistakes that short race for unavailable storage.
    QStringList missing;
    QFileInfo current(path);
    while (!current.isDir()) {
        const auto absolute = QDir::cleanPath(current.absoluteFilePath());
        const auto parent = QDir::cleanPath(current.absolutePath());
        if (absolute == parent)
            return false;
        missing.prepend(absolute);
        current.setFile(parent);
    }
    for (const auto &directory : missing)
        if (!QDir().mkdir(directory) && !QFileInfo(directory).isDir())
            return false;
    return true;
}
bool saveWaveView(const QString &root, const QString &key, const WaveData &wave, bool approximate,
                  const QJsonObject &diagnostics) {
    if (root.isEmpty() || !validKey(key))
        return false;
    const auto data = encodeWave(wave);
    WaveData checked;
    if (!decodeWave(data, &checked))
        return false;
    const auto path = viewPath(root, key);
    if (!ensureCacheDirectory(QFileInfo(path).path()))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QDataStream out(&file);
    const auto metadata = approximate && !diagnostics.isEmpty()
                              ? QJsonDocument(diagnostics).toJson(QJsonDocument::Compact) : QByteArray();
    if (metadata.size() > 4096)
        return false;
    out << quint32(!metadata.isEmpty() ? 0x41575633 : approximate ? 0x41575632 : 0x41575631)
        << QDateTime::currentMSecsSinceEpoch();
    if (!metadata.isEmpty()) {
        out << quint32(metadata.size());
        if (out.writeRawData(metadata.constData(), metadata.size()) != metadata.size())
            return false;
    }
    return out.writeRawData(data.constData(), data.size()) == data.size() && file.commit();
}
bool loadWaveView(const QString &root, const QString &key, WaveData *wave, qint64 *verified,
                  bool *approximate, QJsonObject *diagnostics) {
    if (diagnostics)
        *diagnostics = {};
    if (root.isEmpty() || !validKey(key))
        return false;
    QFile file(viewPath(root, key));
    if (!file.open(QIODevice::ReadOnly) || file.size() < 84 || file.size() > 304112)
        return false;
    quint32 magic = 0;
    qint64 stamp = 0;
    QDataStream in(&file);
    in >> magic >> stamp;
    QFile epoch(QDir(root).filePath("views-v1/epoch"));
    if (epoch.open(QIODevice::ReadOnly) && stamp < epoch.read(32).toLongLong())
        return false;
    QJsonObject metadata;
    if (magic == 0x41575633) {
        quint32 size = 0;
        in >> size;
        if (size == 0 || size > 4096 || file.bytesAvailable() < size)
            return false;
        const auto document = QJsonDocument::fromJson(file.read(size));
        if (!document.isObject())
            return false;
        metadata = document.object();
    }
    if ((magic != 0x41575631 && magic != 0x41575632 && magic != 0x41575633) || stamp <= 0 ||
        !decodeWave(file.readAll(), wave))
        return false;
    if (verified)
        *verified = stamp;
    if (approximate)
        *approximate = magic != 0x41575631;
    if (diagnostics)
        *diagnostics = metadata;
    return true;
}
bool invalidateWaveViews(const QString &root) {
    if (!ensureCacheDirectory(QDir(root).filePath("views-v1")))
        return false;
    QSaveFile file(QDir(root).filePath("views-v1/epoch"));
    const auto stamp = QByteArray::number(QDateTime::currentMSecsSinceEpoch());
    return file.open(QIODevice::WriteOnly) && file.write(stamp) == stamp.size() && file.commit();
}
bool publishWave(const QString &root, const QString &key, const WaveData &wave, QString *error) {
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (root.isEmpty() || !validKey(key))
        return fail("缓存目录或键无效");
    const auto bytes = encodeWave(wave);
    WaveData checked;
    if (!decodeWave(bytes, &checked))
        return fail("波形校验失败");
    const auto folder = keyDir(root, key);
    if (!ensureCacheDirectory(folder))
        return fail("无法创建缓存目录");
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSaveFile object(QDir(folder).filePath(id + ".wave"));
    if (!object.open(QIODevice::WriteOnly) || object.write(bytes) != bytes.size() || !object.commit())
        return fail("峰值文件写入失败");
    QJsonObject m{
        {"version", 1},
        {"key", key},
        {"object", id + ".wave"},
        {"bytes", bytes.size()},
        {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
    const auto descriptor = QJsonDocument(m).toJson(QJsonDocument::Compact);
    QSaveFile manifest(QDir(folder).filePath(id + ".json"));
    if (!manifest.open(QIODevice::WriteOnly) || manifest.write(descriptor) != descriptor.size() ||
        !manifest.commit())
        return fail("完成描述发布失败");
    return true;
}
bool loadWave(const QString &root, const QString &key, WaveData *wave) {
    if (root.isEmpty() || !validKey(key))
        return false;
    QDir directory(keyDir(root, key));
    const auto manifests = directory.entryList({"*.json"}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    int tried = 0;
    for (const auto &name : manifests) {
        if (++tried > 64)
            break;
        QFile manifest(directory.filePath(name));
        if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > 4096)
            continue;
        const auto doc = QJsonDocument::fromJson(manifest.readAll());
        if (!doc.isObject())
            continue;
        const auto m = doc.object();
        const auto objectName = m["object"].toString();
        if (m["version"].toInt() != 1 || m["key"] != key ||
            !QRegularExpression("^[a-f0-9-]{36}\\.wave$").match(objectName).hasMatch())
            continue;
        const auto length = m["bytes"].toInteger();
        if (length < 72 || length > 300000)
            continue;
        QFile object(directory.filePath(objectName));
        if (QFileInfo(object).isSymLink() || !object.open(QIODevice::ReadOnly) || object.size() != length)
            continue;
        const auto bytes = object.readAll();
        if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() !=
            m["sha256"].toString().toLatin1())
            continue;
        if (decodeWave(bytes, wave))
            return true;
    }
    return false;
}
QString ensureLibraryIdentity(const QString &root, QString *error) {
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    if (!ensureCacheDirectory(root))
        return fail("无法创建共享缓存目录");
    const auto path = QDir(root).filePath("library.json");
    if (!QFile::exists(path)) {
        // New clients publish a complete staging file with an exclusive rename. They never
        // infer that a remote PID is dead. Honor a recent legacy initializer if present.
        const auto legacyLock = QDir(root).filePath(".library-init.lock");
        QElapsedTimer wait;
        wait.start();
        auto recentLegacy = [&] {
            const QFileInfo lock(legacyLock);
            return lock.exists() && lock.lastModified().msecsTo(QDateTime::currentDateTime()) < 60000;
        };
        while (!QFile::exists(path) && recentLegacy() && wait.elapsed() < 5000)
            QThread::msleep(25);
        if (!QFile::exists(path) && recentLegacy())
            return fail("另一台电脑正在初始化共享缓存，请稍后重试。");
        if (!QFile::exists(path)) {
            auto entries =
                QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            entries.removeAll(".library-init.lock");
            const QRegularExpression staged("^\\.library-init-[A-Za-z0-9]{6}\\.tmp$");
            entries.removeIf([&](const QString &name) {
                return staged.match(name).hasMatch() && QFileInfo(QDir(root).filePath(name)).isFile();
            });
            // A peer may have completed publication after our initial existence check.
            if (!entries.isEmpty() && !QFile::exists(path))
                return fail("请选择空的独立缓存文件夹，或已有有效库配置的缓存文件夹。");
            const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto bytes =
                QJsonDocument(QJsonObject{{"format", 1}, {"library_id", id}}).toJson(QJsonDocument::Compact);
            QString stage;
            {
                QTemporaryFile file(QDir(root).filePath(".library-init-XXXXXX.tmp"));
                if (!file.open())
                    return fail("缓存库配置不可写");
                if (file.write(bytes) != bytes.size() || !file.flush())
                    return fail("缓存库配置写入失败");
                stage = file.fileName();
                file.setAutoRemove(false);
            } // Close the native staging handle before the exclusive rename on Windows.
            if (!publishFileExclusive(stage, path)) {
                QFile::remove(stage);
                if (!QFile::exists(path))
                    return fail("共享目录不支持安全发布库配置，原配置未改动。");
            }
        }
    }
    QFile file(path);
    // A completed publication may already be visible while Windows still denies
    // sharing the handle. Retry only opening, never accept partial/invalid JSON.
    QElapsedTimer readWait;
    readWait.start();
    while (!file.open(QIODevice::ReadOnly)) {
        if (readWait.elapsed() >= 500 || !QFile::exists(path))
            return fail("缓存库配置不可读：" + file.errorString());
        QThread::msleep(25);
    }
    if (file.size() > 4096)
        return fail("缓存库配置不可读");
    const auto m = QJsonDocument::fromJson(file.readAll()).object();
    const auto id = m["library_id"].toString();
    if (m["format"].toInt() != 1 || QUuid(id).isNull())
        return fail("缓存库配置无效或版本不兼容，原配置已保留");
    return id;
}
static QString resolvedDirectory(const QString &path) {
    QFileInfo current(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    QStringList missing;
    while (!current.exists()) {
        missing.prepend(current.fileName());
        const auto parent = current.dir().absolutePath();
        if (parent == current.absoluteFilePath())
            return {};
        current.setFile(parent);
    }
    const auto canonical = current.canonicalFilePath();
    return canonical.isEmpty() ? QString() : QDir::cleanPath(QDir(canonical).filePath(missing.join('/')));
}
bool prepareSharedCache(const QString &musicRoot, const QString &cacheRoot, QString *canonicalCache,
                        QString *sharedId, QString *error) {
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (cacheRoot.trimmed().isEmpty() || !QFileInfo(musicRoot).isDir() || !QFileInfo(musicRoot).isReadable())
        return fail("音乐目录不可读，或共享缓存目录为空。");
    const auto root = resolvedDirectory(musicRoot), cache = resolvedDirectory(cacheRoot);
#ifdef Q_OS_WIN
    constexpr auto comparison = Qt::CaseInsensitive;
#else
    constexpr auto comparison = Qt::CaseSensitive;
#endif
    const auto prefix = cache.endsWith('/') ? cache : cache + '/';
    if (root.isEmpty() || cache.isEmpty() || root.compare(cache, comparison) == 0 ||
        root.startsWith(prefix, comparison))
        return fail("缓存须使用独立文件夹，不能直接使用音乐根目录或它的上级目录。");
    if (QFileInfo(cache).exists() && !QFileInfo(cache).isDir())
        return fail("缓存位置不是文件夹。");
    QString identityError;
    const auto id = ensureLibraryIdentity(cache, &identityError);
    if (id.isEmpty())
        return fail(identityError);
    QTemporaryFile probe(QDir(cache).filePath(".access-check-XXXXXX.tmp"));
    const QByteArray bytes = QUuid::createUuid().toByteArray();
    if (!probe.open() || probe.write(bytes) != bytes.size() || !probe.flush() || !probe.seek(0) ||
        probe.readAll() != bytes)
        return fail("共享缓存需要读写权限，原有素材库配置未改变。");
    if (canonicalCache)
        *canonicalCache = cache;
    if (sharedId)
        *sharedId = id;
    return true;
}
SharedWaveLease::SharedWaveLease(const QString &root, const QString &key, int staleAfterMs)
    : key_(key), token_(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      staleAfterMs_(qMax(1, staleAfterMs)) {
    if (!root.isEmpty() && validKey(key))
        path_ = QDir(root).filePath("generation-v1/" + key.left(2) + "/" + key + ".lease");
}
QByteArray SharedWaveLease::payload() const {
    return QJsonDocument(
               QJsonObject{{"format", 1}, {"key", key_}, {"owner", token_}, {"sequence", sequence_}})
        .toJson(QJsonDocument::Compact);
}
SharedWaveLease::~SharedWaveLease() {
    if (!owned_ || QFileInfo(path_).isSymLink())
        return;
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 2048)
        return;
    const auto owner = QJsonDocument::fromJson(file.readAll()).object()["owner"].toString();
    file.close();
    if (owner == token_)
        QFile::remove(path_); // Only the immutable owner can remove its own claim.
}
SharedWaveLease::State SharedWaveLease::acquireOrObserve() {
    if (owned_)
        return State::Owned;
    const auto directory = QFileInfo(path_).absolutePath();
    if (path_.isEmpty() || !ensureCacheDirectory(directory) || QFileInfo(path_).isSymLink())
        return State::Unavailable;
    QFile claim(path_);
    if (claim.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        const auto bytes = payload();
        if (claim.write(bytes) != bytes.size() || !claim.flush()) {
            claim.close();
            QFile::remove(path_); // This process just exclusively created this failed claim.
            return State::Unavailable;
        }
        owned_ = true;
        heartbeat_.start();
        return State::Owned;
    }
    if (!QFileInfo::exists(path_))
        return State::Unavailable;
    QFile peer(path_);
    QByteArray bytes;
    if (peer.open(QIODevice::ReadOnly) && peer.size() <= 2048)
        bytes = peer.readAll();
    // Compare heartbeat changes with our own monotonic clock; machine clocks and PIDs are irrelevant.
    if (!unchanged_.isValid() || bytes != observed_) {
        observed_ = bytes;
        unchanged_.restart();
    }
    return unchanged_.elapsed() >= staleAfterMs_ ? State::Expired : State::Waiting;
}
bool SharedWaveLease::renew() {
    if (!owned_ || QFileInfo(path_).isSymLink())
        return false;
    QFile current(path_);
    if (!current.open(QIODevice::ReadOnly) || current.size() > 2048 ||
        QJsonDocument::fromJson(current.readAll()).object()["owner"].toString() != token_)
        return false;
    current.close();
    ++sequence_;
    const auto bytes = payload();
    QSaveFile heartbeat(path_);
    return heartbeat.open(QIODevice::WriteOnly) && heartbeat.write(bytes) == bytes.size() &&
           heartbeat.commit();
}
static QString portablePath(const QString &root, const QString &sharedId, const QString &relative,
                            qint64 size, qint64 modified) {
    const auto key = waveKey(sharedId, relative, size, modified);
    return QDir(root).filePath("portable-v1/" + key.left(2) + "/" + key + ".json");
}
bool publishPortableWave(const QString &root, const QString &sharedId, const QString &relative, qint64 size,
                         qint64 modified, const WaveData &wave, bool approximate, QString *error) {
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (approximate)
        return fail("参考波形仅保留本机，不上传到共享缓存。");
    if (root.isEmpty() || sharedId.isEmpty() || sharedId.size() > 256 || relative.isEmpty() ||
        relative.size() > 32768 || relative.startsWith('/') || relative.contains('\\') ||
        relative.split('/').contains("..") || size < 0)
        return fail("共享波形的库或文件版本信息无效。");
    const auto bytes = encodeWave(wave);
    WaveData checked;
    if (!decodeWave(bytes, &checked))
        return fail("现有波形未通过完整性校验。");
    WaveData existing;
    if (loadPortableWave(root, sharedId, relative, size, modified, &existing) &&
        encodeWave(existing) == bytes)
        return true;
    const auto path = portablePath(root, sharedId, relative, size, modified);
    if (!ensureCacheDirectory(QFileInfo(path).absolutePath()))
        return fail("无法创建共享波形目录。");
    const auto document =
        QJsonDocument(
            QJsonObject{{"format", 1},
                        {"origin", "indexed-local-view"},
                        {"accurate", true},
                        {"shared_library", sharedId},
                        {"relative", relative},
                        {"size", size},
                        {"modified", modified},
                        {"published_at", QDateTime::currentMSecsSinceEpoch()},
                        {"sha256", QString::fromLatin1(
                                       QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
                        {"data", QString::fromLatin1(bytes.toBase64())}})
            .toJson(QJsonDocument::Compact);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(document) != document.size() || !file.commit())
        return fail("共享波形保存失败，请检查网络或写入权限。");
    return true;
}
bool loadPortableWave(const QString &root, const QString &sharedId, const QString &relative, qint64 size,
                      qint64 modified, WaveData *wave) {
    if (root.isEmpty() || sharedId.isEmpty())
        return false;
    const auto path = portablePath(root, sharedId, relative, size, modified);
    if (QFileInfo(path).isSymLink())
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 512 * 1024)
        return false;
    const auto m = QJsonDocument::fromJson(file.readAll()).object();
    if (m["format"].toInt() != 1 || m["origin"].toString() != "indexed-local-view" ||
        !m["accurate"].toBool() || m["shared_library"].toString() != sharedId ||
        m["relative"].toString() != relative || m["size"].toInteger(-1) != size || !m.contains("modified") ||
        m["modified"].toInteger() != modified)
        return false;
    const auto bytes =
        QByteArray::fromBase64(m["data"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() ==
               m["sha256"].toString().toLatin1() &&
           decodeWave(bytes, wave);
}
