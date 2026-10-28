#include "window.h"
#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>

namespace {
QString categoryLabel(const QString &category) {
    static const QMap<QString, QString> labels{{"network", "路径访问 / 读取失败"}, {"unsupported", "格式不支持"},
                                               {"damaged", "解码 / 完整性异常"}, {"decode_warning", "可用音频 · 有异常"},
                                               {"storage", "缓存写入 / 空间"},
                                               {"timeout", "读取超时"},      {"changed", "原文件已变化"},
                                               {"skipped", "手动跳过"},      {"unknown", "其他异常"}};
    return labels.value(category, QString("其他异常"));
}
Track failureTrack(const QJsonObject &item) {
    Track t;
    t.id = item["id"].toInteger();
    t.libraryId = item["library"].toString();
    t.relativePath = item["path"].toString();
    t.name = item["name"].toString();
    t.root = item["root"].toString();
    t.folder = item["folder"].toString();
    t.size = item["size"].toInteger();
    t.modified = item["modified"].toInteger();
    return t;
}
} // namespace

void Window::showWaveFailures() {
    if (failureDialog_) {
        failureDialog_->show();
        failureDialog_->raise();
        failureDialog_->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    failureDialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("波形记录");
    dialog->resize(880, 620);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);
    auto *summary = new QLabel;
    summary->setWordWrap(true);
    summary->setTextFormat(Qt::PlainText);
    layout->addWidget(summary);
    auto *bar = new QHBoxLayout;
    auto *kind = new QComboBox;
    kind->setObjectName("failureRecordKind");
    kind->addItem("待处理异常", false);
    kind->addItem("可用波形（含参考）", true);
    bar->addWidget(kind);
    auto *category = new QComboBox;
    category->setObjectName("failureCategory");
    category->addItem("全部原因", QString());
    for (const auto &id :
         {"network", "unsupported", "damaged", "decode_warning", "storage", "timeout", "changed", "skipped", "unknown"})
        category->addItem(categoryLabel(QString::fromLatin1(id)), QString::fromLatin1(id));
    bar->addWidget(category);
    bar->addStretch();
    auto *repairPaths = new QPushButton("批量处理路径问题…");
    repairPaths->setObjectName("repairNasPaths");
    repairPaths->setToolTip("通过 NAS 读取原件，创建并校验兼容副本；保留原件与已有波形");
    bar->addWidget(repairPaths);
    connect(repairPaths, &QPushButton::clicked, this, &Window::showNasPathRepair);
    auto *retry = new QPushButton("只重试失败");
    retry->setObjectName("retryFailedWaves");
    bar->addWidget(retry);
    layout->addLayout(bar);
    auto *table = new QTableWidget(0, 3);
    table->setObjectName("failureRecords");
    table->setHorizontalHeaderLabels({"音乐名称", "原因", "记录时间"});
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->setColumnWidth(1, 160);
    table->setColumnWidth(2, 175);
    layout->addWidget(table, 1);
    auto *detail = new QPlainTextEdit;
    detail->setObjectName("waveFailureDetails");
    detail->setReadOnly(true);
    detail->setMaximumHeight(105);
    detail->setPlaceholderText("选择一条记录，查看原因与原文件位置");
    layout->addWidget(detail);
    auto *actions = new QHBoxLayout;
    auto *audition = new QPushButton("试听原文件"), *locate = new QPushButton("定位文件夹"),
         *copy = new QPushButton("复制原文件路径");
    actions->addWidget(audition);
    actions->addWidget(locate);
    actions->addWidget(copy);
    actions->addStretch();
    auto *previous = new QPushButton("上一页"), *next = new QPushButton("下一页");
    auto *pageLabel = new QLabel;
    actions->addWidget(previous);
    actions->addWidget(pageLabel);
    actions->addWidget(next);
    layout->addLayout(actions);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText("关闭");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    struct State {
        quint64 request = 0;
        int offset = 0;
        QJsonArray items;
        qint64 known = -1, refs = -1, retryDone = -1;
        bool more = false;
    };
    auto state = std::make_shared<State>();
    auto selected = [this, table, state] {
        const auto row = table->currentRow();
        const auto item =
            row >= 0 && row < state->items.size() ? state->items[row].toObject() : QJsonObject();
        auto track = failureTrack(item);
        for (const auto &library : libraries_)
            if (library.id == track.libraryId) {
                track.root = library.root;
                break;
            }
        return track;
    };
    auto updateSelection = [=] {
        const auto track = selected();
        const bool valid = track.id && !track.relativePath.isEmpty();
        audition->setEnabled(valid);
        locate->setEnabled(valid);
        copy->setEnabled(valid);
        const auto row = table->currentRow();
        const auto item =
            row >= 0 && row < state->items.size() ? state->items[row].toObject() : QJsonObject();
        detail->setPlainText(
            valid ? QString("%1\n%2\n%3")
                        .arg(item["message"].toString(), track.absolutePath(), item["time"].toString())
                  : QString());
    };
    connect(table, &QTableWidget::itemSelectionChanged, dialog, updateSelection);
    connect(audition, &QPushButton::clicked, dialog, [this, selected] {
        const auto track = selected();
        if (track.id)
            activateWorkspaceTrack(track);
    });
    connect(table, &QTableWidget::cellDoubleClicked, dialog, [this, selected](int, int) {
        const auto track = selected();
        if (track.id)
            activateWorkspaceTrack(track);
    });
    connect(locate, &QPushButton::clicked, dialog, [this, selected] {
        const auto track = selected();
        if (track.id)
            locateFolder(track.libraryId, track.folder == "." ? QString() : track.folder);
    });
    connect(copy, &QPushButton::clicked, dialog, [selected] {
        const auto track = selected();
        if (track.id)
            QApplication::clipboard()->setText(track.absolutePath());
    });
    auto request = [this, kind, category, state, table, detail, previous, next, pageLabel, updateSelection] {
        const bool references = kind->currentData().toBool();
        category->setEnabled(true);
        state->request = waveQueue_->requestFailurePage(
            state->offset, 100, category->currentData().toString(), references);
        table->setEnabled(false);
        previous->setEnabled(false);
        next->setEnabled(false);
        pageLabel->setText("读取中…");
        state->items = {};
        detail->clear();
        updateSelection();
    };
    auto *refresh = new QTimer(dialog);
    refresh->setSingleShot(true);
    refresh->setInterval(300);
    connect(refresh, &QTimer::timeout, dialog, request);
    auto updateSummary = [this, summary, retry, kind, state, refresh] {
        const auto prep = waveQueue_->preparation();
        const auto known = prep["knownFailures"].toInteger(), refs = prep["knownReferences"].toInteger();
        QString text = QString("已记录 %1 条待处理异常 · %2 条参考波形。记录保存在本机，不改动原文件。")
                           .arg(known)
                           .arg(refs);
        const auto causes = prep["failureCategories"].toObject();
        const auto usable = prep["referenceCategories"].toObject();
        text += QString("\n路径访问 / 读取失败 %1 · 解码异常 %2 · 已解出可用音频（有异常）%3。")
                    .arg(causes["network"].toInteger() + usable["network"].toInteger())
                    .arg(causes["damaged"].toInteger())
                    .arg(usable["decode_warning"].toInteger());
        const auto unknown = prep["legacyUntracked"].toInteger();
        if (unknown > 0)
            text +=
                QString(
                    "\n旧版还有 %1 条历史失败没有文件明细，‘只重试失败’仅处理已记录项；重新检查全库可补齐。")
                    .arg(unknown);
        const auto inactive = prep["inactiveRecords"].toInteger();
        if (inactive > 0)
            text += QString("\n另有 %1 条记录属于已移除或排除的素材，已隐藏，不参与重试。")
                        .arg(inactive);
        if (prep["reference"].toInteger() > refs + prep["inactiveReferences"].toInteger())
            text += "\n旧版参考波形明细尚未全部收录，重新检查本机缓存后可查看。";
        if (prep["mode"].toString() == "reference")
            text += QString("\n参考波形已分析 %1 / %2 · 已转准确 %3 · 本轮仍参考 %4 · 异常 %5。")
                        .arg(prep["retryDone"].toInteger())
                        .arg(prep["retryTotal"].toInteger())
                        .arg(prep["retryConverted"].toInteger())
                        .arg(prep["retryRemaining"].toInteger())
                        .arg(prep["retryFailed"].toInteger());
        else if (prep["mode"].toString() == "failed")
            text += QString("\n本轮已检查 %1 / %2，仍失败 %3%4。")
                        .arg(prep["retryDone"].toInteger())
                        .arg(prep["retryTotal"].toInteger())
                        .arg(prep["retryFailed"].toInteger())
                        .arg(prep["deferred"].toBool() ? " · " + prep["deferReason"].toString() : QString());
        if (!prep["error"].toString().isEmpty())
            text += "\n" + prep["error"].toString();
        summary->setText(text);
        const bool references = kind->currentData().toBool();
        const QString mode = references ? "reference" : "failed";
        const bool resume = prep["mode"].toString() == mode && !prep["retryComplete"].toBool() &&
                            prep["retryUpper"].toInteger() > 0;
        retry->setEnabled(prep["failuresReady"].toBool() && (references ? refs : known) > 0 &&
                          !prep["running"].toBool());
        retry->setText(references ? (resume ? "继续分析参考波形" : "重新分析参考波形")
                                  : (resume ? "继续重试失败" : "只重试失败"));
        if (state->known >= 0 && (state->known != known || state->refs != refs ||
                                  state->retryDone != prep["retryDone"].toInteger()))
            refresh->start();
        state->known = known;
        state->refs = refs;
        state->retryDone = prep["retryDone"].toInteger();
    };
    connect(waveQueue_, &WaveQueue::preparationChanged, dialog, updateSummary);
    connect(retry, &QPushButton::clicked, dialog, [this, kind] {
        const auto rate = preparationSpeed_->currentData().toLongLong();
        if (kind->currentData().toBool())
            waveQueue_->retryReferences(rate);
        else
            waveQueue_->retryFailed(rate);
    });
    connect(kind, &QComboBox::currentIndexChanged, dialog, [state, request, updateSummary] {
        state->offset = 0;
        request();
        updateSummary();
    });
    connect(category, &QComboBox::currentIndexChanged, dialog, [state, request] {
        state->offset = 0;
        request();
    });
    connect(previous, &QPushButton::clicked, dialog, [state, request] {
        state->offset = qMax(0, state->offset - 100);
        request();
    });
    connect(next, &QPushButton::clicked, dialog, [state, request] {
        if (state->more) {
            state->offset += 100;
            request();
        }
    });
    connect(waveQueue_, &WaveQueue::failurePageReady, dialog, [=](const QJsonObject &page) {
        if (page["requestId"].toInteger() != qint64(state->request))
            return;
        table->setEnabled(true);
        table->setRowCount(0);
        state->items = page["items"].toArray();
        if (!page["error"].toString().isEmpty()) {
            detail->setPlainText("记录读取失败：" + page["error"].toString());
            pageLabel->setText("读取失败");
            return;
        }
        const auto total = page["total"].toInteger();
        if (state->offset > 0 && state->items.isEmpty()) {
            state->offset = qMax(0, int((qMax(qint64(1), total) - 1) / 100) * 100);
            request();
            return;
        }
        for (const auto &value : state->items) {
            const auto item = value.toObject();
            const int row = table->rowCount();
            table->insertRow(row);
            auto *name = new QTableWidgetItem(item["name"].toString());
            name->setToolTip(item["path"].toString());
            table->setItem(row, 0, name);
            table->setItem(row, 1, new QTableWidgetItem(categoryLabel(item["category"].toString())));
            table->setItem(row, 2, new QTableWidgetItem(item["time"].toString()));
        }
        state->more = page["more"].toBool();
        previous->setEnabled(state->offset > 0);
        next->setEnabled(state->more);
        pageLabel->setText(total ? QString("%1–%2 / %3")
                                       .arg(state->offset + 1)
                                       .arg(state->offset + state->items.size())
                                       .arg(total)
                                 : "没有记录");
        updateSelection();
    });
    updateSummary();
    updateSelection();
    request();
    dialog->show();
}
