#include "source_repair_coordinator.h"
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QtTest>
#include <atomic>
#include <sys/stat.h>
#include <unistd.h>
#include <QThread>
#include <QElapsedTimer>

struct Counts {
    std::atomic_int factories{0}, logins{0}, links{0}, downloads{0};
    std::atomic_bool blockLogin{false}, releaseLogin{false}, rejectLogin{false};
};
class LocalConnection : public SourceRepairConnection {
public:
    QString mount; std::shared_ptr<Counts> counts;
    LocalConnection(QString path,std::shared_ptr<Counts> c):mount(std::move(path)),counts(std::move(c)){}
    bool login(const QString&,const QString&,const QString&,const QString&,QString *e) override {
        e->clear();++counts->logins;
        QElapsedTimer timeout; timeout.start();
        while (counts->blockLogin && !counts->releaseLogin && timeout.elapsed() < 1500) QThread::msleep(2);
        if (counts->rejectLogin) { *e="synthetic authentication failure"; return false; }
        return true;
    }
    QString local(QString p)const {return p=="/share"?mount:QDir(mount).filePath(p.mid(7));}
    QList<NasRepairFile> list(const QString&p,QString*e)override {
        e->clear(); QList<NasRepairFile> result;
        for(const auto &f:QDir(local(p)).entryInfoList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot))
            result.append({p+'/'+f.fileName(),f.fileName(),f.size(),f.lastModified().toMSecsSinceEpoch(),f.isDir()});
        return result;
    }
    std::optional<NasRepairFile> stat(const QString&p,QString*e)override {
        e->clear();struct ::stat s{};const auto path=local(p);
        if(::stat(QFile::encodeName(path).constData(),&s)!=0){*e="missing";return {};}
        QFileInfo f(path);return NasRepairFile{p,f.fileName(),f.size(),f.lastModified().toMSecsSinceEpoch(),f.isDir(),quint64(s.st_ino),quint32(s.st_nlink)};
    }
    bool createHardLink(const QString&a,const QString&b,QString*e)override{
        ++counts->links;if(::link(QFile::encodeName(local(a)).constData(),QFile::encodeName(local(b)).constData())==0)return true;
        *e="link failed";return false;
    }
    bool download(const QString&p,const QString&to,QByteArray*sha,QString*e)override{
        ++counts->downloads;QFile in(local(p)),out(to);
        if(!in.open(QIODevice::ReadOnly)||!out.open(QIODevice::WriteOnly|QIODevice::NewOnly)){*e="copy failed";return false;}
        auto bytes=in.readAll();out.write(bytes);out.close();*sha=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);return true;
    }
};
class SourceRepairCoordinatorTests:public QObject {
    Q_OBJECT
    struct Fixture {
        QTemporaryDir tmp;
        QString profile,nas;
        SourceRepairSession session;
        QList<Track> tracks;
        std::shared_ptr<Counts> counts=std::make_shared<Counts>();
        Fixture(){
            const auto root=QFileInfo(tmp.path()).canonicalFilePath();profile=root+"/profile";nas=root+"/nas";
            QDir().mkpath(profile);QDir().mkpath(nas+"/originals");
            Index index(profile+"/library.sqlite");index.open();Library lib{"qa","QA",nas+"/originals",""};index.saveLibrary(lib);
            for(const auto &name:{"a.wav","b.wav"}){QFile f(nas+"/originals/"+name);f.open(QIODevice::WriteOnly);f.write("abc");f.close();
                Track t;t.libraryId="qa";t.root=lib.root;t.relativePath=t.name=name;t.folder=".";t.size=3;t.modified=QFileInfo(f).lastModified().toMSecsSinceEpoch();index.upsert({t});tracks<<*index.trackByIdentity("qa",name);}
            session.host="127.0.0.1";session.share="share";session.options={nas,"/share",nas+"/compatible",{},profile+"/library.sqlite",{lib},true,profile+"/audio-cache","127.0.0.1:445/share"};
        }
        SourceRepairCoordinator::Factory factory(){return [m=nas,c=counts]{++c->factories;return std::make_unique<LocalConnection>(m,c);};}
        void credentials(){session.account="synthetic-coordinator-account";session.password="synthetic-coordinator-private-secret";}
    };
private slots:
    void absentCredentialsPausesBeforeCreatingFiles(){
        Fixture f;SourceRepairCoordinator c(f.profile,f.profile+"/library.sqlite",nullptr,f.factory());c.configure(f.session);
        bool done=false;SourceRepairReply reply;c.repair(f.tracks[0],[&](auto r){reply=r;done=true;});
        QTRY_VERIFY(done);QVERIFY(reply.paused);QVERIFY(reply.needsConnection);QCOMPARE(f.counts->factories.load(),0);
        QVERIFY(!QFileInfo::exists(f.session.options.outputRoot));
    }
    void sameTrackCoalescesAndSessionIsReusedWithoutSecretsOnDisk(){
        Fixture f;f.credentials();SourceRepairCoordinator c(f.profile,f.profile+"/library.sqlite",nullptr,f.factory());c.configure(f.session);
        int done=0;QList<SourceRepairReply> replies;
        for(int n=0;n<2;++n)c.repair(f.tracks[0],[&](auto r){replies<<r;++done;});
        QTRY_COMPARE(done,2);QCOMPARE(replies[0].mapped.id,f.tracks[0].id);QCOMPARE(replies[1].mapped.id,f.tracks[0].id);
        QCOMPARE(f.counts->links.load(),1);QCOMPARE(f.counts->logins.load(),1);
        c.repair(f.tracks[1],[&](auto r){replies<<r;++done;});QTRY_COMPARE(done,3);QVERIFY(replies[2].mapped.id);
        QCOMPARE(f.counts->factories.load(),1);QCOMPARE(f.counts->logins.load(),1);
        QDirIterator it(f.profile,QDir::Files,QDirIterator::Subdirectories);
        while(it.hasNext()){QFile file(it.next());QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();
            QVERIFY(!bytes.contains(f.session.password.toUtf8()));QVERIFY(!bytes.contains(f.session.account.toUtf8()));}
        QCOMPARE(QDir(f.nas).entryList(QDir::Dirs|QDir::NoDotAndDotDot).size(),2);
    }
    void explicitPauseDoesNotStartQueuedWork(){
        Fixture f;f.credentials();SourceRepairCoordinator c(f.profile,f.profile+"/library.sqlite",nullptr,f.factory());c.configure(f.session);c.setPaused(true);
        bool done=false;SourceRepairReply reply;c.repair(f.tracks[0],[&](auto r){reply=r;done=true;});QTRY_VERIFY(done);
        QVERIFY(reply.paused);QCOMPARE(f.counts->factories.load(),0);c.setPaused(false);
        c.repair(f.tracks[0],[&](auto r){reply=r;done=true;});done=false;QTRY_VERIFY(done);QVERIFY(reply.mapped.id);
    }
    void pauseResumeCancelsPreviouslyQueuedEpoch(){
        Fixture f;f.credentials();f.counts->blockLogin=true;
        SourceRepairCoordinator c(f.profile,f.profile+"/library.sqlite",nullptr,f.factory());c.configure(f.session);
        QList<SourceRepairReply> replies;
        c.repair(f.tracks[0],[&](auto r){replies<<r;});c.repair(f.tracks[1],[&](auto r){replies<<r;});
        QTRY_COMPARE(f.counts->logins.load(),1);c.setPaused(true);c.setPaused(false);f.counts->releaseLogin=true;
        QTRY_COMPARE(replies.size(),2);QVERIFY(replies[0].paused);QVERIFY(replies[1].paused);
        QCOMPARE(f.counts->links.load(),0);QVERIFY(!QFileInfo::exists(f.session.options.outputRoot));
        c.repair(f.tracks[1],[&](auto r){replies<<r;});QTRY_COMPARE(replies.size(),3);QVERIFY(replies[2].mapped.id);
    }
    void authenticationFailureIsPausedWithoutCreatingDirectory(){
        Fixture f;f.credentials();f.counts->rejectLogin=true;
        SourceRepairCoordinator c(f.profile,f.profile+"/library.sqlite",nullptr,f.factory());c.configure(f.session);
        bool done=false;SourceRepairReply reply;c.repair(f.tracks[0],[&](auto r){reply=r;done=true;});
        QTRY_VERIFY(done);QVERIFY(reply.paused);QVERIFY(reply.needsConnection);QVERIFY(!reply.error.isEmpty());
        QCOMPARE(f.counts->links.load(),0);QVERIFY(!QFileInfo::exists(f.session.options.outputRoot));
    }
    void staleCatalogVersionAndUnconfiguredLocalPathDoNotContactNas(){
        Fixture f;f.credentials();SourceRepairCoordinator c(f.profile,f.profile+"/library.sqlite",nullptr,f.factory());c.configure(f.session);
        auto stale=f.tracks[0];++stale.size;QList<SourceRepairReply> replies;
        c.repair(stale,[&](auto r){replies<<r;});QTRY_COMPARE(replies.size(),1);
        QVERIFY(!replies[0].paused);QVERIFY(!replies[0].mapped.id);QVERIFY(!replies[0].error.isEmpty());
        f.session.options.mountRoot="/unconfigured";c.configure(f.session);
        c.repair(f.tracks[1],[&](auto r){replies<<r;});QTRY_COMPARE(replies.size(),2);
        QVERIFY(!replies[1].paused);QVERIFY(!replies[1].needsConnection);QCOMPARE(f.counts->factories.load(),0);
    }
};
QTEST_GUILESS_MAIN(SourceRepairCoordinatorTests)
#include "source_repair_coordinator_tests.moc"
