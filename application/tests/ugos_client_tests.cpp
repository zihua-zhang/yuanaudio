#include "ugos_client.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <Security/Security.h>
#include <functional>
#include <sys/resource.h>
#include <signal.h>

namespace {
// A real filesystem write failure without a production injection seam or a
// disk image. Restore both process limits and signal disposition on every exit.
class ScopedFileSizeLimit {
  public:
    explicit ScopedFileSizeLimit(rlim_t maximum) {
        if (::getrlimit(RLIMIT_FSIZE,&previousLimit_) != 0) return;
        struct sigaction ignored{};
        ignored.sa_handler = SIG_IGN;
        sigemptyset(&ignored.sa_mask);
        if (::sigaction(SIGXFSZ,&ignored,&previousSignal_) != 0) return;
        auto limited = previousLimit_;
        limited.rlim_cur = std::min(maximum,previousLimit_.rlim_max);
        if (::setrlimit(RLIMIT_FSIZE,&limited) != 0) {
            ::sigaction(SIGXFSZ,&previousSignal_,nullptr);
            return;
        }
        active_ = true;
    }
    ~ScopedFileSizeLimit() {
        if (active_) {
            ::setrlimit(RLIMIT_FSIZE,&previousLimit_);
            ::sigaction(SIGXFSZ,&previousSignal_,nullptr);
        }
    }
    bool active() const { return active_; }
  private:
    struct rlimit previousLimit_{};
    struct sigaction previousSignal_{};
    bool active_ = false;
};
QByteArray cfBytes(CFDataRef data) {
    return data ? QByteArray(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                             CFDataGetLength(data)) : QByteArray();
}
class FakeNas : public QTcpServer {
  public:
    FakeNas() {
        const void *keys[] = {kSecAttrKeyType, kSecAttrKeySizeInBits};
        const void *values[] = {kSecAttrKeyTypeRSA, nullptr};
        int bits = 2048;
        auto size = CFNumberCreate(nullptr, kCFNumberIntType, &bits);
        values[1] = size;
        auto attrs = CFDictionaryCreate(nullptr, keys, values, 2,
                                        &kCFTypeDictionaryKeyCallBacks,
                                        &kCFTypeDictionaryValueCallBacks);
        privateKey = SecKeyCreateRandomKey(attrs, nullptr);
        CFRelease(attrs);
        CFRelease(size);
        auto pub = SecKeyCopyPublicKey(privateKey);
        auto bytes = SecKeyCopyExternalRepresentation(pub, nullptr);
        publicPem = "-----BEGIN RSA PUBLIC KEY-----\n" + cfBytes(bytes).toBase64()
                    + "\n-----END RSA PUBLIC KEY-----\n";
        CFRelease(bytes);
        CFRelease(pub);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto socket = nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    const auto split = buffer->indexOf("\r\n\r\n");
                    if (split < 0) return;
                    const auto lines = buffer->left(split).split('\n');
                    QMap<QByteArray,QByteArray> headers;
                    for (const auto &line : lines) {
                        const auto colon = line.indexOf(':');
                        if (colon > 0) headers.insert(line.left(colon).trimmed().toLower(),
                                                      line.mid(colon+1).trimmed());
                    }
                    const int length = headers.value("content-length").toInt();
                    if (buffer->size() < split+4+length) return;
                    socket->disconnect(this);
                    handle(socket, QUrl::fromEncoded(lines.first().split(' ').value(1)),
                           headers, buffer->mid(split+4, length));
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        listen(QHostAddress::LocalHost);
    }
    ~FakeNas() { if (privateKey) CFRelease(privateKey); }
    QUrl url() const { return QUrl("http://127.0.0.1:" + QString::number(serverPort())); }
    QByteArray decryptRsa(const QByteArray &data) {
        auto encoded = CFDataCreate(nullptr,
            reinterpret_cast<const UInt8 *>(data.constData()), data.size());
        auto decoded = SecKeyCreateDecryptedData(privateKey,
                        kSecKeyAlgorithmRSAEncryptionPKCS1, encoded, nullptr);
        auto result = cfBytes(decoded);
        if (decoded) CFRelease(decoded);
        CFRelease(encoded);
        return result;
    }
    QByteArray spkiPem() const {
        const auto raw = QByteArray::fromBase64(publicPem.split('\n').value(1));
        // RSA-2048 SubjectPublicKeyInfo wrapper (270-byte PKCS#1 inner key).
        const auto spki = QByteArray::fromHex("30820122300d06092a864886f70d01010105000382010f00") + raw;
        return "-----BEGIN PUBLIC KEY-----\n" + spki.toBase64() + "\n-----END PUBLIC KEY-----\n";
    }
    static void reply(QTcpSocket *socket, const QByteArray &data,
                       const QByteArray &type = "application/json",
                       const QByteArray &extra = {}, int status = 200) {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Result\r\nContent-Type: "
                      + type + "\r\nContent-Length: " + QByteArray::number(data.size())
                      + "\r\nConnection: close\r\n" + extra + "\r\n" + data);
        socket->disconnectFromHost();
    }
    QByteArray authType = "header";
    QByteArray publicPem;
    QByteArray token = "synthetic-api-token";
    QByteArray payload = QByteArray("RIFF\0synthetic-audio\xff", 22);
    QString failure;
    int requests = 0;
    bool rejectDownload = false;
    bool jsonDownloadError = false;
    std::function<void()> downloadHook;
    QUrl redirect;
  private:
    SecKeyRef privateKey = nullptr;
    void handle(QTcpSocket *socket, const QUrl &url,
                const QMap<QByteArray,QByteArray> &headers, const QByteArray &body) {
        ++requests;
        const auto path = url.path();
        if (path.endsWith("/verify/check")) {
            auto input = QJsonDocument::fromJson(body).object();
            if (input.value("username") != "test-user") failure = "check username missing";
            reply(socket, R"({"code":200,"data":{}})", "application/json",
                  "X-Rsa-Token: " + publicPem.toBase64() + "\r\n");
            return;
        }
        if (path.endsWith("/verify/login")) {
            const auto input = QJsonDocument::fromJson(body).object();
            if (decryptRsa(QByteArray::fromBase64(input.value("password").toString().toLatin1()))
                != "test-password") failure = "password RSA mismatch";
            if (headers.value("ug-client-id").isEmpty()) failure = "client id missing";
            reply(socket, QJsonDocument(QJsonObject{{"code",200},{"data", QJsonObject{
                  {"token",QString::fromLatin1(token)}, {"auth_type",QString::fromLatin1(authType)},
                  {"public_key",QString::fromLatin1(publicPem.toBase64())}}}}).toJson(QJsonDocument::Compact));
            return;
        }
        if (path.endsWith("/downloadFile")) {
            if (downloadHook) downloadHook();
            if (QUrlQuery(url).queryItemValue("token") != "synthetic-download-token"
                || headers.contains("x-ugreen-token")) failure = "download auth mismatch";
            if (jsonDownloadError) {
                reply(socket,R"({"code":1303,"msg":"synthetic-api-token server error"})");
                return;
            }
            reply(socket, rejectDownload ? QByteArray("failure") : payload,
                  "application/octet-stream", {}, rejectDownload ? 403 : 200);
            return;
        }
        const auto key = decryptRsa(QByteArray::fromBase64(headers.value("x-ugreen-security-code")));
        const auto encodedQuery = url.query(QUrl::FullyEncoded).section('=',1).toLatin1();
        if (encodedQuery.contains('+') || encodedQuery.contains('/') || encodedQuery.contains('='))
            failure = "outer Base64 query must percent-encode reserved characters";
        if (key.size() != 32) failure = "AES key must have 32 ASCII bytes";
        const auto inner = UgosCrypto::open(key, QByteArray::fromBase64(
                   QUrlQuery(url).queryItemValue("encrypt_query",QUrl::FullyDecoded).toLatin1()));
        const auto query = QUrlQuery(QString::fromUtf8(inner));
        const auto apiToken = authType == "url" ? query.queryItemValue("token").toUtf8()
            : decryptRsa(QByteArray::fromBase64(headers.value("x-ugreen-token")));
        if (apiToken != token) failure = "API token mismatch";
        if (headers.value("x-ugreen-security-key")
            != QCryptographicHash::hash(token,QCryptographicHash::Md5).toHex())
            failure = "security MD5 mismatch";
        QJsonObject input;
        if (!body.isEmpty()) {
            const auto outer = QJsonDocument::fromJson(body).object();
            const auto plain = UgosCrypto::open(key,QByteArray::fromBase64(
                                  outer.value("encrypt_req_body").toString().toLatin1()));
            if (outer.value("req_body_sha256").toString().toLatin1()
                != QCryptographicHash::hash(plain,QCryptographicHash::Sha256).toHex())
                failure = "body hash mismatch";
            input = QJsonDocument::fromJson(plain).object();
        }
        if (path.endsWith("/stall")) return;
        if (path.endsWith("/oversize")) {
            reply(socket,QByteArray(8*1024*1024+1,'x'));
            return;
        }
        if (path.endsWith("/redirect")) {
            reply(socket, {}, "application/json", "Location: " + redirect.toEncoded() + "\r\n", 302);
            return;
        }
        QJsonObject data{{"received",input},{"query",query.queryItemValue("name",QUrl::FullyDecoded)}};
        if (path.endsWith("/addPathsTask")) {
            if (input.value("paths").toArray().isEmpty()) failure = "download paths missing";
            data = QJsonObject{{"result","task-test"}};
        } else if (path.endsWith("/getDownloadToken")) {
            if (query.queryItemValue("task_uuid") != "task-test"
                || query.queryItemValue("coding") != "true") failure = "download query mismatch";
            data = QJsonObject{{"dl_token","synthetic-download-token"}};
        }
        auto encrypted = UgosCrypto::seal(key, QJsonDocument(QJsonObject{
                             {"code",200},{"data",data}}).toJson(QJsonDocument::Compact));
        if (path.endsWith("/tampered") && !encrypted.isEmpty()) encrypted[encrypted.size()-1] ^= 1;
        reply(socket,QJsonDocument(QJsonObject{{"encrypt_resp_body",
              QString::fromLatin1(encrypted.toBase64())}}).toJson(QJsonDocument::Compact));
    }
};
}

class UgosClientTests : public QObject {
    Q_OBJECT
  private slots:
    void aes256GcmMatchesNistAndRejectsTampering() {
        // NIST SP800-38D: AES-256, all-zero key/IV/plaintext, no AAD.
        const QByteArray key(32,'\0'), nonce(12,'\0'), plain(16,'\0');
        const auto expected = QByteArray::fromHex(
            "000000000000000000000000cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919");
        QCOMPARE(UgosCrypto::seal(key,plain,nonce),expected);
        QCOMPARE(UgosCrypto::open(key,expected),plain);
        auto tampered = expected;
        tampered[tampered.size()-1] ^= 1;
        QVERIFY(UgosCrypto::open(key,tampered).isNull());
        QVERIFY(UgosCrypto::seal(QByteArray(16,'a'),plain,nonce).isNull());
    }
    void validatesLocalOriginsWithoutEmbeddedCredentials() {
        QVERIFY(UgosClient::validateEndpoint(QUrl("http://192.168.1.2:9999")));
        QVERIFY(UgosClient::validateEndpoint(QUrl("https://nas.local:9443/")));
        QVERIFY(UgosClient::validateEndpoint(QUrl("http://[fd00::1]:9999")));
        for (const auto &url : {"http://user:pass@192.168.1.2", "http://8.8.8.8",
                                "file:///tmp/x", "http://127.0.0.1/path",
                                "http://127.0.0.1/?token=x", "http://127.0.0.1/#x"})
            QVERIFY2(!UgosClient::validateEndpoint(QUrl(url)),url);
    }
    void rsaSupportsBothPemContainersAndRejectsInvalidInput() {
        FakeNas nas;
        const QByteArray plain("synthetic RSA plaintext");
        for (const auto &pem : {nas.publicPem,nas.spkiPem()}) {
            const auto encrypted = UgosCrypto::rsaEncrypt(pem,plain);
            QCOMPARE(encrypted.size(),256);
            QCOMPARE(nas.decryptRsa(encrypted),plain);
        }
        QVERIFY(UgosCrypto::rsaEncrypt("invalid PEM",plain).isNull());
    }
    void passwordAndEncryptedApiInteroperate_data() {
        QTest::addColumn<QByteArray>("auth");
        QTest::newRow("header") << QByteArray("header");
        QTest::newRow("url") << QByteArray("url");
    }
    void passwordAndEncryptedApiInteroperate() {
        QFETCH(QByteArray,auth);
        FakeNas nas;
        nas.authType = auth;
        UgosClient client;
        QString error;
        QVERIFY2(client.login(nas.url(),"test-user","test-password",&error),qPrintable(error));
        QUrlQuery query;
        query.addQueryItem("name",QString::fromUtf8("测试 +/%"));
        const QJsonObject body{{"path",QString::fromUtf8("/共享/音效.wav")},{"type",false}};
        auto result = client.call("POST","v2/filemgr/echo",body,query,&error);
        QVERIFY2(error.isEmpty(),qPrintable(error));
        QCOMPARE(result.value("data").toObject().value("received").toObject(),body);
        QCOMPARE(result.value("data").toObject().value("query").toString(),QString::fromUtf8("测试 +/%"));
        QVERIFY2(nas.failure.isEmpty(),qPrintable(nas.failure));
        QVERIFY(client.call("GET","v2/tampered",{}, {},&error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY2(nas.failure.isEmpty(),qPrintable(nas.failure));
    }
    void downloadStreamsHashesAndNeverOverwrites() {
        FakeNas nas;
        nas.payload = QByteArray(9*1024*1024,'a');
        nas.payload.append("tail");
        UgosClient client;
        QString error;
        QVERIFY2(client.login(nas.url(),"test-user","test-password",&error),qPrintable(error));
        QTemporaryDir dir;
        const auto file = dir.filePath("owned.partial");
        QByteArray hash;
        QVERIFY2(client.download("/share/test.wav",file,&hash,&error),qPrintable(error));
        QCOMPARE(hash,QCryptographicHash::hash(nas.payload,QCryptographicHash::Sha256));
        QFile actual(file);
        QVERIFY(actual.open(QIODevice::ReadOnly));
        QCOMPARE(actual.readAll(),nas.payload);
        actual.close();
        const int before = nas.requests;
        QVERIFY(!client.download("/share/test.wav",file,&hash,&error));
        QCOMPARE(nas.requests,before);
        QVERIFY(actual.open(QIODevice::ReadOnly));
        QCOMPARE(actual.readAll(),nas.payload);
        nas.rejectDownload = true;
        const auto failed = dir.filePath("failed.partial");
        QVERIFY(!client.download("/share/test.wav",failed,&hash,&error));
        QVERIFY(!QFile::exists(failed));
        nas.rejectDownload = false;
        nas.jsonDownloadError = true;
        QVERIFY(!client.download("/share/test.wav",failed,&hash,&error));
        QVERIFY(!QFile::exists(failed));
        QVERIFY(!error.contains("synthetic"));
        QVERIFY2(nas.failure.isEmpty(),qPrintable(nas.failure));
    }
    void failedDownloadPreservesAReplacementAtThePartialPath() {
        FakeNas nas;
        UgosClient client;
        QString error;
        QVERIFY2(client.login(nas.url(),"test-user","test-password",&error),qPrintable(error));
        QTemporaryDir dir;
        const auto file = dir.filePath("partial");
        nas.rejectDownload = true;
        nas.downloadHook = [&] {
            QFile::remove(file);
            QFile replacement(file);
            if (replacement.open(QIODevice::WriteOnly|QIODevice::NewOnly)) replacement.write("preserve me");
        };
        QByteArray hash;
        QVERIFY(!client.download("/share/test.wav",file,&hash,&error));
        QFile actual(file);
        QVERIFY(actual.open(QIODevice::ReadOnly));
        QCOMPARE(actual.readAll(),QByteArray("preserve me"));
    }
    void destinationWriteFailureIsTypedAndResetsForOtherFailures_data() {
        QTest::addColumn<int>("payloadSize");
        QTest::addColumn<qulonglong>("limitBytes");
        QTest::newRow("streaming-write") << 9*1024*1024 << qulonglong(1024);
        QTest::newRow("buffered-flush") << 128 << qulonglong(0);
    }
    void destinationWriteFailureIsTypedAndResetsForOtherFailures() {
        QFETCH(int,payloadSize);
        QFETCH(qulonglong,limitBytes);
        FakeNas nas;
        nas.payload = QByteArray(payloadSize,'a');
        UgosClient client;
        QString error;
        QVERIFY2(client.login(nas.url(),"test-user","test-password",&error),qPrintable(error));
        QTemporaryDir dir;
        const auto file = dir.filePath("write-error.partial");
        QByteArray hash;
        {
            ScopedFileSizeLimit limit(limitBytes);
            QVERIFY(limit.active());
            QVERIFY(!client.download("/share/test.wav",file,&hash,&error));
            QVERIFY(client.lastDownloadStorageFailure());
        }
        QVERIFY(!QFile::exists(file));
        nas.rejectDownload = true;
        QVERIFY(!client.download("/share/test.wav",file,&hash,&error));
        QVERIFY(!client.lastDownloadStorageFailure());
        QVERIFY(!QFile::exists(file));
    }
    void rejectsOversizeApiResponseWithoutReturningServerText() {
        FakeNas nas;
        UgosClient client;
        QString error;
        QVERIFY2(client.login(nas.url(),"test-user","test-password",&error),qPrintable(error));
        QVERIFY(client.call("GET","v2/oversize",{}, {},&error).isEmpty());
        QVERIFY(error.contains("8 MiB"));
    }
    void redirectsAndTimeoutDoNotLeakAuthentication() {
        FakeNas nas, foreign;
        nas.redirect = foreign.url();
        UgosClient client;
        QString error;
        QVERIFY2(client.login(nas.url(),"test-user","test-password",&error),qPrintable(error));
        QVERIFY(client.call("GET","v2/redirect",{}, {},&error).isEmpty());
        QVERIFY(!error.isEmpty());
        QCOMPARE(foreign.requests,0);
        client.setTimeoutMs(100);
        QElapsedTimer timer;
        timer.start();
        QVERIFY(client.call("GET","v2/stall",{}, {},&error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(timer.elapsed()<2000);
        QVERIFY(!error.contains("synthetic"));
    }
};
QTEST_GUILESS_MAIN(UgosClientTests)
#include "ugos_client_tests.moc"
