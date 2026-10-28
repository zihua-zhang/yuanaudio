#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

// Native bridge, also exposed here for independent protocol-vector tests.
namespace UgosCrypto {
QByteArray randomBytes(int count);
QByteArray seal(const QByteArray &key, const QByteArray &plain,
                const QByteArray &nonce = {});
QByteArray open(const QByteArray &key, const QByteArray &combined);
QByteArray rsaEncrypt(const QByteArray &publicPem, const QByteArray &plain);
}

// Synchronous: construct and use on the same background thread. No credentials,
// API tokens, download tokens or server response text are logged or persisted.
class UgosClient {
  public:
    UgosClient();
    ~UgosClient();
    UgosClient(const UgosClient &) = delete;
    UgosClient &operator=(const UgosClient &) = delete;
    bool login(QUrl endpoint, QString username, QString password, QString *error);
    QJsonObject call(QString method, QString api, QJsonObject body = {},
                     QUrlQuery query = {}, QString *error = nullptr);
    // NewOnly destination. On failure removes only the partial created by this
    // invocation. sha256 receives the raw 32-byte digest, never a hex string.
    bool download(QString remotePath, QString localTemp, QByteArray *sha256,
                  QString *error);
    // Typed stop condition for a batch: destination write/flush failed. Reset
    // at the start of each download; network/API/read failures leave it false.
    bool lastDownloadStorageFailure() const { return downloadStorageFailure_; }
    void setTimeoutMs(int milliseconds);
    static bool validateEndpoint(const QUrl &endpoint, QString *error = nullptr);

  private:
    QUrl endpoint_;
    QByteArray token_;
    QByteArray publicKey_;
    QString authType_;
    QByteArray clientId_;
    int timeoutMs_ = 35000;
    bool downloadStorageFailure_ = false;
    bool resolveLocalEndpoint(QString *error) const;
    QJsonObject plainJson(QString method, QString api, QJsonObject body,
                          QString *error);
    void clearSession();
};
