#include "smb_client.h"
#include "ugos_client.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QTimer>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/stat.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

namespace {
void fail(QString *error,const QString &value) { if(error) *error=value; }
void clear(QString *error) { if(error) error->clear(); }
bool validComponent(const QString &part) {
    return !part.isEmpty() && part!="." && part!=".." && !part.contains('/')
        && !part.contains('\\') && !part.contains(':') && !part.contains(QChar::Null)
        && !part.contains('\r') && !part.contains('\n')
        && QString::fromUtf8(part.toUtf8())==part;
}
bool ownsPath(QFile &file) {
    struct stat opened{},current{};
    return file.isOpen() && ::fstat(file.handle(),&opened)==0
        && ::lstat(QFile::encodeName(file.fileName()).constData(),&current)==0
        && opened.st_dev==current.st_dev && opened.st_ino==current.st_ino;
}
bool sameVersion(const NasRepairFile &a,const NasRepairFile &b) {
    return !a.directory && !b.directory && a.fileId==b.fileId && a.size==b.size && a.modified==b.modified;
}
std::optional<NasRepairFile> entry(const QString &logical,const smb2_stat_64 &st) {
    if (st.smb2_size>quint64(std::numeric_limits<qint64>::max())
        || st.smb2_mtime>quint64(std::numeric_limits<qint64>::max()/1000)) return {};
    NasRepairFile result;
    result.path=logical;
    result.name=logical.section('/',-1);
    result.size=qint64(st.smb2_size);
    result.modified=qint64(st.smb2_mtime)*1000+st.smb2_mtime_nsec/1000000;
    result.directory=st.smb2_type==SMB2_TYPE_DIRECTORY;
    result.fileId=st.smb2_ino;
    result.links=st.smb2_nlink;
    return result;
}
QString resolvedPrivateHost(const QUrl &endpoint,int timeout,QString *error) {
    const auto host=endpoint.host();
    QHostAddress address(host);
    if(!address.isNull()) return host;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QHostInfo result;
    bool done=false;
    const int id=QHostInfo::lookupHost(host,&loop,[&](const QHostInfo &info) {
        result=info; done=true; loop.quit();
    });
    QObject::connect(&timer,&QTimer::timeout,&loop,&QEventLoop::quit);
    timer.start(timeout);
    if(!done) loop.exec();
    QHostInfo::abortHostLookup(id);
    if(!done || result.error()!=QHostInfo::NoError || result.addresses().isEmpty()) {
        fail(error,QStringLiteral("SMB 主机名解析失败或超时。")); return {};
    }
    QString selected;
    for(const auto &ip:result.addresses()) {
        QUrl local; local.setScheme("http"); local.setHost(ip.toString());
        if(!UgosClient::validateEndpoint(local)) {
            fail(error,QStringLiteral("SMB 主机名必须仅解析到局域网地址。")); return {};
        }
        if(selected.isEmpty() || ip.protocol()==QAbstractSocket::IPv4Protocol) selected=ip.toString();
    }
    return selected;
}
}

struct SmbClient::Impl {
    smb2_context *ctx=nullptr;
    QString share;
    QUrl origin;
    int timeout=35000;
    quint32 nt=0;
    bool storage=false;
    ~Impl() { disconnect(); }
    void disconnect() { if(ctx) { smb2_destroy_context(ctx); ctx=nullptr; } }
    using Start=std::function<int(smb2_command_cb,void*)>;
    int request(const Start &start,void **output,QString *error,int budget=-1) {
        clear(error); nt=0;
        if(!ctx) { fail(error,QStringLiteral("SMB 会话未连接。")); return -ENOTCONN; }
        smb2_set_error(ctx,""); // Do not reuse NTSTATUS from an earlier command.
        struct Completion { bool done=false; int status=-EIO; void *data=nullptr; } state;
        auto callback=[](smb2_context*,int status,void *data,void *opaque) {
            auto *s=static_cast<Completion*>(opaque);
            s->status=status; s->data=data; s->done=true;
        };
        const int initiated=start(callback,&state);
        if(initiated<0) {
            nt=quint32(smb2_get_nterror(ctx));
            fail(error,QStringLiteral("SMB 请求未能启动（代码 %1）。").arg(initiated));
            return initiated;
        }
        QElapsedTimer elapsed; elapsed.start();
        const int limit=budget<0?timeout:std::max(1,budget);
        while(!state.done) {
            if(elapsed.elapsed()>=limit) {
                disconnect();
                fail(error,QStringLiteral("SMB 请求超时。")); return -ETIMEDOUT;
            }
            size_t count=0; int connectTimeout=-1;
            const auto *fds=smb2_get_fds(ctx,&count,&connectTimeout);
            QList<pollfd> waiting;
            const auto events=short(smb2_which_events(ctx));
            for(size_t i=0;i<count;++i) waiting.append(pollfd{fds[i],events,0});
            int slice=std::min(100,limit-int(elapsed.elapsed()));
            if(connectTimeout>=0) slice=std::min(slice,connectTimeout);
            const int polled=::poll(waiting.data(),waiting.size(),std::max(0,slice));
            if(polled<0 && errno==EINTR) continue;
            bool failed=polled<0;
            if(!failed && polled==0) {
                // Service timeout PDUs even when a peer never sends data.
                failed=smb2_service(ctx,0)<0;
                if(!failed && connectTimeout>=0 && slice==connectTimeout)
                    failed=smb2_service_fd(ctx,-1,0)<0;
            }
            if(!failed && polled>0) {
                for(const auto &fd:waiting) {
                    if(fd.revents && smb2_service_fd(ctx,fd.fd,fd.revents)<0) { failed=true; break; }
                }
            }
            if(failed) {
                nt=quint32(smb2_get_nterror(ctx));
                disconnect();
                fail(error,QStringLiteral("SMB 网络连接中断。")); return -ENETDOWN;
            }
        }
        if(state.status<0) {
            nt=quint32(smb2_get_nterror(ctx));
            fail(error,QStringLiteral("SMB 请求失败（状态 0x%1）。")
                 .arg(nt,8,16,QChar('0')));
        } else if(output) *output=state.data;
        return state.status;
    }
    std::optional<smb2_stat_64> rawStat(const QByteArray &path,QString *error,int budget=-1) {
        smb2_stat_64 value{};
        const int result=request([&](auto cb,void *data) {
            return smb2_stat_async(ctx,path.constData(),&value,cb,data);
        },nullptr,error,budget);
        if(result<0) return {};
        return value;
    }
};

SmbClient::SmbClient():d_(std::make_unique<Impl>()){}
SmbClient::~SmbClient()=default;
bool SmbClient::readVersionMatches(const NasRepairFile &a,const NasRepairFile &b) { return sameVersion(a,b); }
bool SmbClient::sessionUnavailable()const { return !d_->ctx; }
bool SmbClient::lastDownloadStorageFailure()const { return d_->storage; }
quint32 SmbClient::lastNtStatus()const { return d_->nt; }
void SmbClient::setTimeoutMs(int value) { d_->timeout=std::clamp(value,100,180000); }
std::optional<QByteArray> SmbClient::relativePath(const QString &logical,const QString &share,
                                                bool root,QString *error) {
    clear(error);
    const QString prefix='/'+share;
    if(!validComponent(share) || (!logical.startsWith(prefix+'/') && logical!=prefix)) {
        fail(error,QStringLiteral("SMB 路径不属于所选共享。")); return {};
    }
    if(logical==prefix) {
        if(root) return QByteArray("");
        fail(error,QStringLiteral("SMB 文件路径不能是共享根目录。")); return {};
    }
    const auto relative=logical.mid(prefix.size()+1);
    for(const auto &part:relative.split('/')) if(!validComponent(part)) {
        fail(error,QStringLiteral("SMB 路径包含无效或越界分量。")); return {};
    }
    return relative.toUtf8();
}
std::optional<QUrl> SmbClient::endpoint(const QString &host,QString *error) {
    clear(error);
    QUrl result;
    if(host.contains("://")) {
        result=QUrl(host);
        if(result.scheme()!="http" && result.scheme()!="https" && result.scheme()!="smb") {
            fail(error,QStringLiteral("SMB 主机地址格式无效。")); return {};
        }
        const int port=result.scheme()=="smb"?result.port(445):445;
        result.setScheme("http"); result.setPort(port);
    } else {
        QHostAddress direct(host);
        if(!direct.isNull()) { result.setScheme("http"); result.setHost(host); result.setPort(445); }
        else { result=QUrl("http://"+host); result.setPort(result.port(445)); }
    }
    if(!UgosClient::validateEndpoint(result,error)) return {};
    return result;
}
bool SmbClient::login(QString host,QString share,QString username,QString password,QString *error) {
    clear(error); d_->disconnect(); d_->nt=0; d_->share.clear();
    const auto origin=endpoint(host,error);
    if(!origin) return false;
    if(!validComponent(share) || username.isEmpty() || password.isEmpty()
       || username.contains(QChar::Null) || password.contains(QChar::Null)) {
        fail(error,QStringLiteral("SMB 共享、账号或密码参数无效。")); return false;
    }
    const auto resolved=resolvedPrivateHost(*origin,d_->timeout,error);
    if(resolved.isEmpty()) return false;
    d_->origin=*origin; d_->share=share;
    d_->ctx=smb2_init_context();
    if(!d_->ctx) { fail(error,QStringLiteral("无法创建 SMB 会话。")); return false; }
    smb2_set_timeout(d_->ctx,(d_->timeout+999)/1000);
    smb2_set_version(d_->ctx,SMB2_VERSION_ANY3);
    smb2_set_sign(d_->ctx,1);
    smb2_set_security_mode(d_->ctx,SMB2_NEGOTIATE_SIGNING_ENABLED|SMB2_NEGOTIATE_SIGNING_REQUIRED);
    smb2_set_authentication(d_->ctx,SMB2_SEC_NTLMSSP);
    QString domain;
    if(username.contains('\\')) {
        domain=username.section('\\',0,0);
        username=username.section('\\',1);
        if(domain.isEmpty() || username.isEmpty() || username.contains('\\')) {
            d_->disconnect(); fail(error,QStringLiteral("SMB 域账号格式无效。")); return false;
        }
    }
    auto user=username.toUtf8(), secret=password.toUtf8(), domainBytes=domain.toUtf8();
    smb2_set_domain(d_->ctx,domainBytes.constData());
    smb2_set_user(d_->ctx,user.constData());
    smb2_set_password(d_->ctx,secret.constData());
    secret.fill('\0'); password.fill(QChar::Null);
    const auto server=(resolved.contains(':') ? '['+resolved+']' : resolved)
                      + ':'+QString::number(origin->port(445));
    const auto serverBytes=server.toUtf8(), shareBytes=share.toUtf8();
    const int result=d_->request([&](auto cb,void *data) {
        return smb2_connect_share_async(d_->ctx,serverBytes.constData(),shareBytes.constData(),user.constData(),cb,data);
    },nullptr,error);
    if(result>=0) return true;
    const bool network=d_->nt==0;
    d_->disconnect();
    if(network) {
        const auto diagnostic=UgosClient::connectionDiagnostic(*origin,std::min(5000,d_->timeout));
        if(diagnostic==UgosClient::ConnectionState::LocalNetworkDenied)
            fail(error,QStringLiteral("macOS 已拒绝当前应用的本地网络访问，请在系统设置中允许此版本后重试。"));
        else if(diagnostic==UgosClient::ConnectionState::Ready)
            fail(error,QStringLiteral("SMB TCP 可连接，但会话未能建立；请检查 SMB3、签名与账号权限后重试。"));
    }
    return false;
}
QList<NasRepairFile> SmbClient::list(QString logical,QString *error) {
    clear(error);
    const auto path=relativePath(logical,d_->share,true,error);
    if(!path) return {};
    void *value=nullptr;
    if(d_->request([&](auto cb,void *data) {
        return smb2_opendir_async(d_->ctx,path->constData(),cb,data);
    },&value,error)<0) return {};
    auto *directory=static_cast<smb2dir*>(value);
    if(!directory) { fail(error,QStringLiteral("SMB 目录响应无效。")); return {}; }
    QList<NasRepairFile> result;
    while(auto *item=smb2_readdir(d_->ctx,directory)) {
        const QByteArray raw(item->name);
        const auto name=QString::fromUtf8(raw);
        if(name=="." || name=="..") continue;
        if(!validComponent(name) || name.toUtf8()!=raw) {
            smb2_closedir(d_->ctx,directory);
            fail(error,QStringLiteral("SMB 目录含无法安全表示的文件名。")); return {};
        }
        auto converted=entry(logical+'/'+name,item->st);
        if(!converted) { smb2_closedir(d_->ctx,directory); fail(error,QStringLiteral("SMB 文件信息超出支持范围。")); return {}; }
        result.append(*converted);
    }
    smb2_closedir(d_->ctx,directory);
    return result;
}
std::optional<NasRepairFile> SmbClient::stat(QString logical,QString *error) {
    const auto path=relativePath(logical,d_->share,true,error);
    if(!path) return {};
    const auto raw=d_->rawStat(*path,error);
    if(!raw) return {};
    auto result=entry(logical,*raw);
    if(!result) fail(error,QStringLiteral("SMB 文件信息超出支持范围。"));
    return result;
}
bool SmbClient::createHardLink(QString source,QString destination,QString *error) {
    const auto src=relativePath(source,d_->share,false,error);
    if(!src) return false;
    const auto dst=relativePath(destination,d_->share,false,error);
    if(!dst) return false;
    const auto destinationText=QString::fromUtf8(*dst);
    if(source==destination || destinationText.normalized(QString::NormalizationForm_C)!=destinationText) {
        fail(error,QStringLiteral("SMB 硬链接目标必须是独立的 NFC 路径。")); return false;
    }
    const auto rawBefore=d_->rawStat(*src,error);
    if(!rawBefore) return false;
    const auto before=entry(source,*rawBefore);
    if(!before || rawBefore->smb2_type!=SMB2_TYPE_FILE || before->fileId==0) {
        fail(error,QStringLiteral("SMB 源文件必须是具有有效身份的普通文件。")); return false;
    }
    if(d_->request([&](auto cb,void *data) {
        return smb2_link_async(d_->ctx,src->constData(),dst->constData(),cb,data);
    },nullptr,error)<0) return false;
    const auto after=stat(source,error);
    if(!after) return false;
    const auto alias=stat(destination,error);
    if(!alias) return false;
    if(!sameVersion(*before,*after) || !sameVersion(*after,*alias) || alias->links<2) {
        fail(error,QStringLiteral("SMB 硬链接身份验证失败，未标记为可用。")); return false;
    }
    return true;
}
bool SmbClient::download(QString source,QString localTemp,QByteArray *sha256,QString *error) {
    clear(error); d_->storage=false;
    if(sha256) sha256->clear();
    const auto path=relativePath(source,d_->share,false,error);
    if(!path) return false;
    QFile output(localTemp);
    if(!output.open(QIODevice::WriteOnly|QIODevice::NewOnly)) {
        const QFileInfo destination(localTemp);
        d_->storage=!destination.exists() && !destination.isSymbolicLink();
        fail(error,d_->storage ? QStringLiteral("本机缓存临时文件创建失败，已停止继续写入。")
                              : QStringLiteral("缓存临时文件已存在，不会覆盖。")); return false;
    }
    smb2fh *handle=nullptr;
    auto discard=[&] {
        const quint32 nt=d_->nt;
        if(handle && d_->ctx) {
            QString ignored;
            d_->request([&](auto cb,void *data) { return smb2_close_async(d_->ctx,handle,cb,data); },
                        nullptr,&ignored,std::min(1000,d_->timeout));
        }
        handle=nullptr; d_->nt=nt;
        const bool owned=ownsPath(output); output.close(); if(owned) output.remove();
        return false;
    };
    QElapsedTimer elapsed; elapsed.start();
    const auto rawBefore=d_->rawStat(*path,error);
    if(!rawBefore) return discard();
    const auto before=entry(source,*rawBefore);
    if(!before || rawBefore->smb2_type!=SMB2_TYPE_FILE || before->fileId==0) {
        fail(error,QStringLiteral("SMB 下载源必须是具有有效身份的普通文件。")); return discard();
    }
    auto remaining=[&] { return std::max(1,d_->timeout-int(elapsed.elapsed())); };
    void *opened=nullptr;
    if(d_->request([&](auto cb,void *data) { return smb2_open_async(d_->ctx,path->constData(),O_RDONLY,cb,data); },
                    &opened,error,remaining())<0) return discard();
    handle=static_cast<smb2fh*>(opened);
    smb2_stat_64 snapshot{};
    auto fingerprint=[&]() -> std::optional<NasRepairFile> {
        if(d_->request([&](auto cb,void *data) { return smb2_fstat_async(d_->ctx,handle,&snapshot,cb,data); },
                       nullptr,error,remaining())<0) return {};
        return entry(source,snapshot);
    };
    const auto first=fingerprint();
    if(!first || !sameVersion(*before,*first)) { fail(error,QStringLiteral("SMB 源文件在读取前已改变。")); return discard(); }
    QByteArray buffer(256*1024,'\0');
    const auto maxRead=std::min<quint32>(buffer.size(),smb2_get_max_read_size(d_->ctx));
    if(maxRead==0) { fail(error,QStringLiteral("SMB 读取协商无效。")); return discard(); }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    quint64 offset=0;
    while(offset<quint64(before->size)) {
        if(elapsed.elapsed()>=d_->timeout) { fail(error,QStringLiteral("SMB 下载超时。")); return discard(); }
        const auto count=quint32(std::min<quint64>(maxRead,quint64(before->size)-offset));
        const int received=d_->request([&](auto cb,void *data) {
            return smb2_pread_async(d_->ctx,handle,reinterpret_cast<uint8_t*>(buffer.data()),count,offset,cb,data);
        },nullptr,error,remaining());
        if(received<=0 || quint32(received)>count) {
            if(received==0) fail(error,QStringLiteral("SMB 文件提前结束。"));
            return discard();
        }
        if(output.write(buffer.constData(),received)!=received) {
            d_->storage=true; fail(error,QStringLiteral("本地缓存写入失败。")); return discard();
        }
        hash.addData(QByteArrayView(buffer.constData(),received)); offset+=received;
    }
    const auto last=fingerprint();
    if(!last || !sameVersion(*before,*last)) { fail(error,QStringLiteral("SMB 源文件在读取期间已改变。")); return discard(); }
    if(d_->request([&](auto cb,void *data) { return smb2_close_async(d_->ctx,handle,cb,data); },nullptr,error,remaining())<0) {
        handle=nullptr; return discard();
    }
    handle=nullptr;
    if(!output.flush()) { d_->storage=true; fail(error,QStringLiteral("本地缓存写入失败。")); return discard(); }
    if(!ownsPath(output)) { fail(error,QStringLiteral("缓存临时路径已改变。")); return discard(); }
    output.close(); if(sha256) *sha256=hash.result(); return true;
}
