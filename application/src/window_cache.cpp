#include "window.h"
#include <QBoxLayout>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

void Window::configureSharedCache(const QString &libraryId) {
    if (catalogBusy_) {
        setStatus("正在处理素材库，请完成后设置共享缓存。");
        return;
    }
    if (!indexReady_ || libraries_.isEmpty()) {
        setStatus("请先添加音乐目录。");
        return;
    }
    if (registration_ || scan_) {
        setStatus("目录正在检查或收录，完成后即可设置共享缓存。");
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setObjectName("sharedCacheDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("共享波形缓存");
    dialog->resize(620, 300);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(14);
    auto *libraries = new QComboBox;
    for (const auto &lib : libraries_)
        libraries->addItem(lib.name, lib.id);
    const auto wanted = libraryId.isEmpty() ? scopeLibrary_ : libraryId;
    const int at = libraries->findData(wanted);
    if (at >= 0)
        libraries->setCurrentIndex(at);
    layout->addWidget(libraries);
    auto *source = new QLabel;
    source->setWordWrap(true);
    source->setTextFormat(Qt::PlainText);
    layout->addWidget(source);
    auto *row = new QHBoxLayout;
    auto *path = new QLineEdit;
    path->setObjectName("sharedCachePath");
    path->setPlaceholderText("专门存放波形缓存的共享文件夹");
    auto *browse = new QPushButton("选择文件夹");
    row->addWidget(path, 1);
    row->addWidget(browse);
    layout->addLayout(row);
    auto *hint = new QLabel("选择空的专用文件夹，或同事已建立的波形缓存文件夹。可以放在音乐目录内，收录会跳过"
                            "它。\n现有索引、收藏和标签保留；整首拖出使用原件；路径修复项使用已校验的兼容副本。");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *result = new QLabel;
    result->setWordWrap(true);
    result->setTextFormat(Qt::PlainText);
    result->setObjectName("sharedCacheResult");
    layout->addWidget(result);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Save)->setText("接入共享缓存");
    buttons->button(QDialogButtonBox::Close)->setText("关闭");
    layout->addWidget(buttons);
    auto load = [this, libraries, path, source] {
        for (const auto &lib : libraries_)
            if (lib.id == libraries->currentData().toString()) {
                path->setText(QDir::toNativeSeparators(lib.sharedCache));
                source->setText("音乐目录：" + QDir::toNativeSeparators(lib.root));
                break;
            }
    };
    load();
    connect(libraries, &QComboBox::currentIndexChanged, dialog, load);
    connect(browse, &QPushButton::clicked, dialog, [dialog, path] {
        const auto selected =
            QFileDialog::getExistingDirectory(dialog, "选择共享波形缓存文件夹", path->text());
        if (!selected.isEmpty())
            path->setText(QDir::toNativeSeparators(selected));
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(buttons, &QDialogButtonBox::accepted, dialog,
            [this, dialog, libraries, path, result, buttons, browse] {
                if (registration_)
                    return;
                if (scan_) {
                    result->setText("目录正在收录，完成或停止收录后再接入共享缓存。");
                    return;
                }
                if (path->text().trimmed().isEmpty()) {
                    result->setText("请填写共享缓存文件夹路径。");
                    return;
                }
                const auto id = libraries->currentData().toString();
                libraries->setEnabled(false);
                path->setEnabled(false);
                browse->setEnabled(false);
                buttons->button(QDialogButtonBox::Save)->setEnabled(false);
                result->setText("正在检查文件夹和共享缓存身份…");
                QPointer<QDialog> guard(dialog);
                auto completed = std::make_shared<bool>(false);
                registration_ = worker(
                    "attach-cache",
                    {{"database", databasePath_},
                     {"library", id},
                     {"cache", QDir::fromNativeSeparators(path->text().trimmed())}},
                    [this, guard, result, completed](const QJsonObject &message) {
                        if (message["event"] == "cache-attached") {
                            *completed = true;
                            waveQueue_->refreshIndex();
                            reloadLibraries();
                            setStatus(
                                "共享波形缓存已接入，已有索引和个人标记保留。准备全部波形可补齐共享缓存。");
                            if (guard)
                                result->setText("已接入：" +
                                                QDir::toNativeSeparators(message["cache"].toString()));
                        } else if (message["event"] == "error") {
                            if (guard)
                                result->setText(message["message"].toString());
                            setStatus("共享缓存未接入：" + message["message"].toString());
                        }
                    });
                auto *process = registration_;
                auto finish = [this, process, guard, libraries, path, browse, buttons, result, completed] {
                    if (registration_ != process)
                        return;
                    registration_ = nullptr;
                    if (guard) {
                        libraries->setEnabled(true);
                        path->setEnabled(true);
                        browse->setEnabled(true);
                        buttons->button(QDialogButtonBox::Save)->setEnabled(true);
                        if (!*completed && process->exitStatus() == QProcess::CrashExit)
                            result->setText("检查已停止，请确认共享文件夹可访问后重试。");
                        if (!*completed && process->error() == QProcess::FailedToStart)
                            result->setText("目录检查进程无法启动，请检查软件文件是否完整。");
                    }
                    process->deleteLater();
                };
                connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, finish);
                connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
                    if (error == QProcess::FailedToStart)
                        finish();
                });
                if (process->state() == QProcess::NotRunning && process->error() == QProcess::FailedToStart)
                    QTimer::singleShot(0, process, finish);
                QTimer::singleShot(20000, process, [process] {
                    if (process->state() != QProcess::NotRunning)
                        process->kill();
                });
            });
    dialog->show();
}
