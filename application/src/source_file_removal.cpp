#include "source_file_removal.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

namespace {
#ifdef Q_OS_WIN
constexpr auto pathComparison = Qt::CaseInsensitive;
#else
constexpr auto pathComparison = Qt::CaseSensitive;
#endif
bool pathInsideOrEqual(const QString &child, const QString &parent) {
    const auto prefix = parent.endsWith('/') ? parent : parent + '/';
    return child.compare(parent, pathComparison) == 0 ||
           child.startsWith(prefix, pathComparison);
}
QString canonicalExisting(const QString &path) {
    const auto resolved = QFileInfo(path).canonicalFilePath();
    return resolved.isEmpty() ? QString() : QDir::cleanPath(resolved);
}
} // namespace

QString validatedOriginalPath(const Library &library, const QString &relativePath, bool folder,
                              QString *error) {
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    if (library.id.isEmpty() || library.root.isEmpty() || relativePath.isEmpty() ||
        QDir::isAbsolutePath(relativePath) || relativePath.startsWith('/') ||
        relativePath.contains('\\') || relativePath.contains(':') ||
        QDir::cleanPath(relativePath) != relativePath ||
        relativePath.split('/').contains("..") || relativePath.split('/').contains("."))
        return fail("原文件路径无效，未执行删除。");

    const QFileInfo rootInfo(library.root);
    const auto root = canonicalExisting(library.root);
    if (!rootInfo.isDir() || root.isEmpty())
        return fail("素材库目录不可访问，未执行删除。");

    const auto targetPath = QDir(root).filePath(relativePath);
    const QFileInfo targetInfo(targetPath);
    const auto target = canonicalExisting(targetPath);
    if (!targetInfo.exists() || target.isEmpty())
        return fail("原文件已不存在或不可访问，未执行删除。");
    const auto rootPrefix = root.endsWith('/') ? root : root + '/';
    if (!target.startsWith(rootPrefix, pathComparison))
        return fail("原文件不在该素材库目录内，未执行删除。");
    if (targetInfo.isSymbolicLink() || targetInfo.isDir() != folder ||
        (!folder && !targetInfo.isFile()))
        return fail("原文件类型已变化或是链接，未执行删除。");

    // A symlink in any intermediate directory can change where deletion lands.
    auto parent = targetInfo.dir();
    const auto rootDir = QDir(root);
    while (QDir::cleanPath(parent.absolutePath()).compare(root, pathComparison) != 0) {
        if (QFileInfo(parent.absolutePath()).isSymbolicLink() ||
            !canonicalExisting(parent.absolutePath()).startsWith(rootPrefix, pathComparison))
            return fail("原文件路径经过链接或已离开素材库，未执行删除。");
        if (!parent.cdUp())
            return fail("无法核对原文件所在目录，未执行删除。");
    }
    if (!rootDir.exists())
        return fail("素材库目录不可访问，未执行删除。");
    if (error)
        error->clear();
    return targetPath;
}

bool moveOriginalToTrash(const Library &library, const QString &relativePath, bool folder,
                         const QList<Library> &allLibraries, QString *error) {
    const auto path = validatedOriginalPath(library, relativePath, folder, error);
    if (path.isEmpty())
        return false;
    const auto target = canonicalExisting(path);
    bool knownLibrary = false;
    for (const auto &entry : allLibraries) {
        knownLibrary |= entry.id == library.id &&
                        canonicalExisting(entry.root) == canonicalExisting(library.root);
        if (entry.id != library.id) {
            const auto otherRoot = canonicalExisting(entry.root);
            if (!otherRoot.isEmpty() &&
                (pathInsideOrEqual(target, otherRoot) || (folder && pathInsideOrEqual(otherRoot, target)))) {
                if (error)
                    *error = "这个原件与另一个已连接的素材库重叠，未执行删除。";
                return false;
            }
        }
        if (!entry.sharedCache.isEmpty()) {
            const auto cache = canonicalExisting(entry.sharedCache);
            if (!cache.isEmpty() &&
                (pathInsideOrEqual(target, cache) || (folder && pathInsideOrEqual(cache, target)))) {
                if (error)
                    *error = "这个原件与正在使用的共享波形缓存重叠，未执行删除。";
                return false;
            }
        }
    }
    if (!knownLibrary) {
        if (error)
            *error = "素材库配置已变化，未执行删除。";
        return false;
    }
    if (!QFile::supportsMoveToTrash()) {
        if (error)
            *error = "当前系统不支持回收站，未删除原文件；可仅从列表移除。";
        return false;
    }
    if (!QFile::moveToTrash(path)) {
        if (error)
            *error = "无法移入系统回收站。共享目录可能不支持回收站；原文件与列表均未改变。";
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool moveLibraryRootToTrash(const Library &library, const QList<Library> &allLibraries,
                            QString *error) {
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const QFileInfo rootInfo(library.root);
    const auto root = canonicalExisting(library.root);
    if (library.id.isEmpty() || !rootInfo.isDir() || rootInfo.isSymbolicLink() || root.isEmpty())
        return fail("素材库根目录不可访问或是链接，未执行删除。");
    bool knownLibrary = false;
    for (const auto &entry : allLibraries)
        knownLibrary |= entry.id == library.id &&
                        canonicalExisting(entry.root) == root;
    if (!knownLibrary)
        return fail("素材库配置已变化，未执行删除。");
    const QStorageInfo volume(root);
    const auto volumeRoot = canonicalExisting(volume.rootPath());
    if (!volume.isValid() || volumeRoot.isEmpty() || QDir(root).isRoot() ||
        root.compare(volumeRoot, pathComparison) == 0)
        return fail("不能将整个磁盘或网络共享根目录移入回收站。");
    for (const auto &other : allLibraries) {
        if (other.id != library.id) {
            const auto otherRoot = canonicalExisting(other.root);
            if (!otherRoot.isEmpty() &&
                (pathInsideOrEqual(otherRoot, root) || pathInsideOrEqual(root, otherRoot)))
                return fail("这个目录与另一个已连接的素材库重叠，未执行删除。");
        }
        if (!other.sharedCache.isEmpty()) {
            const auto cache = canonicalExisting(other.sharedCache);
            if (!cache.isEmpty() &&
                (pathInsideOrEqual(cache, root) || pathInsideOrEqual(root, cache)))
                return fail("这个目录与正在使用的共享波形缓存重叠，未执行删除。");
        }
    }
    if (!QFile::supportsMoveToTrash())
        return fail("当前系统不支持回收站，未删除素材库原件；可仅从列表移除。");
    if (!QFile::moveToTrash(root))
        return fail("无法将整个素材库移入系统回收站。共享目录可能不支持回收站；原文件与列表均未改变。");
    if (error)
        error->clear();
    return true;
}
