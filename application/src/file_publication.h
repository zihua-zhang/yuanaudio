#pragma once
#include <QFile>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

namespace file_publication_detail {
inline bool publishByHardLinkExclusive(const QString &source, const QString &target) {
    // POSIX link(2) creates the destination entry atomically and fails if it
    // already exists. It never exposes a partly written staging file.
    const auto from = QFile::encodeName(source);
    const auto to = QFile::encodeName(target);
    if (::link(from.constData(), to.constData()) != 0)
        return false;
    ::unlink(from.constData());
    return true;
}
} // namespace file_publication_detail
#endif

inline bool publishFileExclusive(const QString &source, const QString &target) {
    // Both paths must be on the same filesystem. Never fall back to copying:
    // QFile::rename can truncate a competing winner after its initial existence
    // check, exposing partial content (and may then remove that winner on error).
#ifdef Q_OS_WIN
    return MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
                       reinterpret_cast<LPCWSTR>(target.utf16()), MOVEFILE_WRITE_THROUGH);
#elif defined(Q_OS_DARWIN)
    const auto from = QFile::encodeName(source);
    const auto to = QFile::encodeName(target);
    if (renameatx_np(AT_FDCWD, from.constData(), AT_FDCWD, to.constData(), RENAME_EXCL) == 0)
        return true;
    // Some SMB mounts reject exclusive rename. A hard link is a second
    // no-overwrite operation; unsupported shares still fail safely.
    return file_publication_detail::publishByHardLinkExclusive(source, target);
#else
    return file_publication_detail::publishByHardLinkExclusive(source, target);
#endif
}
