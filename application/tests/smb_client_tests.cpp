#include "smb_client.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTemporaryDir>
#include <cstdint>
#include <ctime>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#ifndef SMB_CLIENT_NATIVE_PROBE
#include <QProcess>
#include <QtTest>
#include <sys/stat.h>

class SmbClientTests : public QObject {
    Q_OBJECT
private slots:
    void exactNamesAndShareBounds() {
        QString error;
        const QString source = QString::fromUtf8("/公司/音效/か\u3099.wav");
        auto relative = SmbClient::relativePath(source,QString::fromUtf8("公司"),false,&error);
        QVERIFY(relative.has_value());
        QCOMPARE(*relative,QString::fromUtf8("音效/か\u3099.wav").toUtf8());
        QVERIFY(*relative != QString::fromUtf8("音效/が.wav").toUtf8());
        QVERIFY(SmbClient::relativePath("/share","share",true).has_value());
        for (const auto &path : {"/share","/share/../x","/share/a/./b","/share//x",
                                 "/share2/x","/other/x","//share/x","share/x",
                                 "/share/a\\b","/share/a:stream","/share/x/"})
            QVERIFY2(!SmbClient::relativePath(path,"share",false,&error),path);
        QVERIFY(!SmbClient::relativePath(QString("/share/a")+QChar::Null+"b","share",false));
        QCOMPARE(*SmbClient::relativePath("/share/%2e%2e","share",false),QByteArray("%2e%2e"));
    }
    void validatesPrivateServerAndConvertsHttpPortToSmb() {
        QVERIFY(SmbClient::endpoint("http://192.168.1.2:9999").has_value());
        QCOMPARE(SmbClient::endpoint("http://192.168.1.2:9999")->port(),445);
        QCOMPARE(SmbClient::endpoint("127.0.0.1:1445")->port(),1445);
        QCOMPARE(SmbClient::endpoint("smb://nas.local:445")->host(),QString("nas.local"));
        for (const auto &host : {"http://u:p@192.168.1.2","8.8.8.8",
                                 "http://127.0.0.1/path","http://127.0.0.1/?x=y"})
            QVERIFY2(!SmbClient::endpoint(host),host);
    }
    void refusedLoopbackIsBoundedAndSanitized() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const auto port = server.serverPort();
        server.close();
        SmbClient client;
        client.setTimeoutMs(150);
        QString error;
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(!client.login("127.0.0.1:"+QString::number(port),"share",
                              "synthetic-user","synthetic-secret",&error));
        QVERIFY(elapsed.elapsed()<2500);
        QVERIFY(!error.isEmpty());
        QVERIFY(!error.contains("synthetic"));
        QVERIFY(!error.contains("127.0.0.1"));
    }
    void connectedPeerThatNeverAnswersIsBounded() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        SmbClient client;
        client.setTimeoutMs(150);
        QString error;
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(!client.login("127.0.0.1:"+QString::number(server.serverPort()),"share",
                              "synthetic-user","synthetic-secret",&error));
        QVERIFY(elapsed.elapsed()<2500);
        QVERIFY(!error.contains("synthetic"));
        QVERIFY(!error.contains("127.0.0.1"));
    }
    void invalidSourceAndExistingDestinationNeverChangeFiles() {
        QTemporaryDir dir;
        const auto path = dir.filePath("existing.partial");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("preserve");
        file.close();
        SmbClient client;
        QString error;
        QByteArray hash;
        QVERIFY(!client.download("/share/a",path,&hash,&error));
        QVERIFY(!client.lastDownloadStorageFailure());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(),QByteArray("preserve"));
        QVERIFY(!client.createHardLink("/share/../source","/share/alias",&error));
        QVERIFY(!client.stat("/other/path",&error));
    }
    void unknownInodeReadContractStillRejectsVersionChanges() {
        NasRepairFile a; a.size=10; a.modified=1000; a.fileId=0;
        auto b=a;
        QVERIFY(SmbClient::readVersionMatches(a,b));
        b.size++; QVERIFY(!SmbClient::readVersionMatches(a,b)); b=a;
        b.modified++; QVERIFY(!SmbClient::readVersionMatches(a,b)); b=a;
        b.fileId=123; QVERIFY(!SmbClient::readVersionMatches(a,b));
        a.fileId=123; b=a; b.fileId=456; QVERIFY(!SmbClient::readVersionMatches(a,b));
    }
    void localCreateFailureIsStorageButExistingDestinationIsPreserved() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        const auto port = server.serverPort(); server.close();
        SmbClient client; client.setTimeoutMs(100); QString error; QByteArray sha;
        QVERIFY(!client.login("127.0.0.1:"+QString::number(port),"share","synthetic","synthetic",&error));
        QTemporaryDir dir;
        QVERIFY(!client.download("/share/source",dir.filePath("missing-parent/file.partial"),&sha,&error));
        QVERIFY(client.lastDownloadStorageFailure());
        QFile keep(dir.filePath("existing.partial")); QVERIFY(keep.open(QIODevice::WriteOnly));
        keep.write("keep"); keep.close();
        QVERIFY(!client.download("/share/source",keep.fileName(),&sha,&error));
        QVERIFY(!client.lastDownloadStorageFailure());
        QVERIFY(keep.open(QIODevice::ReadOnly)); QCOMPARE(keep.readAll(),QByteArray("keep"));
    }
    void ambientCredentialFileIsNeverOpened() {
        QTemporaryDir dir;
        const auto fifo = QFile::encodeName(dir.filePath("synthetic-credential-fifo"));
        QVERIFY(::mkfifo(fifo.constData(),0600)==0);
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),{"--credential-file-guard",QString::fromUtf8(fifo)});
        const bool done = child.waitForFinished(2000);
        if (!done) { child.kill(); child.waitForFinished(); }
        QVERIFY2(done,"libsmb2 attempted to open the ambient credential FIFO");
        QCOMPARE(child.exitCode(),0);
    }
};

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    if (app.arguments().value(1)=="--credential-file-guard") {
        qputenv("NTLM_USER_FILE",app.arguments().value(2).toUtf8());
        auto *ctx=smb2_init_context();
        if (!ctx) return 2;
        smb2_set_user(ctx,"synthetic-user");
        smb2_set_domain(ctx,"synthetic-domain");
        smb2_destroy_context(ctx);
        return 0;
    }
    SmbClientTests tests;
    return QTest::qExec(&tests,argc,argv);
}
#include "smb_client_tests.moc"
#else
class ProbeRepairBackend final : public PathRepairBackend {
public:
    ProbeRepairBackend(SmbClient &client,bool local):client_(client),local_(local){}
    bool storageFailure() const override { return client_.lastDownloadStorageFailure(); }
    bool sessionUnavailable() const override { return client_.sessionUnavailable(); }
    QList<NasRepairFile> list(const QString &path,QString *error) override {
        return client_.list(path,error);
    }
    std::optional<NasRepairFile> stat(const QString &path,QString *error) override {
        return client_.stat(path,error);
    }
    bool createHardLink(const QString &source,const QString &destination,QString *error) override {
        if(local_) { if(error) *error=QStringLiteral("QA 指定验证本地缓存回退。"); return false; }
        return client_.createHardLink(source,destination,error);
    }
    bool download(const QString &source,const QString &destination,QByteArray *hash,QString *error) override {
        return client_.download(source,destination,hash,error);
    }
private:
    SmbClient &client_;
    bool local_;
};
static bool repairProbe(SmbClient &client,const QJsonObject &request,QJsonObject &out,QString *error) {
    PathRepairOptions options;
    options.indexPath=request.value("index_path").toString();
    options.mountRoot=request.value("mount_root").toString();
    options.serverRoot=request.value("server_root").toString();
    options.outputRoot=request.value("output_root").toString();
    options.localCacheRoot=request.value("local_cache_root").toString();
    options.checkpointPath=request.value("checkpoint_path").toString();
    options.preferHardLinks=true;
    const auto endpoint=SmbClient::endpoint(request.value("host").toString());
    if(!endpoint) { *error=QStringLiteral("QA SMB 地址无效。"); return false; }
    options.serverIdentity="smb:"+endpoint->host()+':'+QString::number(endpoint->port(445))
                           +'/'+request.value("share").toString();
    const auto libraryId=request.value("library_id").toString();
    QList<Track> tracks;
    if(libraryId.isEmpty() || options.indexPath.isEmpty()) {
        *error=QStringLiteral("QA 索引或资料库标识为空。"); return false;
    }
    {
        Index index(options.indexPath);
        if(!index.open(error)) { *error=QStringLiteral("QA 索引无法打开。"); return false; }
        for(const auto &library:index.libraries())
            if(library.id==libraryId) options.libraries.append(library);
        if(options.libraries.size()!=1) { *error=QStringLiteral("QA 资料库标识没有唯一匹配。"); return false; }
        const auto page=index.search({},libraryId,{},false,0,10000);
        if(!index.lastError().isEmpty() || page.more || page.tracks.isEmpty()) {
            *error=QStringLiteral("QA 资料库为空、过大或查询失败。"); return false;
        }
        tracks=page.tracks;
    }
    ProbeRepairBackend backend(client,request.value("force_local").toBool());
    std::atomic_bool pause{false};
    const auto result=runPathRepair(options,tracks,backend,&pause,{});
    out["succeeded"]=result.succeeded;
    out["failed"]=result.failed;
    out["done"]=result.done;
    out["total"]=result.total;
    out["hardlinked"]=result.hardlinked;
    out["cached"]=result.cached;
    out["reused"]=result.reused;
    out["paused"]=result.paused;
    *error=result.error.isEmpty()?result.lastError:result.error;
    error->replace(QRegularExpression("https?://[^\\s]+"),"[NAS]");
    return !result.paused && result.failed==0 && result.succeeded==tracks.size() && result.error.isEmpty();
}
// Optional QA executable only: bounded JSON from stdin, never argv/files.
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    QFile input;
    if(!input.open(stdin,QIODevice::ReadOnly)) return 2;
    auto bytes=input.read(32769);
    QJsonObject out;
    auto request=bytes.size()<=32768?QJsonDocument::fromJson(bytes).object():QJsonObject();
    bytes.fill('\0');
    QString error;
    SmbClient client;
    client.setTimeoutMs(request.value("timeout_ms").toInt(15000));
    const bool authenticated=request.size()>0 && client.login(request.value("host").toString(),
        request.value("share").toString(),request.value("username").toString(),
        request.value("password").toString(),&error);
    request.remove("password");
    request.remove("username");
    bool okay=authenticated;
    if (okay && request.value("repair").toBool()) {
        okay=repairProbe(client,request,out,&error);
    } else if (okay && request.value("create_link").toBool())
        okay=client.createHardLink(request.value("path").toString(),request.value("alias").toString(),&error);
    if (request.value("repair").toBool()) {
        // The actual service has already supplied its counts and diagnostics.
    } else if (okay && request.value("list").toBool()) {
        const auto files=client.list(request.value("path").toString(),&error);
        QJsonArray names;
        for(const auto &file:files) names.append(QJsonObject{{"name",file.name},{"path",file.path},
            {"size",double(file.size)},{"file_id",QString::number(file.fileId)},{"links",int(file.links)}});
        out["entries"]=names;
        okay=error.isEmpty();
    } else if (okay && !request.value("download_to").toString().isEmpty()) {
        QByteArray hash;
        okay=client.download(request.value("path").toString(),request.value("download_to").toString(),&hash,&error);
        out["sha256"]=QString::fromLatin1(hash.toHex());
        out["storage_failure"]=client.lastDownloadStorageFailure();
    } else if (okay) {
        auto file=client.stat(request.value("create_link").toBool()
            ? request.value("alias").toString() : request.value("path").toString(),&error);
        okay=file.has_value();
        if (file) {
            out["file_id"]=QString::number(file->fileId);
            out["links"]=int(file->links);
            out["size"]=double(file->size);
            out["modified"]=double(file->modified);
        }
    }
    out["ok"]=okay;
    out["error"]=error;
    out["nt_status"]=QString::number(client.lastNtStatus(),16);
    QFile output;
    if(!output.open(stdout,QIODevice::WriteOnly)) return 2;
    output.write(QJsonDocument(out).toJson(QJsonDocument::Compact)+"\n");
    return okay?0:1;
}
#endif
