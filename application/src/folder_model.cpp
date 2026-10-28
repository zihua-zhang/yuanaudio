#include "folder_model.h"
#include "theme.h"
#include "ui_icons.h"
#include <QFutureWatcher>
#include <QSet>
#include <QtConcurrent>
#include <algorithm>

FolderModel::FolderModel(QString database, QObject *parent)
    : QAbstractItemModel(parent), database_(std::move(database)) {
    pool_.setMaxThreadCount(1);
    for (int i = 0; i < 2; ++i) {
        auto entry = std::make_unique<Node>();
        entry->special = true;
        entry->favorite = i == 1;
        entry->folder.name = i == 0 ? "全部音乐" : "我的收藏";
        entry->parent = &root_;
        entry->more = false;
        root_.children.push_back(std::move(entry));
    }
}
FolderModel::~FolderModel() {
    *latestEpoch_ = ++epoch_;
    pool_.clear();
    pool_.waitForDone();
}
FolderModel::Node *FolderModel::node(const QModelIndex &index) const {
    return index.isValid() ? static_cast<Node *>(index.internalPointer()) : const_cast<Node *>(&root_);
}
QModelIndex FolderModel::nodeIndex(Node *n) const {
    if (!n || n == &root_ || !n->parent)
        return {};
    const auto &siblings = n->parent->children;
    for (size_t i = 0; i < siblings.size(); ++i)
        if (siblings[i].get() == n)
            return createIndex(int(i), 0, n);
    return {};
}
QModelIndex FolderModel::index(int row, int column, const QModelIndex &parent) const {
    const auto *p = node(parent);
    if (column != 0 || row < 0 || size_t(row) >= p->children.size())
        return {};
    return createIndex(row, column, p->children[size_t(row)].get());
}
QModelIndex FolderModel::parent(const QModelIndex &index) const {
    return index.isValid() ? nodeIndex(node(index)->parent) : QModelIndex();
}
int FolderModel::rowCount(const QModelIndex &parent) const {
    return parent.column() > 0 ? 0 : int(node(parent)->children.size());
}
QVariant FolderModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid())
        return {};
    const auto *n = node(index);
    switch (role) {
    case Qt::DisplayRole:
        return n->folder.name;
    case Qt::DecorationRole:
        return ui::icon(n->special ? (n->favorite ? ui::Icon::Star : ui::Icon::Music)
                                  : (n->parent == &root_ ? ui::Icon::Library : ui::Icon::Folder),
                        theme::current().muted, 16);
    case Qt::ToolTipRole:
        return n->special ? n->folder.name : n->folder.libraryName + " / " + n->folder.path;
    case LibraryRole:
        return n->folder.libraryId;
    case PathRole:
        return n->folder.path;
    case FavoriteRole:
        return n->favorite;
    }
    return {};
}
bool FolderModel::hasChildren(const QModelIndex &parent) const {
    const auto *n = node(parent);
    return !n->children.empty() || (!n->special && n->more && n->folder.hasChildren);
}
bool FolderModel::canFetchMore(const QModelIndex &parent) const {
    const auto *n = node(parent);
    return parent.isValid() && !n->special && !n->busy && n->more && n->folder.hasChildren;
}
FolderModel::Node *FolderModel::insert(Node *p, const FolderRecord &folder) {
    for (const auto &child : p->children)
        if (!child->special && child->folder.libraryId == folder.libraryId &&
            child->folder.path == folder.path) {
            child->folder = folder;
            emit dataChanged(nodeIndex(child.get()), nodeIndex(child.get()));
            return child.get();
        }
    int row = int(p->children.size());
    if (p != &root_) {
        row = 0;
        while (row < int(p->children.size()) && p->children[size_t(row)]->folder.path < folder.path)
            ++row;
    }
    beginInsertRows(nodeIndex(p), row, row);
    auto child = std::make_unique<Node>();
    child->folder = folder;
    child->parent = p;
    auto *result = child.get();
    p->children.insert(p->children.begin() + row, std::move(child));
    endInsertRows();
    return result;
}
void FolderModel::fetchMore(const QModelIndex &parent) {
    if (!canFetchMore(parent))
        return;
    auto *n = node(parent);
    n->busy = true;
    const auto epoch = epoch_;
    const auto offset = n->fetched;
    const auto database = database_, library = n->folder.libraryId, path = n->folder.path;
    auto *watcher = new QFutureWatcher<FolderPage>(this);
    connect(watcher, &QFutureWatcher<FolderPage>::finished, this, [this, watcher, n, epoch] {
        const auto page = watcher->result();
        watcher->deleteLater();
        if (epoch != epoch_)
            return;
        n->busy = false;
        if (!page.error.isEmpty()) {
            n->more = false; // refresh retries, avoid a view-triggered error loop
            emit error(page.error);
            return;
        }
        n->fetched += page.folders.size();
        n->more = page.more;
        for (const auto &folder : page.folders)
            insert(n, folder);
        emit dataChanged(nodeIndex(n), nodeIndex(n));
        emit pageLoaded();
    });
    watcher->setFuture(
        QtConcurrent::run(&pool_, [database, library, path, offset, epoch, latest = latestEpoch_] {
            if (latest->load() != epoch)
                return FolderPage{};
            Index db(database);
            QString error;
            if (!db.open(&error)) {
                FolderPage p;
                p.error = error;
                return p;
            }
            if (latest->load() != epoch)
                return FolderPage{};
            return db.childFolders(library, path, offset);
        }));
}
void FolderModel::setLibraries(const QList<Library> &libraries) {
    QSet<QString> wanted;
    for (const auto &library : libraries)
        wanted.insert(library.id);
    bool removing = false;
    for (int row = int(root_.children.size()) - 1; row >= 2; --row) {
        if (wanted.contains(root_.children[size_t(row)]->folder.libraryId))
            continue;
        removing = true;
        beginRemoveRows({}, row, row);
        root_.children.erase(root_.children.begin() + row);
        endRemoveRows();
    }
    if (removing)
        invalidatePendingPages();
    for (const auto &library : libraries)
        insert(&root_, {library.id, "", "", library.name, library.name, true});
}
bool FolderModel::removeFolderSubtree(const QString &libraryId, const QString &path) {
    if (libraryId.isEmpty() || path.isEmpty())
        return false;
    bool removed = false;
    for (const auto &library : root_.children) {
        if (library->special || library->folder.libraryId != libraryId)
            continue;
        std::function<bool(Node *)> remove = [&](Node *parent) {
            for (int row = 0; row < int(parent->children.size()); ++row) {
                auto *child = parent->children[size_t(row)].get();
                if (child->folder.path == path) {
                    beginRemoveRows(nodeIndex(parent), row, row);
                    parent->children.erase(parent->children.begin() + row);
                    endRemoveRows();
                    parent->fetched = qMax(0, parent->fetched - 1);
                    return true;
                }
                if (path.startsWith(child->folder.path + '/') && remove(child))
                    return true;
            }
            return false;
        };
        removed = remove(library.get());
        break;
    }
    // A page requested before the catalog change may contain the deleted folder even
    // when that folder had not been materialized in the model yet.
    invalidatePendingPages();
    return removed;
}
void FolderModel::invalidatePendingPages() {
    *latestEpoch_ = ++epoch_;
    std::vector<Node *> pending;
    std::function<void(Node *)> visit = [&](Node *parent) {
        for (const auto &child : parent->children)
            visit(child.get());
        if (parent->busy) {
            parent->busy = false;
            parent->more = true;
            parent->fetched = 0;
            pending.push_back(parent);
        }
    };
    visit(&root_);
    for (auto *parent : pending)
        fetchMore(nodeIndex(parent));
}
void FolderModel::refresh() {
    *latestEpoch_ = ++epoch_;
    std::vector<Node *> pending;
    std::function<void(Node *)> visit = [&](Node *n) {
        for (const auto &child : n->children)
            visit(child.get());
        if (n != &root_ && !n->special) {
            if (n->fetched || n->busy)
                pending.push_back(n);
            n->fetched = 0;
            n->busy = false;
            n->more = true;
            n->folder.hasChildren = true;
        }
    };
    visit(&root_);
    for (auto *n : pending)
        fetchMore(nodeIndex(n));
}
QModelIndex FolderModel::materialize(const QList<FolderRecord> &ancestors) {
    Node *current = &root_;
    for (const auto &folder : ancestors)
        current = insert(current, folder);
    return nodeIndex(current);
}
QModelIndex FolderModel::special(bool favorite) const {
    return index(favorite ? 1 : 0, 0);
}
