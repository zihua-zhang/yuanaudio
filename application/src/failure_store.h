#pragma once
#include "index.h"
#include <QJsonArray>
#include <QJsonObject>

struct FailureRecord {
    qint64 id = 0;
    Track track;
    QString kind, message;
    bool reference = false;
};
struct FailureOutcome {
    Track track;
    bool success = false, reference = false;
    QString kind, message;
    qint64 retryId = 0;
};

// Local-only SQLite catalog. Instances and all calls stay on a background thread.
class FailureStore {
  public:
    FailureStore(QString path, QString indexPath);
    ~FailureStore();
    bool open(QString *error);
    bool importLegacy(const QJsonArray &recent, qint64 failed, QString *error);
    bool apply(const QList<FailureOutcome> &outcomes, QString *error);
    QJsonObject summary(QString *error = nullptr) const;
    QJsonObject page(int offset, int limit, const QString &category, bool references, QString *error) const;
    QList<FailureRecord> retryPage(qint64 cursor, qint64 upper, int limit, QString *error,
                                   bool references = false) const;
    qint64 retryUpper(QString *error, bool references = false) const;
    bool clearLegacyUnknown(QString *error);
    static QString category(const QString &kind);

  private:
    QString path_, indexPath_, connection_;
    QSqlDatabase db_;
    mutable bool catalogAttached_ = false;
    bool attachCatalog(QString *error) const;
    QString activeFilter(QString *error) const;
    bool currentTracks(QList<FailureRecord> *records, QString *error) const;
};
