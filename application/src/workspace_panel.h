#pragma once
#include "workspace_store.h"
#include <QListWidget>
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QMimeData;

class WorkspaceTrackList : public QListWidget {
    Q_OBJECT
  public:
    using QListWidget::QListWidget;
    void setTrackRows(const QList<Track> &tracks);
    void setFolderRows(const QList<PinnedFolder> &folders);
    QMimeData *originalMimeData() const;
    void setCatalogPath(const QString &path) { catalogPath_ = path; }

  protected:
    void startDrag(Qt::DropActions supportedActions) override;

  private:
    QList<Track> tracks_;
    QString catalogPath_;
};

class WorkspacePanel : public QWidget {
    Q_OBJECT
  public:
    enum Mode { Candidates, Recent, Folders };
    explicit WorkspacePanel(const QString &databasePath, QWidget *parent = nullptr);
    WorkspaceStore &store();
    void setCatalogPath(const QString &path);
    bool addCandidate(const Track &track);
    bool recordAudition(const Track &track);
    bool pinFolder(const QString &libraryId, const QString &path, const QString &name,
                   const QString &root = {});
    void refresh();
    void setMode(Mode mode);
    qint64 currentCandidateList() const;
    void setCurrentCandidateList(qint64 listId);

  signals:
    void trackActivated(const Track &track);
    void folderActivated(const QString &libraryId, const QString &path);
    void warning(const QString &message);

  private:
    WorkspaceStore store_;
    QComboBox *mode_, *lists_;
    QWidget *listTools_;
    WorkspaceTrackList *rows_;
    QLabel *notice_;
    QList<Track> tracks_;
    QList<PinnedFolder> folders_;
    bool report(bool success);
    void activateRow(int row);
    void editLists();
    void contextMenu(const QPoint &point);
};
