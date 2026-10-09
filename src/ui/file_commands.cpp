#include "file_commands.h"
#include "command_registry.h"
#include "folder_pane.h"
#include "main_window.h"
#include "application/operation_service.h"
#include "domain/file_operation.h"
#include <QApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QStatusBar>
#include <QToolButton>
#include <algorithm>

namespace mlex {
namespace {
class RenameDialog : public QInputDialog {
public:
    RenameDialog(QWidget *parent, int stemLength) : QInputDialog(parent), stemLength_(stemLength) {}
    void setVisible(bool visible) override {
        QInputDialog::setVisible(visible);
        if (visible) if (auto *edit = findChild<QLineEdit *>()) edit->setSelection(0, stemLength_);
    }
private:
    int stemLength_;
};
}
FileCommands::FileCommands(MainWindow *window, CommandRegistry *commands)
    : QObject(window), window_(window), commands_(commands), operations_(new OperationService(this)),
      jobLabel_(new QLabel(window)), cancelButton_(new QToolButton(window)) {
    jobLabel_->setTextFormat(Qt::PlainText);
    jobLabel_->setObjectName("jobStatus");
    jobLabel_->setMaximumWidth(500);
    cancelButton_->setText("中断");
    window_->statusBar()->addPermanentWidget(jobLabel_);
    window_->statusBar()->addPermanentWidget(cancelButton_);
    cancelButton_->hide();
    connect(cancelButton_, &QToolButton::clicked, operations_, &OperationService::cancel);
    connect(operations_, &OperationService::stateChanged, this, [this] {
        cancelButton_->setVisible(operations_->busy());
        cancelButton_->setEnabled(!operations_->cancelling());
        commands_->update();
    });
    connect(operations_, &OperationService::progress, this, [this](const QString &phase, qint64 bytes) {
        const auto text = phase + (bytes ? "  " + QLocale().formattedDataSize(bytes) : QString());
        jobLabel_->setText(text);
        if (origin_) origin_->showOperationStatus(text);
    });
    connect(operations_, &OperationService::finished, this, &FileCommands::completed);
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this] { commands_->update(); });
    auto add = [this](const QString &id, const QString &text, const QString &key, std::function<void()> run, std::function<bool()> condition) {
        commands_->add(id, text, key.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{QKeySequence(key)}, std::move(run), true, true);
        commands_->setCondition(id, std::move(condition));
    };
    auto idle = [this] { return fileContext() && !operations_->busy(); };
    auto selected = [this, idle] {
        if (!idle()) return false;
        const auto paths = window_->activePane()->operationPaths();
        return !paths.isEmpty() && std::all_of(paths.begin(), paths.end(), validOperationPath);
    };
    add("file.rename", "名前の変更(&M)…", "F2", [this] { rename(); }, [this, selected] { return selected() && window_->activePane()->operationPaths().size() == 1; });
    add("file.mkdir", "新しいフォルダ(&N)…", "Ctrl+Shift+N", [this] { createFolder(); }, [this, idle] { return idle() && !window_->activePane()->operationDirectory().isEmpty(); });
    add("file.copy", "コピー(&C)", "Ctrl+C", [this] { copy(false); }, selected);
    add("file.cut", "切り取り(&T)", "Ctrl+X", [this] { copy(true); }, selected);
    add("file.paste", "貼り付け(&P)", "Ctrl+V", [this] { paste(); }, [this, idle] {
        const auto *mime = QApplication::clipboard()->mimeData();
        return idle() && !window_->activePane()->operationDirectory().isEmpty() && mime && (mime->hasUrls() || mime->hasFormat("x-special/gnome-copied-files") || mime->hasFormat("application/x-mlex-file-transfer"));
    });
    add("file.trash", "ごみ箱へ移動(&D)…", "Delete", [this] { remove(false); }, selected);
    add("file.delete", "完全に削除…", "Shift+Delete", [this] { remove(true); }, selected);
    add("file.undo", "元に戻す(&U)", "Ctrl+Z", [this] { history(false); }, [this] { return fileContext() && operations_->canUndo(); });
    add("file.redo", "やり直し(&R)", "Ctrl+Y", [this] { history(true); }, [this] { return fileContext() && operations_->canRedo(); });
    commands_->add("file.cancel", "ファイル操作を中断", {}, [this] { operations_->cancel(); });
    commands_->setCondition("file.cancel", [this] { return operations_->busy() && !operations_->cancelling(); });
    auto *edit = new QMenu("編集(&E)", window);
    fillMenu(edit);
    const auto menus = window->menuBar()->actions();
    window->menuBar()->insertMenu(menus.size() > 1 ? menus[1] : nullptr, edit);
    edit->addSeparator();
    edit->addAction(commands_->get("file.cancel"));
    connect(edit, &QMenu::aboutToShow, commands_, &CommandRegistry::update);
}

bool FileCommands::fileContext() const { return window_->activePane() && window_->activePane()->fileViewFocused(); }
void FileCommands::error(const QString &message) {
    QMessageBox box(QMessageBox::Warning, "ファイル操作", message, QMessageBox::Ok, window_);
    box.setTextFormat(Qt::PlainText);
    box.exec();
}
void FileCommands::fillMenu(QMenu *menu) {
    for (const auto *id : {"file.undo", "file.redo", "", "file.cut", "file.copy", "file.paste", "", "file.mkdir", "file.rename", "file.trash", "file.delete"}) {
        if (*id) menu->addAction(commands_->get(id)); else menu->addSeparator();
    }
}
void FileCommands::showContextMenu(FolderPane *pane, const QPoint &position) {
    if (pane != window_->activePane()) return;
    commands_->update();
    QMenu menu(window_);
    fillMenu(&menu);
    menu.addSeparator();
    menu.addAction(commands_->get("file.properties"));
    menu.exec(position);
}

void FileCommands::rename() {
    auto *pane = window_->activePane();
    const auto paths = pane->operationPaths();
    if (paths.size() != 1) return;
    const auto items = captureItems(paths);
    const QFileInfo info(paths.first());
    const int dot = info.fileName().lastIndexOf('.');
    RenameDialog dialog(window_, !info.isDir() && dot > 0 ? dot : info.fileName().size());
    dialog.setWindowTitle("名前の変更");
    dialog.setLabelText("新しい名前");
    dialog.setTextValue(info.fileName());
    dialog.setOkButtonText("変更");
    dialog.setCancelButtonText("キャンセル");
    if (dialog.exec() != QDialog::Accepted) return;
    const auto name = dialog.textValue();
    if (!validFileName(name)) { error("名前が不正です。空文字、/、.、.. は使えません。"); return; }
    execute({{"kind", "rename"}, {"items", items}, {"destination", info.absolutePath()}, {"name", name}});
}
void FileCommands::createFolder() {
    const auto destination = window_->activePane()->operationDirectory();
    bool ok = false;
    const auto name = QInputDialog::getText(window_, "新しいフォルダ", "フォルダ名", QLineEdit::Normal, "新しいフォルダ", &ok);
    if (!ok) return;
    if (!validFileName(name)) { error("名前が不正です。空文字、/、.、.. は使えません。"); return; }
    execute({{"kind", "mkdir"}, {"destination", destination}, {"name", name}});
}
void FileCommands::copy(bool cut) {
    const auto paths = window_->activePane()->operationPaths();
    if (paths.isEmpty() || paths.size() > MaxOperationItems) { error("一度に操作できる項目は1〜256件です。"); return; }
    const auto items = captureItems(paths);
    auto mime = makeFileTransfer(items, cut);
    const auto transfer = readFileTransfer(mime.get());
    if (!transfer.error.isEmpty()) { error(transfer.error); return; }
    QApplication::clipboard()->setMimeData(mime.release());
    window_->activePane()->showOperationStatus(QString("%1 項目を%2。貼り付け先で Ctrl+V").arg(items.size()).arg(cut ? "切り取り予約" : "コピー"), 8000);
}
void FileCommands::paste() {
    const auto transfer = readFileTransfer(QApplication::clipboard()->mimeData());
    if (!transfer.error.isEmpty()) { error(transfer.error); return; }
    const auto destination = window_->activePane()->operationDirectory();
    bool collision = false;
    for (const auto &value : transfer.items) {
        const auto source = value.toObject().value("path").toString();
        const auto target = QDir(destination).filePath(QFileInfo(source).fileName());
        if (QFileInfo(source).canonicalPath() != QFileInfo(destination).canonicalFilePath() && !fileStamp(target).isEmpty()) collision = true;
    }
    QString policy = "skip";
    if (collision) {
        QMessageBox box(QMessageBox::Question, "同名の項目があります", "同名の項目をどのように扱いますか？", QMessageBox::NoButton, window_);
        auto *skip = box.addButton("同名をスキップ", QMessageBox::AcceptRole);
        auto *both = box.addButton("別名で両方を保持", QMessageBox::ActionRole);
        box.addButton("キャンセル", QMessageBox::RejectRole);
        box.setDefaultButton(skip);
        box.exec();
        if (box.clickedButton() == both) policy = "keep_both";
        else if (box.clickedButton() != skip) return;
    }
    pendingCut_ = transfer.cut ? transfer : FileTransfer{};
    execute({{"kind", transfer.cut ? "move" : "copy"}, {"items", transfer.items}, {"destination", destination}, {"policy", policy}});
}
void FileCommands::remove(bool permanent) {
    const auto paths = window_->activePane()->operationPaths();
    if (paths.isEmpty()) return;
    if (paths.size() > MaxOperationItems) { error("一度に操作できる項目は256件までです。"); return; }
    const auto items = captureItems(paths);
    QMessageBox box(QMessageBox::Warning, permanent ? "完全に削除" : "ごみ箱へ移動",
        QString("%1 項目を%2か？").arg(items.size()).arg(permanent ? "完全に削除します" : "ごみ箱へ移動します"), QMessageBox::Ok | QMessageBox::Cancel, window_);
    box.setTextFormat(Qt::PlainText);
    box.setInformativeText(permanent ? "この操作は元に戻せません。" : "復元するときはOSのごみ箱を使ってください。");
    QStringList names;
    for (const auto &value : items) names.append(value.toObject().value("path").toString());
    box.setDetailedText(names.join('\n'));
    box.button(QMessageBox::Ok)->setText(permanent ? "完全に削除" : "ごみ箱へ移動");
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() == QMessageBox::Ok) execute({{"kind", permanent ? "remove" : "trash"}, {"items", items}});
}
void FileCommands::history(bool redo) {
    origin_ = window_->activePane();
    originPath_ = origin_->path();
    pendingCut_ = {};
    QString message;
    if (!(redo ? operations_->redo(&message) : operations_->undo(&message))) { if (!message.isEmpty()) error(message); return; }
    jobLabel_->setText(redo ? "やり直し中" : "取り消し中");
}
void FileCommands::execute(const QJsonObject &request) {
    origin_ = window_->activePane();
    originPath_ = origin_->path();
    QString message;
    if (!operations_->start(request, &message)) { pendingCut_ = {}; error(message); return; }
    jobLabel_->setText("ファイル操作中");
    origin_->showOperationStatus("ファイル操作中");
}
void FileCommands::completed(const QJsonObject &result) {
    const auto successes = result.value("successes").toArray();
    if (pendingCut_.cut && pendingCut_.fingerprint == readFileTransfer(QApplication::clipboard()->mimeData()).fingerprint) {
        QSet<QString> moved;
        for (const auto &value : successes) {
            const auto item = value.toObject();
            if (item.value("kind") == "move") moved.insert(item.value("source").toString());
        }
        QJsonArray remaining;
        for (const auto &value : pendingCut_.items) if (!moved.contains(value.toObject().value("path").toString())) remaining.append(value);
        if (remaining.isEmpty()) QApplication::clipboard()->clear();
        else QApplication::clipboard()->setMimeData(makeFileTransfer(remaining, true).release());
    }
    pendingCut_ = {};
    window_->applyOperationResult(result);
    if (origin_ && origin_->path() == originPath_) {
        QStringList targets;
        for (const auto &value : successes) {
            const auto target = value.toObject().value("target").toString();
            if (!target.isEmpty() && QFileInfo(target).absolutePath() == originPath_) targets.append(target);
        }
        if (!targets.isEmpty()) origin_->selectPaths(targets);
    }
    const auto failures = result.value("failures").toArray();
    const bool changed = result.value("changed").toBool();
    QString text = QString("%1 件完了").arg(successes.size());
    if (!failures.isEmpty()) text += QString(" / %1 件失敗").arg(failures.size());
    if (result.value("cancelled").toBool()) text += " / 中断しました";
    if (changed && result.value("undo").toObject().isEmpty()) text += "（取り消し対象外）";
    jobLabel_->setText(text);
    if (origin_) origin_->showOperationStatus(text, 10000);
    QStringList details;
    for (const auto &value : failures) {
        const auto failure = value.toObject();
        details.append(failure.value("source").toString() + '\n' + failure.value("error").toString());
    }
    for (const auto &value : result.value("warnings").toArray()) if (!details.contains(value.toString())) details.append(value.toString());
    jobLabel_->setToolTip(details.join("\n\n"));
    if (!failures.isEmpty()) {
        QMessageBox box(QMessageBox::Warning, "ファイル操作の結果", text, QMessageBox::Ok, window_);
        box.setTextFormat(Qt::PlainText);
        box.setInformativeText(failures.first().toObject().value("error").toString());
        box.setDetailedText(details.join("\n\n"));
        box.exec();
    }
    commands_->update();
}
}
