#pragma once
#include "index.h"
#include <QAbstractItemModel>
#include <QThreadPool>
#include <atomic>
#include <memory>
#include <vector>

class FolderModel : public QAbstractItemModel {
    Q_OBJECT
  public:
    enum Role { LibraryRole = Qt::UserRole, PathRole, FavoriteRole };
    explicit FolderModel(QString database, QObject *parent = nullptr);
    ~FolderModel() override;
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex & = {}) const override {
        return 1;
    }
    QVariant data(const QModelIndex &index, int role) const override;
    bool hasChildren(const QModelIndex &parent = {}) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;
    void setLibraries(const QList<Library> &libraries);
    // Remove a catalog-excluded folder from the visible tree without resetting other libraries.
    bool removeFolderSubtree(const QString &libraryId, const QString &path);
    void refresh();
    QModelIndex materialize(const QList<FolderRecord> &ancestors);
    QModelIndex special(bool favorite) const;
  signals:
    void error(const QString &message);
    void pageLoaded();

  private:
    struct Node {
        FolderRecord folder;
        Node *parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
        bool special = false, favorite = false, busy = false, more = true;
        int fetched = 0;
    };
    QString database_;
    Node root_;
    QThreadPool pool_;
    quint64 epoch_ = 0;
    std::shared_ptr<std::atomic<quint64>> latestEpoch_ = std::make_shared<std::atomic<quint64>>(0);
    Node *node(const QModelIndex &index) const;
    QModelIndex nodeIndex(Node *node) const;
    Node *insert(Node *parent, const FolderRecord &folder);
    void invalidatePendingPages();
};
