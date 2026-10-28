#include "ugos_client.h"
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <limits>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#ifdef Q_OS_MACOS
extern "C" {
int ugos_random(unsigned char *, int);
int ugos_seal(const unsigned char *, int, const unsigned char *, int,
              const unsigned char *, int, unsigned char **, int *);
int ugos_open(const unsigned char *, int, const unsigned char *, int,
              unsigned char **, int *);
int ugos_rsa_encrypt(const unsigned char *, int, const unsigned char *, int,
                     unsigned char **, int *);
void ugos_release(unsigned char *, int);
}
#endif

namespace {
void fail(QString *error, const QString &message) { if (error) *error = message; }
void clearError(QString *error) { if (error) error->clear(); }
bool ownsPartialPath(QFile &file) {
#ifdef Q_OS_UNIX
    struct stat opened{}, current{};
    return file.isOpen() && ::fstat(file.handle(),&opened) == 0
        && ::lstat(QFile::encodeName(file.fileName()).constData(),&current) == 0
        && opened.st_dev == current.st_dev && opened.st_ino == current.st_ino;
#else
    return file.isOpen();
#endif
}
const unsigned char *bytes(const QByteArray &value) {
    return reinterpret_cast<const unsigned char *>(value.constData());
}
bool privateAddress(const QHostAddress &address) {
    bool isV4 = false;
    const auto ip = address.toIPv4Address(&isV4);
    if (isV4)
        return (ip & 0xff000000u) == 0x0a000000u
            || (ip & 0xfff00000u) == 0xac100000u
            || (ip & 0xffff0000u) == 0xc0a80000u
            || (ip & 0xff000000u) == 0x7f000000u
            || (ip & 0xffff0000u) == 0xa9fe0000u;
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        const auto ip6 = address.toIPv6Address();
        return address.isLoopback() || (ip6[0] & 0xfe) == 0xfc
            || (ip6[0] == 0xfe && (ip6[1] & 0xc0) == 0x80);
    }
    return false;
}
QByteArray queryBytes(const QUrlQuery &query) {
    QByteArray output;
    for (const auto &item : query.queryItems(QUrl::FullyDecoded)) {
        if (!output.isEmpty()) output += '&';
        output += QUrl::toPercentEncoding(item.first);
        output += '=';
        output += QUrl::toPercentEncoding(item.second);
    }
    return output;
}
QUrl apiUrl(QUrl endpoint, QString api) {
    if (api.startsWith('/')) api.remove(0,1);
    if (!api.startsWith("ugreen/")) api.prepend("ugreen/");
    endpoint.setPath('/' + api);
    endpoint.setQuery(QString());
    endpoint.setFragment({});
    return endpoint;
}
bool validApi(const QString &api) {
    return !api.isEmpty() && !api.contains(":") && !api.contains("..")
        && !api.contains('?') && !api.contains('#') && !api.contains('\\')
        && !api.contains(QChar::Null) && !api.startsWith("//");
}
QMap<QByteArray,QByteArray> baseHeaders(const QByteArray &clientId) {
    return {{"Content-Type","application/json"},{"UG-Agent","PC/WEB"},
            {"Client-Id",clientId},{"UG-Client-Id",clientId},
            {"Client-Version","78578"},{"Client-Version-Str","1.20.0"},
            {"User-Feature","Mac"},{"X-Specify-Language","zh-CN"},
            {"Cache-Control","no-cache"}};
}
struct HttpResult {
    QByteArray body;
    QMap<QByteArray,QByteArray> headers;
    bool ok = false;
    bool writeFailed = false;
};
HttpResult http(const QUrl &url, const QByteArray &method, const QByteArray &body,
                 const QMap<QByteArray,QByteArray> &headers, int timeout,
                 QString *error, QFile *destination = nullptr,
                 QCryptographicHash *digest = nullptr) {
    HttpResult result;
    QNetworkAccessManager manager;
    manager.setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(timeout);
    for (auto it=headers.cbegin(); it!=headers.cend(); ++it)
        request.setRawHeader(it.key(),it.value());
    auto reply = manager.sendCustomRequest(request,method,body);
    reply->setReadBufferSize(256*1024);
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool expired = false, writeFailed = false, tooLarge = false, tlsFailed = false;
    bool downloadError = false;
    QObject::connect(&deadline,&QTimer::timeout,&loop,[&] {
        expired = true;
        reply->abort();
        loop.quit();
    });
    QObject::connect(reply,&QNetworkReply::sslErrors,&loop,[&](const auto &) {
        tlsFailed = true;
        reply->abort();
    });
    auto drain = [&] {
        if (!reply->isOpen()) return;
        const auto chunk = reply->readAll();
        if (destination) {
            if (!chunk.isEmpty()) {
                if (reply->header(QNetworkRequest::ContentTypeHeader).toString()
                        .contains("application/json",Qt::CaseInsensitive)) {
                    downloadError = true;
                    reply->abort();
                    return;
                }
                const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status >= 200 && status < 300) {
                    if (destination->write(chunk) != chunk.size()) {
                        writeFailed = true;
                        reply->abort();
                    } else if (digest) digest->addData(chunk);
                }
            }
        } else if (result.body.size() + chunk.size() > 8*1024*1024) {
            tooLarge = true;
            reply->abort();
        } else result.body += chunk;
    };
    QObject::connect(reply,&QNetworkReply::readyRead,&loop,drain);
    QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
    deadline.start(timeout);
    if (!reply->isFinished()) loop.exec();
    deadline.stop();
    drain();
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.writeFailed = writeFailed;
    for (const auto &pair : reply->rawHeaderPairs())
        result.headers.insert(pair.first.toLower(),pair.second);
    // URLs, response bodies and errorString() can contain secrets supplied by
    // the server, so diagnostics only contain local messages and numeric codes.
    if (expired) fail(error,QStringLiteral("NAS 请求超时。"));
    else if (tlsFailed) fail(error,QStringLiteral("NAS TLS 证书验证失败。"));
    else if (writeFailed) fail(error,QStringLiteral("兼容副本写入失败。"));
    else if (downloadError) fail(error,QStringLiteral("NAS 下载接口返回错误响应。"));
    else if (tooLarge) fail(error,QStringLiteral("NAS 接口响应超过 8 MiB 限制。"));
    else if (status >= 300 && status < 400)
        fail(error,QStringLiteral("NAS 返回重定向，已停止请求。"));
    else if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
        fail(error,QStringLiteral("NAS 网络请求失败（HTTP %1）。").arg(status));
    else result.ok = true;
    delete reply;
    return result;
}
QJsonObject parseJson(const QByteArray &data, QString *error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data,&parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        fail(error,QStringLiteral("NAS 返回的 JSON 无效。"));
        return {};
    }
    return document.object();
}
bool successful(const QJsonObject &response, QString *error) {
    const auto code = response.value("code");
    if (!code.isDouble() || (code.toInt() != 200 && code.toInt() != 0)) {
        fail(error,QStringLiteral("NAS 接口未成功（代码 %1）。").arg(code.toInt(-1)));
        return false;
    }
    return true;
}
}
namespace UgosCrypto {
QByteArray randomBytes(int count) {
#ifdef Q_OS_MACOS
    if (count <= 0 || count > 1024*1024) return {};
    QByteArray result(count,'\0');
    if (ugos_random(reinterpret_cast<unsigned char *>(result.data()),count)) return result;
#else
    Q_UNUSED(count);
#endif
    return {};
}
QByteArray seal(const QByteArray &key, const QByteArray &plain, const QByteArray &nonce) {
#ifdef Q_OS_MACOS
    const auto iv = nonce.isEmpty() ? randomBytes(12) : nonce;
    unsigned char *output = nullptr;
    int count = 0;
    if (key.size() != 32 || iv.size() != 12 || plain.size() > std::numeric_limits<int>::max()) return {};
    if (!ugos_seal(bytes(key),key.size(),bytes(iv),iv.size(),bytes(plain),plain.size(),&output,&count)) return {};
    QByteArray result(reinterpret_cast<char *>(output),count);
    ugos_release(output,count);
    return result;
#else
    Q_UNUSED(key); Q_UNUSED(plain); Q_UNUSED(nonce);
    return {};
#endif
}
QByteArray open(const QByteArray &key, const QByteArray &combined) {
#ifdef Q_OS_MACOS
    unsigned char *output = nullptr;
    int count = 0;
    if (key.size() != 32 || combined.size() < 28 || combined.size() > std::numeric_limits<int>::max()) return {};
    if (!ugos_open(bytes(key),key.size(),bytes(combined),combined.size(),&output,&count)) return {};
    QByteArray result(reinterpret_cast<char *>(output),count);
    ugos_release(output,count);
    return result;
#else
    Q_UNUSED(key); Q_UNUSED(combined);
    return {};
#endif
}
QByteArray rsaEncrypt(const QByteArray &publicPem, const QByteArray &plain) {
#ifdef Q_OS_MACOS
    unsigned char *output = nullptr;
    int count = 0;
    if (publicPem.isEmpty() || publicPem.size() > 16384 || plain.size() > 8192) return {};
    if (!ugos_rsa_encrypt(bytes(publicPem),publicPem.size(),bytes(plain),plain.size(),&output,&count)) return {};
    QByteArray result(reinterpret_cast<char *>(output),count);
    ugos_release(output,count);
    return result;
#else
    Q_UNUSED(publicPem); Q_UNUSED(plain);
    return {};
#endif
}
}
UgosClient::UgosClient()
    : clientId_(QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1()) {}
UgosClient::~UgosClient() { clearSession(); }
void UgosClient::clearSession() {
    token_.fill('\0');
    token_.clear();
    publicKey_.clear();
    authType_.clear();
}
void UgosClient::setTimeoutMs(int milliseconds) {
    timeoutMs_ = std::clamp(milliseconds,100,180000);
}
bool UgosClient::validateEndpoint(const QUrl &endpoint, QString *error) {
    clearError(error);
    if (!endpoint.isValid() || (endpoint.scheme() != "http" && endpoint.scheme() != "https")
        || endpoint.host().isEmpty() || !endpoint.userInfo().isEmpty()
        || endpoint.hasQuery() || endpoint.hasFragment()
        || (!endpoint.path().isEmpty() && endpoint.path() != "/")
        || endpoint.port(1) < 1 || endpoint.port(1) > 65535) {
        fail(error,QStringLiteral("请输入不含账号、参数和路径的 NAS HTTP(S) 地址。"));
        return false;
    }
    const QHostAddress address(endpoint.host());
    if (!address.isNull()) {
        if (privateAddress(address)) return true;
        fail(error,QStringLiteral("NAS 地址必须位于本机或局域网。"));
        return false;
    }
    const auto host = QString::fromLatin1(QUrl::toAce(endpoint.host()));
    static const QRegularExpression hostname(
        R"(^(?=.{1,253}$)[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?\.?$)");
    if (!hostname.match(host).hasMatch() || host.contains("..")) {
        fail(error,QStringLiteral("NAS 主机名无效。"));
        return false;
    }
    return true;
}
bool UgosClient::resolveLocalEndpoint(QString *error) const {
    QHostAddress address(endpoint_.host());
    if (!address.isNull()) return privateAddress(address);
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool finished = false, allowed = false;
    const int lookup = QHostInfo::lookupHost(endpoint_.host(),&loop,[&](const QHostInfo &info) {
        finished = true;
        allowed = info.error() == QHostInfo::NoError && !info.addresses().isEmpty();
        for (const auto &entry : info.addresses()) allowed = allowed && privateAddress(entry);
        loop.quit();
    });
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    deadline.start(std::min(timeoutMs_,10000));
    if (!finished) loop.exec();
    QHostInfo::abortHostLookup(lookup);
    if (!finished || !allowed) fail(error,QStringLiteral("NAS 主机名未解析为局域网地址。"));
    return finished && allowed;
}
QJsonObject UgosClient::plainJson(QString method, QString api, QJsonObject body, QString *error) {
    auto response = http(apiUrl(endpoint_,api),method.toLatin1(),
                         QJsonDocument(body).toJson(QJsonDocument::Compact),
                         baseHeaders(clientId_),timeoutMs_,error);
    if (!response.ok) return {};
    return parseJson(response.body,error);
}
bool UgosClient::login(QUrl endpoint, QString username, QString password, QString *error) {
    clearError(error);
    clearSession();
    if (!validateEndpoint(endpoint,error)) return false;
    endpoint_ = endpoint;
    if (!resolveLocalEndpoint(error)) return false;
    if (username.isEmpty() || password.isEmpty() || username == "admin") {
        fail(error,QStringLiteral("请输入普通 NAS 用户账号和密码；初始化 admin 账号不受支持。"));
        return false;
    }
    auto check = http(apiUrl(endpoint_,"v1/verify/check"),"POST",
        QJsonDocument(QJsonObject{{"username",username}}).toJson(QJsonDocument::Compact),
        baseHeaders(clientId_),timeoutMs_,error);
    if (!check.ok) return false;
    const auto pem = QByteArray::fromBase64(check.headers.value("x-rsa-token"));
    auto passwordBytes = password.toUtf8();
    const auto encrypted = UgosCrypto::rsaEncrypt(pem,passwordBytes);
    passwordBytes.fill('\0');
    password.fill(QChar::Null);
    if (encrypted.isEmpty()) {
        fail(error,QStringLiteral("无法读取 NAS 登录加密公钥。"));
        return false;
    }
    auto response = plainJson("POST","v1/verify/login",QJsonObject{
        {"username",username},{"password",QString::fromLatin1(encrypted.toBase64())},
        {"keepalive",false},{"otp",true},{"is_simple",false}},error);
    if (response.isEmpty() || !successful(response,error)) return false;
    const auto data = response.value("data").toObject();
    if (data.value("enable_otp").toBool() && data.value("token").toString().isEmpty()) {
        fail(error,QStringLiteral("该 NAS 账号需要二次验证；请使用无需二次验证的专用读取账号。"));
        return false;
    }
    token_ = data.value("token").toString().toUtf8();
    publicKey_ = QByteArray::fromBase64(data.value("public_key").toString().toLatin1());
    authType_ = data.value("auth_type").toString("url");
    if (token_.isEmpty() || publicKey_.isEmpty()
        || UgosCrypto::rsaEncrypt(publicKey_,QByteArray("probe")).isEmpty()) {
        clearSession();
        fail(error,QStringLiteral("NAS 登录响应缺少有效会话或加密公钥。"));
        return false;
    }
    return true;
}
QJsonObject UgosClient::call(QString method, QString api, QJsonObject body,
                            QUrlQuery query, QString *error) {
    clearError(error);
    method = method.toUpper();
    if (token_.isEmpty() || !validApi(api)
        || !QStringList{"GET","POST","PUT","DELETE","PATCH"}.contains(method)) {
        fail(error,QStringLiteral("NAS 会话或接口参数无效。"));
        return {};
    }
    auto key = UgosCrypto::randomBytes(16).toHex();
    auto headers = baseHeaders(clientId_);
    headers.insert("X-Ugreen-Security-Key",QCryptographicHash::hash(token_,QCryptographicHash::Md5).toHex());
    if (authType_ == "url") {
        query.removeAllQueryItems("token");
        query.addQueryItem("token",QString::fromUtf8(token_));
    } else {
        const auto encryptedToken = UgosCrypto::rsaEncrypt(publicKey_,token_);
        if (encryptedToken.isEmpty()) {
            fail(error,QStringLiteral("NAS 会话加密失败。"));
            return {};
        }
        headers.insert("X-Ugreen-Token",encryptedToken.toBase64());
    }
    auto url = apiUrl(endpoint_,api);
    QByteArray requestBody;
    if (endpoint_.scheme() == "http") {
        const auto securityCode = UgosCrypto::rsaEncrypt(publicKey_,key);
        const auto encryptedQuery = UgosCrypto::seal(key,queryBytes(query));
        if (securityCode.isEmpty() || encryptedQuery.isEmpty()) {
            fail(error,QStringLiteral("NAS 请求加密失败。"));
            return {};
        }
        headers.insert("X-Ugreen-Security-Code",securityCode.toBase64());
        // QUrlQuery leaves '+' unescaped; UGOS uses form/qs parsing, where it
        // becomes a space. Encode the Base64 value explicitly, including '='.
        url.setQuery(QString::fromLatin1("encrypt_query=" +
                     QUrl::toPercentEncoding(QString::fromLatin1(encryptedQuery.toBase64()))));
        if (method != "GET") {
            const auto plain = QJsonDocument(body).toJson(QJsonDocument::Compact);
            const auto encryptedBody = UgosCrypto::seal(key,plain);
            if (encryptedBody.isEmpty()) {
                fail(error,QStringLiteral("NAS 请求加密失败。"));
                return {};
            }
            requestBody = QJsonDocument(QJsonObject{
                {"encrypt_req_body",QString::fromLatin1(encryptedBody.toBase64())},
                {"req_body_sha256",QString::fromLatin1(QCryptographicHash::hash(plain,QCryptographicHash::Sha256).toHex())}
            }).toJson(QJsonDocument::Compact);
        }
    } else {
        url.setQuery(QString::fromLatin1(queryBytes(query)));
        if (method != "GET") requestBody = QJsonDocument(body).toJson(QJsonDocument::Compact);
    }
    auto response = http(url,method.toLatin1(),requestBody,headers,timeoutMs_,error);
    if (!response.ok) { key.fill('\0'); return {}; }
    auto result = parseJson(response.body,error);
    if (result.contains("encrypt_resp_body")) {
        const auto plain = UgosCrypto::open(key,QByteArray::fromBase64(
                   result.value("encrypt_resp_body").toString().toLatin1()));
        if (plain.isNull()) {
            fail(error,QStringLiteral("NAS 响应验证或解密失败。"));
            key.fill('\0');
            return {};
        }
        result = parseJson(plain,error);
    }
    key.fill('\0');
    if (result.isEmpty() || !successful(result,error)) return {};
    return result;
}
bool UgosClient::download(QString remotePath, QString localTemp, QByteArray *sha256,
                          QString *error) {
    downloadStorageFailure_ = false;
    clearError(error);
    if (sha256) sha256->clear();
    if (token_.isEmpty() || !remotePath.startsWith('/') || remotePath.contains(QChar::Null)
        || localTemp.isEmpty()) {
        fail(error,QStringLiteral("NAS 下载参数或会话无效。"));
        return false;
    }
    QFile output(localTemp);
    if (!output.open(QIODevice::WriteOnly|QIODevice::NewOnly)) {
        fail(error,QStringLiteral("无法新建兼容副本临时文件，已有文件不会覆盖。"));
        return false;
    }
    auto discard = [&] {
        const bool owned = ownsPartialPath(output);
        output.close();
        if (owned) output.remove();
        return false;
    };
    auto task = call("POST","v2/filemgr/addPathsTask",QJsonObject{
        {"paths",QJsonArray{QJsonObject{{"path",remotePath},{"intranet_share_id",0}}}}},{},error);
    const auto taskId = task.value("data").toObject().value("result").toString();
    if (task.isEmpty()) return discard();
    if (taskId.isEmpty()) {
        fail(error,QStringLiteral("NAS 未返回下载任务。"));
        return discard();
    }
    QUrlQuery query;
    query.addQueryItem("task_uuid",taskId);
    query.addQueryItem("intranet_share_id","0");
    query.addQueryItem("coding","true");
    auto tokenResponse = call("GET","v2/filemgr/getDownloadToken",{},query,error);
    auto downloadToken = tokenResponse.value("data").toObject().value("dl_token").toString();
    if (tokenResponse.isEmpty()) return discard();
    if (downloadToken.isEmpty()) {
        fail(error,QStringLiteral("NAS 未返回下载凭据。"));
        return discard();
    }
    auto url = apiUrl(endpoint_,"v2/filemgr/downloadFile");
    url.setQuery(QString::fromLatin1("token=" + QUrl::toPercentEncoding(downloadToken)));
    downloadToken.fill(QChar::Null);
    QCryptographicHash digest(QCryptographicHash::Sha256);
    const auto response = http(url,"GET",{},{{"Cache-Control","no-cache"}},
                               timeoutMs_,error,&output,&digest);
    downloadStorageFailure_ = response.writeFailed;
    if (!response.ok) return discard();
    if (!output.flush()) {
        downloadStorageFailure_ = true;
        fail(error,QStringLiteral("兼容副本写入失败。"));
        return discard();
    }
    if (!ownsPartialPath(output)) {
        fail(error,QStringLiteral("兼容副本临时路径已改变，已停止下载。"));
        return discard();
    }
    output.close();
    if (sha256) *sha256 = digest.result();
    return true;
}
