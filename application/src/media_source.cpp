#include "media_source.h"
#include <QCryptographicHash>
#include <QFile>

QString verifiedMediaPath(const Track &track, QString *error,
                          const std::function<void(qint64)> &progress) {
    if (error)
        error->clear();
    if (track.compatiblePath.isEmpty())
        return track.absolutePath();
    if (progress)
        progress(0);
    if (!track.sourceMatches(error))
        return {};
    QFile file(track.compatiblePath);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto expected = track.compatibleSha256.toLower();
    auto fail = [&] {
        if (error)
            *error = "兼容副本的完整 SHA-256 校验未通过，请重新修复原曲路径。";
        return QString();
    };
    if (expected.size() != 64 || QByteArray::fromHex(expected).toHex() != expected ||
        !file.open(QIODevice::ReadOnly))
        return fail();
    qint64 read = 0;
    while (!file.atEnd()) {
        // Bound a read so healthy SMB verification can report activity throughout
        // large files instead of appearing stalled to the parent watchdog.
        const auto chunk = file.read(256 * 1024);
        if (chunk.isEmpty() || file.error() != QFileDevice::NoError)
            return fail();
        hash.addData(chunk);
        read += chunk.size();
        if (progress)
            progress(read);
    }
    if (file.error() != QFileDevice::NoError || read != track.compatibleSize ||
        hash.result().toHex() != expected)
        return fail();
    file.close();
    if (!track.sourceMatches(error))
        return {};
    return track.sourcePath();
}
