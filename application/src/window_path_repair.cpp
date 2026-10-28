#include "window.h"
#include "path_repair.h"
#include "smb_client.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QStorageInfo>
#include <QVBoxLayout>
#include <QCryptographicHash>
#include <QtConcurrent>

namespace {
class RepairDialog : public QDialog {
public:
    using QDialog::QDialog;
    bool running = false;
    std::shared_ptr<std::atomic_bool> pause = std::make_shared<std::atomic_bool>(false);
    void reject() override {
        if (running) { pause->store(true); return; }
        QDialog::reject();
    }
};
class NativeRepairBackend : public PathRepairBackend {
public:
    SmbClient client;
    bool storageFailure() const override { return client.lastDownloadStorageFailure(); }
    bool sessionUnavailable() const override { return client.sessionUnavailable(); }
    QList<NasRepairFile> list(const QString &path, QString *error) override {
        return client.list(path, error);
    }
    std::optional<NasRepairFile> stat(const QString &path, QString *error) override { return client.stat(path, error); }
    bool createHardLink(const QString &source, const QString &destination, QString *error) override {
        return client.createHardLink(source, destination, error);
    }
    bool download(const QString &path, const QString &destination, QByteArray *sha, QString *error) override {
        client.setTimeoutMs(180000);
        return client.download(path, destination, sha, error);
    }
};
QString pathSettings(const QString &root) { return QDir(root).filePath("path-repair-v2-settings.json"); }
} // namespace

void Window::showNasPathRepair() {
    if (auto *existing = findChild<QDialog *>("nasPathRepairDialog")) {
        existing->show(); existing->raise(); existing->activateWindow(); return;
    }
    auto *dialog = new RepairDialog(this);
    dialog->setObjectName("nasPathRepairDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("批量处理路径问题");
    dialog->resize(800, 750);
    auto *layout = new QVBoxLayout(dialog);
    auto *intro = new QLabel("处理 NAS 特殊字符路径：优先建立兼容硬链接，不支持或无法访问的单项缓存到本机。\n"
                            "原文件名不变，不写入音频内容。硬链接与原件共享内容，软件仅只读使用；不额外复制音频。\n"
                            "保留收藏、标签和已有波形。仅处理路径异常；账号密码只用于本次 SMB 连接。");
    intro->setWordWrap(true); layout->addWidget(intro);
    auto *form = new QFormLayout;
    auto *library = new QComboBox;
    library->setObjectName("pathRepairLibrary");
    for (const auto &l : libraries_) library->addItem(l.name, l.id);
    form->addRow("处理素材库", library);
    auto *endpoint = new QLineEdit;
    endpoint->setObjectName("pathRepairEndpoint"); endpoint->setPlaceholderText("NAS 的局域网 IP 或主机名（现有 SMB 服务）");
    auto *username = new QLineEdit; username->setObjectName("pathRepairUsername");
    auto *password = new QLineEdit; password->setObjectName("pathRepairPassword");
    password->setEchoMode(QLineEdit::Password);
    form->addRow("NAS 地址", endpoint); form->addRow("账号", username); form->addRow("密码", password);
    auto *mount = new QLineEdit; mount->setObjectName("pathRepairMount");
    auto *remote = new QLineEdit; remote->setObjectName("pathRepairRemoteRoot");
    remote->setPlaceholderText("例如 漫剧制作部（2），不要填写 /volume2 路径");
    form->addRow("本机共享挂载目录", mount); form->addRow("SMB 共享名称", remote);
    auto *output = new QLineEdit; output->setObjectName("pathRepairOutput");
    auto *choose = new QPushButton("选择…");
    auto *outputRow = new QHBoxLayout; outputRow->addWidget(output); outputRow->addWidget(choose);
    form->addRow("NAS 兼容入口目录", outputRow);
    auto *local = new QLineEdit(QDir(dataRoot_).filePath("audio-compat-cache"));
    local->setObjectName("pathRepairLocalCache");
    auto *chooseLocal = new QPushButton("选择…");
    auto *localRow = new QHBoxLayout; localRow->addWidget(local); localRow->addWidget(chooseLocal);
    form->addRow("本机异常缓存目录", localRow);
    auto *limit = new QSpinBox; limit->setObjectName("pathRepairLimit");
    limit->setRange(0, 1000000); limit->setSpecialValueText("全部");
    limit->setToolTip("可先处理少量文件验证，填 0 处理全部路径异常；继续时校验并复用已完成项");
    form->addRow("本轮最多处理", limit);
    layout->addLayout(form);
    auto *status = new QLabel("正在读取本机异常记录…"); status->setObjectName("pathRepairStatus");
    status->setTextFormat(Qt::PlainText); status->setWordWrap(true); layout->addWidget(status);
    auto *bar = new QProgressBar; bar->setObjectName("pathRepairProgress"); bar->setValue(0); layout->addWidget(bar);
    auto *details = new QPlainTextEdit; details->setObjectName("pathRepairResults");
    details->setReadOnly(true); details->setMaximumBlockCount(500); layout->addWidget(details, 1);
    auto *buttons = new QDialogButtonBox;
    auto *start = buttons->addButton("开始 / 继续处理", QDialogButtonBox::ActionRole);
    start->setObjectName("startPathRepair"); start->setEnabled(false);
    auto *pause = buttons->addButton("暂停", QDialogButtonBox::ActionRole);
    pause->setObjectName("pausePathRepair"); pause->setEnabled(false);
    auto *close = buttons->addButton("关闭", QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(pause, &QPushButton::clicked, dialog, [=] {
        dialog->pause->store(true); pause->setEnabled(false); status->setText("正在完成当前文件并保存记录，随后暂停…");
    });
    connect(choose, &QPushButton::clicked, dialog, [=] {
        const auto selected = QFileDialog::getExistingDirectory(dialog, "选择同一 NAS 共享中的空兼容入口目录", output->text());
        if (!selected.isEmpty()) output->setText(QFileInfo(selected).canonicalFilePath());
    });
    connect(chooseLocal, &QPushButton::clicked, dialog, [=] {
        const auto selected = QFileDialog::getExistingDirectory(dialog, "选择本机空目录或本软件的异常缓存目录", local->text());
        if (!selected.isEmpty()) local->setText(QFileInfo(selected).canonicalFilePath());
    });
    auto defaults = [=, this] {
        for (const auto &l : libraries_) if (l.id == library->currentData().toString()) {
            const QStorageInfo volume(l.root);
            mount->setText(volume.rootPath());
            remote->setText(QFileInfo(volume.rootPath()).fileName());
            if (endpoint->text().isEmpty()) {
                const QUrl device("smb:" + QString::fromUtf8(volume.device()));
                if (!device.host().isEmpty()) endpoint->setText(device.host());
            }
            output->setText(QFileInfo(l.root).dir().filePath("源音效兼容入口"));
        }
    };
    defaults();
    QFile saved(pathSettings(dataRoot_));
    if (saved.open(QIODevice::ReadOnly)) {
        const auto s = QJsonDocument::fromJson(saved.readAll()).object();
        const int index = library->findData(s["library"].toString()); if (index >= 0) library->setCurrentIndex(index);
        for (auto pair : {qMakePair(endpoint, "endpoint"), qMakePair(mount, "mount"), qMakePair(remote, "remote"), qMakePair(output, "output"), qMakePair(local, "local")})
            if (s.contains(pair.second)) pair.first->setText(s[pair.second].toString());
    }
    struct Candidates { quint64 generation = 0; QList<Track> tracks; };
    auto candidates = std::make_shared<Candidates>();
    auto reload = [=, this] {
        const auto generation = ++candidates->generation;
        start->setEnabled(false); candidates->tracks.clear();
        auto *watcher = new QFutureWatcher<QPair<QList<Track>, QString>>(dialog);
        connect(watcher, &QFutureWatcherBase::finished, dialog, [=] {
            const auto result = watcher->result(); watcher->deleteLater();
            if (generation != candidates->generation) return;
            candidates->tracks = result.first;
            status->setText(result.second.isEmpty() ? QString("当前素材库有 %1 条去重路径异常。可先处理少量验证，再继续全部。\n"
                "暂停会在当前文件完成后生效；重启软件继续时需要重新登录。").arg(candidates->tracks.size()) : result.second);
            start->setEnabled(result.second.isEmpty() && !candidates->tracks.isEmpty());
        });
        const auto index = databasePath_, failure = QDir(dataRoot_).filePath("waveform-failures.sqlite"), id = library->currentData().toString();
        watcher->setFuture(QtConcurrent::run([=] {
            QString error; const auto items = pathRepairCandidates(index, failure, id, &error);
            return qMakePair(items, error);
        }));
    };
    connect(library, &QComboBox::currentIndexChanged, dialog, [=] { defaults(); reload(); });
    connect(start, &QPushButton::clicked, dialog, [=, this] {
        if (username->text().trimmed().isEmpty() || password->text().isEmpty()) {
            status->setText("请填写 NAS 账号和密码；密码不会保存。"); return;
        }
        QString endpointError;
        const auto address = SmbClient::endpoint(endpoint->text().trimmed(), &endpointError);
        if (!address) { status->setText(endpointError); return; }
        const auto share = remote->text().trimmed();
        if (!SmbClient::relativePath("/" + share, share, true, &endpointError)) { status->setText(endpointError); return; }
        PathRepairOptions options;
        options.indexPath = databasePath_; options.libraries = libraries_;
        options.mountRoot = mount->text().trimmed(); options.serverRoot = "/" + share; options.outputRoot = output->text().trimmed();
        options.preferHardLinks = true; options.localCacheRoot = local->text().trimmed();
        options.serverIdentity = address->host().toLower() + ":" + QString::number(address->port(445)) + "/" + share;
        const auto digest = QCryptographicHash::hash((options.serverIdentity + "\n" + options.outputRoot + "\n" + options.localCacheRoot).toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
        options.checkpointPath = QDir(dataRoot_).filePath("path-repair-v2-" + QString::fromLatin1(digest) + ".json");
        QSaveFile settings(pathSettings(dataRoot_));
        if (settings.open(QIODevice::WriteOnly)) {
            settings.write(QJsonDocument(QJsonObject{{"library", library->currentData().toString()}, {"endpoint", endpoint->text().trimmed()},
                {"mount", options.mountRoot}, {"remote", share}, {"output", options.outputRoot}, {"local", options.localCacheRoot}}).toJson()); settings.commit();
        }
        auto tracks = candidates->tracks;
        if (limit->value() > 0 && tracks.size() > limit->value()) tracks = tracks.mid(0, limit->value());
        const auto account = username->text(); auto secret = password->text(); password->clear();
        dialog->running = true; dialog->pause->store(false);
        start->setEnabled(false); pause->setEnabled(true); close->setEnabled(false);
        for (auto *widget : QList<QWidget *>{library, endpoint, username, password, mount, remote, output, local, limit, choose, chooseLocal}) widget->setEnabled(false);
        waveQueue_->pausePreparation();
        status->setText("正在连接 NAS…"); bar->setRange(0, tracks.size()); bar->setValue(0);
        auto *watcher = new QFutureWatcher<PathRepairResult>(dialog);
        connect(watcher, &QFutureWatcherBase::finished, dialog, [=, this] {
            const auto result = watcher->result(); watcher->deleteLater(); dialog->running = false;
            pause->setEnabled(false); close->setEnabled(true); start->setEnabled(true);
            for (auto *widget : QList<QWidget *>{library, endpoint, username, password, mount, remote, output, local, limit, choose, chooseLocal}) widget->setEnabled(true);
            QString text = result.error.isEmpty() ? QString("%1：已处理 %2 / %3 · 硬链接 %4 · 本机缓存 %5 · 复用 %6 · 未通过 %7。")
                .arg(result.paused ? "已暂停" : "本轮处理结束").arg(result.done).arg(result.total)
                .arg(result.hardlinked).arg(result.cached).arg(result.reused).arg(result.failed) : result.error;
            if (!result.lastError.isEmpty()) text += "\n最近一项：" + result.lastError;
            if (result.succeeded > 0) {
                waveQueue_->refreshIndex(false); refreshSearch(); workspace_->refresh();
                for (const auto &mapped : result.repairedTracks)
                    if (mapped.id == selected_.id)
                        selectTrackValue(mapped);
                waveQueue_->retryFailed();
                text += "\n兼容文件已接入，正在重试异常项波形；波形成功后异常记录才会消除，参考波形仍保留参考标记。";
            }
            status->setText(text); details->appendPlainText(text);
        });
        const auto guard = QPointer<RepairDialog>(dialog);
        const auto pauseFlag = dialog->pause;
        connect(qApp, &QCoreApplication::aboutToQuit, dialog, [pauseFlag] { pauseFlag->store(true); });
        const auto endpointValue = endpoint->text().trimmed();
        watcher->setFuture(QtConcurrent::run([=, secret = std::move(secret)]() mutable {
            NativeRepairBackend backend;
            PathRepairResult failed; failed.total = tracks.size();
            if (!backend.client.login(endpointValue, share, account, secret, &failed.error)) { secret.fill(QChar(0)); return failed; }
            secret.fill(QChar(0));
            return runPathRepair(options, tracks, backend, pauseFlag.get(), [guard, status, bar, details](const QJsonObject &event) {
                if (!guard) return;
                QMetaObject::invokeMethod(guard, [=] {
                    bar->setValue(event["done"].toInt());
                    status->setText(QString("正在处理 %1 / %2 · 硬链接 %3 · 本机缓存 %4 · 复用 %5 · 未通过 %6\n%7")
                        .arg(event["done"].toInt()).arg(event["total"].toInt()).arg(event["hardlinked"].toInt())
                        .arg(event["cached"].toInt()).arg(event["reused"].toInt()).arg(event["failed"].toInt()).arg(event["current"].toString()));
                    const auto message = event["message"].toString();
                    const auto strategy = event["strategy"].toString();
                    if (!message.isEmpty()) details->appendPlainText(event["current"].toString() + "：" + message);
                    else if (!strategy.isEmpty()) details->appendPlainText(event["current"].toString() + "：" +
                        (event["reusedEntry"].toBool() ? QString("复用已校验入口") : strategy == "hardlink" ? QString("硬链接已校验") : QString("本机缓存已校验")));
                }, Qt::QueuedConnection);
            });
        }));
    });
    reload(); dialog->show();
}
