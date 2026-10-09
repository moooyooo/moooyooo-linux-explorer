#include "main_window.h"
#include "command_registry.h"
#include "folder_pane.h"
#include "file_commands.h"
#include "application/operation_service.h"
#include "domain/project.h"
#include "persistence/project_store.h"
#include "persistence/recovery_store.h"
#include "platform/paths.h"

#include <QApplication>
#include <QAbstractButton>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonArray>
#include <QLayout>
#include <QLineEdit>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QStatusBar>
#include <QTabBar>
#include <cmath>

namespace mlex {
namespace {
QRect restoreRect(const QJsonObject &rect, const QSize &size) {
    const int width = qMax(1, size.width()), height = qMax(1, size.height());
    const int paneWidth = qBound(qMin(320, width), qRound(rect.value("width").toDouble() * width), width);
    const int paneHeight = qBound(qMin(220, height), qRound(rect.value("height").toDouble() * height), height);
    return QRect(qBound(0, qRound(rect.value("x").toDouble() * width), width - paneWidth),
                 qBound(0, qRound(rect.value("y").toDouble() * height), height - paneHeight), paneWidth, paneHeight);
}
QJsonObject saveRect(QRect rect, const QSize &size) {
    const double width = qMax(1, size.width()), height = qMax(1, size.height());
    const double w = qBound(0.01, rect.width() / width, 1.0), h = qBound(0.01, rect.height() / height, 1.0);
    return {{"x", qBound(0.0, rect.x() / width, 1.0 - w)}, {"y", qBound(0.0, rect.y() / height, 1.0 - h)},
            {"width", w}, {"height", h}};
}
}

QFileSystemModel *MainWindow::createModel(QObject *parent) {
    auto *model = new QFileSystemModel(parent);
    model->setReadOnly(true);
    model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    model->setOption(QFileSystemModel::DontUseCustomDirectoryIcons, true);
    model->setRootPath(QDir::rootPath());
    return model;
}

MainWindow::MainWindow(const QJsonObject &project, std::unique_ptr<ProjectStore> store, bool recoveryEnabled)
    : project_(project), mdi_(new QMdiArea(this)), model_(createModel(this)),
      commands_(new CommandRegistry(this, [this] { return activePane() != nullptr; })),
      store_(store ? std::move(store) : std::make_unique<ProjectStore>()) {
    setObjectName("workspace");
    setMinimumSize(640, 480);
    setCentralWidget(mdi_);
    mdi_->setObjectName("mdiArea");
    mdi_->setActivationOrder(QMdiArea::CreationOrder);
    mdi_->setTabsClosable(true);
    mdi_->setTabsMovable(true);
    mdi_->setDocumentMode(true);
    mdi_->setOption(QMdiArea::DontMaximizeSubWindowOnActivation, true);
    setupCommands();
    fileCommands_ = new FileCommands(this, commands_);
    connect(operations(), &OperationService::finished, this, [this] {
        if (closeWhenFinished_) { closeWhenFinished_ = false; QTimer::singleShot(0, this, &MainWindow::close); }
    });
    connect(mdi_, &QMdiArea::subWindowActivated, this, [this] { markDirty(); updateUi(); });
    connect(&recoveryDebounce_, &QTimer::timeout, this, &MainWindow::writeRecovery);
    connect(&recoveryDeadline_, &QTimer::timeout, this, &MainWindow::writeRecovery);
    recoveryDebounce_.setSingleShot(true);
    recoveryDebounce_.setInterval(2000);
    recoveryDeadline_.setSingleShot(true);
    recoveryDeadline_.setInterval(30000);
    if (recoveryEnabled) {
        QString error;
        const QString state = privateStateDirectory(&error);
        if (!state.isEmpty()) recovery_ = std::make_unique<RecoveryStore>(QDir(state).filePath("recovery"));
        else recoveryError_ = error;
    }
    restore(project);
}

MainWindow::~MainWindow() {
    closing_ = true;
    recoveryDebounce_.stop();
    recoveryDeadline_.stop();
    // QWidget deletes descendants after our C++ members are gone. Disconnect
    // deferred subwindow destruction first, and remove focus callbacks before UI teardown.
    for (auto *child : findChildren<QObject *>()) disconnect(child, nullptr, this, nullptr);
    delete commands_;
    commands_ = nullptr;
    delete fileCommands_;
    fileCommands_ = nullptr;
    delete takeCentralWidget();
    mdi_ = nullptr;
}

FolderPane *MainWindow::activePane() const {
    auto *window = mdi_->activeSubWindow();
    return window ? qobject_cast<FolderPane *>(window->widget()) : nullptr;
}

OperationService *MainWindow::operations() const { return fileCommands_->operations(); }

void MainWindow::applyOperationResult(const QJsonObject &result) {
    for (const auto &value : result.value("successes").toArray()) {
        const auto item = value.toObject();
        if (item.value("kind") != "rename" && item.value("kind") != "move") continue;
        const auto from = item.value("source").toString(), to = item.value("target").toString();
        if (from.isEmpty() || to.isEmpty()) continue;
        for (auto *sub : mdi_->subWindowList()) {
            auto *pane = qobject_cast<FolderPane *>(sub->widget());
            if (pane && (pane->path() == from || pane->path().startsWith(from + '/')))
                pane->navigate(to + pane->path().mid(from.size()));
        }
    }
    refreshFiles();
}

QMdiSubWindow *MainWindow::appendPane(const QJsonObject &state) {
    auto *pane = new FolderPane(model_, state);
    auto *sub = mdi_->addSubWindow(pane);
    sub->setAttribute(Qt::WA_DeleteOnClose);
    sub->setMinimumSize(320, 220);
    sub->setWindowTitle(pane->title());
    sub->setProperty("paneId", pane->id());
    paneOrder_.append(pane->id());
    sub->setProperty("savedWindowState", state.value("window_state").toString());
    sub->installEventFilter(this);
    connect(pane, &FolderPane::stateChanged, this, [this, sub, pane] { sub->setWindowTitle(pane->title()); markDirty(); updateUi(); });
    connect(pane, &FolderPane::selectionChanged, this, &MainWindow::updateUi);
    connect(pane, &FolderPane::refreshRequested, this, &MainWindow::refreshFiles);
    connect(pane, &FolderPane::contextMenuRequested, fileCommands_, [this, pane](const QPoint &position) { fileCommands_->showContextMenu(pane, position); });
    connect(sub, &QObject::destroyed, this, [this] { if (!closing_) { markDirty(); updateUi(); } });
    sub->show();
    pane->setFocus();
    const QRect geometry = restoreRect(state.value("rect").toObject(), mdi_->viewport()->size());
    sub->setGeometry(geometry);
    sub->setProperty("normalRect", sub->geometry());
    sub->setProperty("normalNormalizedRect", state.value("rect").toObject());
    if (state.value("window_state") == "maximized") sub->showMaximized();
    else if (state.value("window_state") == "minimized") sub->showMinimized();
    return sub;
}

void MainWindow::restore(const QJsonObject &project) {
    restoring_ = true;
    mdi_->closeAllSubWindows();
    paneOrder_.clear();
    project_ = project;
    arrangement_ = project.value("layout").toObject().value("arrangement").toString();
    const auto window = project.value("window").toObject();
    const auto screenSize = screen()->availableGeometry().size();
    resize(qMin(window.value("width").toInt(), screenSize.width()), qMin(window.value("height").toInt(), screenSize.height()));
    // Geometry must be applied after the central widget has its final layout size.
    layout()->activate();
    QMdiSubWindow *active = nullptr;
    for (const auto &state : project.value("panes").toArray()) {
        auto *sub = appendPane(state.toObject());
        if (state.toObject().value("pane_id") == project.value("active_pane_id")) active = sub;
    }
    if (active) mdi_->setActiveSubWindow(active);
    if (project.value("layout").toObject().value("mode") == "tabs") setTabbed(true);
    if (window.value("maximized").toBool()) setWindowState(windowState() | Qt::WindowMaximized);
    dirty_ = false;
    restoring_ = false;
    updateUi();
}

void MainWindow::addPane() {
    if (mdi_->subWindowList().size() >= 64) {
        QMessageBox::information(this, "内部ファイラ", "1つの業務ウィンドウに開ける内部ファイラは64個までです。");
        return;
    }
    auto state = makePane(QDir::homePath());
    if (activePane()) state["view"] = activePane()->state().value("view");
    const double offset = 0.025 * (mdi_->subWindowList().size() % 8);
    state["rect"] = QJsonObject{{"x", offset}, {"y", offset}, {"width", 0.72}, {"height", 0.78}};
    auto *sub = appendPane(state);
    mdi_->setActiveSubWindow(sub);
    markDirty();
    updateUi();
}

QList<QMdiSubWindow *> MainWindow::orderedWindows() const {
    const auto windows = mdi_->subWindowList(QMdiArea::CreationOrder);
    QList<QMdiSubWindow *> result;
    for (const auto &id : paneOrder_)
        for (auto *sub : windows)
            if (sub->property("paneId") == id && !sub->property("closing").toBool()) result.append(sub);
    return result;
}

QJsonObject MainWindow::snapshot() const {
    QJsonObject result = project_;
    const bool maximized = isMaximized() || isFullScreen();
    const QSize size = maximized ? normalGeometry().size() : this->size();
    result["window"] = QJsonObject{{"width", qBound(640, size.width(), 16384)}, {"height", qBound(480, size.height(), 16384)}, {"maximized", isMaximized()}};
    const bool tabs = mdi_->viewMode() == QMdiArea::TabbedView;
    result["layout"] = QJsonObject{{"mode", tabs ? "tabs" : "mdi"}, {"arrangement", arrangement_}};
    QJsonArray panes;
    for (auto *sub : orderedWindows()) {
        auto *pane = qobject_cast<FolderPane *>(sub->widget());
        if (!pane || sub->property("closing").toBool()) continue;
        auto state = pane->state();
        const QRect rect = (tabs || sub->isMinimized() || sub->isMaximized()) ? sub->property("normalRect").toRect() : sub->geometry();
        state["rect"] = tabs ? sub->property("normalNormalizedRect").toJsonObject() : saveRect(rect, mdi_->viewport()->size());
        state["window_state"] = tabs ? sub->property("savedWindowState").toString() : sub->isMaximized() ? "maximized" : sub->isMinimized() ? "minimized" : "normal";
        panes.append(state);
    }
    result["panes"] = panes;
    QString active = activePane() ? activePane()->id() : QString();
    bool found = false;
    for (const auto &value : panes) if (value.toObject().value("pane_id") == active) found = true;
    if (!found && !panes.isEmpty()) active = panes.first().toObject().value("pane_id").toString();
    result["active_pane_id"] = panes.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(active);
    return result;
}

void MainWindow::setTabbed(bool tabbed) {
    if (tabbed == (mdi_->viewMode() == QMdiArea::TabbedView)) return;
    const bool previous = restoring_;
    restoring_ = true;
    if (tabbed) {
        for (auto *sub : mdi_->subWindowList()) {
            if (!sub->isMinimized() && !sub->isMaximized()) sub->setProperty("normalRect", sub->geometry());
            sub->setProperty("normalNormalizedRect", saveRect(sub->property("normalRect").toRect(), mdi_->viewport()->size()));
            sub->setProperty("savedWindowState", sub->isMaximized() ? "maximized" : sub->isMinimized() ? "minimized" : "normal");
        }
        mdi_->setViewMode(QMdiArea::TabbedView);
        if (auto *bar = mdi_->findChild<QTabBar *>()) {
            bar->setMovable(true);
            if (!bar->property("orderConnected").toBool()) {
                connect(bar, &QTabBar::tabMoved, this, [this](int from, int to) {
                    if (restoring_) return;
                    QStringList order;
                    for (auto *sub : orderedWindows()) order.append(sub->property("paneId").toString());
                    if (from < 0 || to < 0 || from >= order.size() || to >= order.size()) return;
                    order.move(from, to);
                    paneOrder_ = order;
                    markDirty();
                });
                bar->setProperty("orderConnected", true);
            }
        }
    } else {
        mdi_->setViewMode(QMdiArea::SubWindowView);
        for (auto *sub : mdi_->subWindowList()) {
            sub->showNormal();
            sub->setGeometry(restoreRect(sub->property("normalNormalizedRect").toJsonObject(), mdi_->viewport()->size()));
            sub->setProperty("normalRect", sub->geometry());
            if (sub->property("savedWindowState") == "maximized") sub->showMaximized();
            else if (sub->property("savedWindowState") == "minimized") sub->showMinimized();
        }
    }
    restoring_ = previous;
    markDirty();
    updateUi();
}

void MainWindow::arrange(const QString &mode) {
    setTabbed(false);
    const auto windows = orderedWindows();
    if (windows.isEmpty()) return;
    restoring_ = true;
    for (auto *sub : windows) sub->showNormal();
    if (mode == "free") mdi_->cascadeSubWindows();
    else {
        const int count = windows.size();
        const int cols = mode == "tile_vertical" ? count : mode == "tile_horizontal" ? 1 : qCeil(std::sqrt(count));
        const int rows = (count + cols - 1) / cols;
        const QSize size = mdi_->viewport()->size();
        for (int i = 0; i < count; ++i) {
            const int col = i % cols, row = i / cols;
            const int left = col * size.width() / cols, top = row * size.height() / rows;
            windows[i]->setGeometry(left, top, (col + 1) * size.width() / cols - left, (row + 1) * size.height() / rows - top);
        }
    }
    for (auto *sub : windows) { sub->setProperty("normalRect", sub->geometry()); sub->setProperty("savedWindowState", "normal"); }
    arrangement_ = mode;
    restoring_ = false;
    markDirty();
}

void MainWindow::refreshFiles() {
    auto *old = model_;
    model_ = createModel(this);
    for (auto *sub : mdi_->subWindowList()) {
        auto *pane = qobject_cast<FolderPane *>(sub->widget());
        pane->setFileSystemModel(model_);
        pane->navigate(pane->path(), false);
    }
    old->deleteLater();
    updateUi();
}

void MainWindow::markDirty() {
    if (restoring_ || closing_ || !initialized_) return;
    dirty_ = true;
    if (recovery_) {
        recoveryDebounce_.start();
        if (!recoveryDeadline_.isActive()) recoveryDeadline_.start();
    }
    updateUi();
}

void MainWindow::updateUi() {
    setWindowTitle(project_.value("name").toString() + (dirty_ ? " *" : "") + (store_->readOnly() ? " [上書き不可]" : "") + " — moooyooo Linux Explorer");
    commands_->update();
    if (auto *pane = activePane()) {
        commands_->get("view.hidden")->setChecked(pane->hidden());
        commands_->get("view.tree")->setChecked(pane->treeVisible());
        commands_->get("view.icons")->setChecked(pane->icons());
        statusBar()->showMessage(QString("%1 内部ファイラ").arg(mdi_->subWindowList().size()));
    } else statusBar()->showMessage("Ctrl+N で内部ファイラを追加");
    commands_->get("layout.tabs")->setChecked(mdi_->viewMode() == QMdiArea::TabbedView);
    if (!recoveryError_.isEmpty()) statusBar()->showMessage(recoveryError_);
}

bool MainWindow::saveTo(const QString &path, QString *error) {
    auto next = snapshot();
    const bool success = path.isEmpty() ? store_->save(&next, error) : store_->saveAs(path, &next, error);
    if (!success) return false;
    project_ = next;
    dirty_ = false;
    recoveryDebounce_.stop();
    recoveryDeadline_.stop();
    if (recovery_) recovery_->clear();
    updateUi();
    return true;
}

bool MainWindow::save(bool as) {
    QString path;
    if (as || store_->path().isEmpty() || store_->readOnly()) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "プロジェクトを保存", "業務の名前", QLineEdit::Normal, project_.value("name").toString(), &ok).trimmed();
        if (!ok || name.isEmpty()) return false;
        if (name.toUcs4().size() > 120) { QMessageBox::warning(this, "保存", "業務の名前は120文字以内にしてください。"); return false; }
        QString error;
        const QString data = privateDataDirectory(&error);
        QString directory;
        if (!data.isEmpty()) {
            directory = QDir(data).filePath("projects");
            if (!preparePrivateDirectory(directory, &error)) directory.clear();
        }
        if (directory.isEmpty()) { QMessageBox::warning(this, "保存", error); return false; }
        path = QFileDialog::getSaveFileName(this, "プロジェクトを保存（新しいファイル名）", QDir(directory).filePath(newId() + ".mlex-project.json"), "Explorer project (*.mlex-project.json)");
        if (path.isEmpty()) return false;
        if (!path.endsWith(".mlex-project.json")) path += ".mlex-project.json";
        project_["name"] = name;
        markDirty();
    }
    QString error;
    if (!saveTo(path, &error)) { QMessageBox::warning(this, "保存できません", error); return false; }
    return true;
}

void MainWindow::launchParent(const QStringList &arguments) {
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments))
        QMessageBox::warning(this, "新しい業務ウィンドウ", "新しいプロセスを起動できませんでした。");
}

void MainWindow::writeRecovery() {
    recoveryDebounce_.stop();
    recoveryDeadline_.stop();
    if (!dirty_ || !recovery_) return;
    QString error;
    recoveryError_ = recovery_->write(snapshot(), &error) ? QString() : error;
    updateUi();
}

void MainWindow::offerRecovery() {
    if (!recovery_) return;
    for (const auto &path : recovery_->candidates()) {
        auto claim = RecoveryStore::claim(path);
        if (!claim) continue;
        QFile file(path);
        QJsonObject project;
        QString error;
        if (!file.open(QIODevice::ReadOnly) || !decodeProject(file.read(MaxProjectBytes + 1), &project, &error)) {
            statusBar()->showMessage("読み取れない復元記録があります。元の記録は保持しています。");
            continue;
        }
        file.close();
        QMessageBox box(QMessageBox::Question, "前回の未保存状態", "前回の未保存プロジェクトを復元しますか？", QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, this);
        box.setInformativeText(project.value("name").toString());
        box.setTextFormat(Qt::PlainText);
        box.button(QMessageBox::Yes)->setText("復元");
        box.button(QMessageBox::No)->setText("破棄");
        box.button(QMessageBox::Cancel)->setText("後で");
        const int choice = box.exec();
        if (choice == QMessageBox::Cancel) return;
        if (choice == QMessageBox::Yes) {
            store_ = std::make_unique<ProjectStore>();
            restore(project);
            markDirty();
            // Retain the old record unless the replacement recovery is safely stored.
            if (recovery_->write(snapshot(), &error)) QFile::remove(path);
            return;
        }
        QFile::remove(path);
    }
}

bool MainWindow::eventFilter(QObject *object, QEvent *event) {
    auto *sub = qobject_cast<QMdiSubWindow *>(object);
    if (sub && !restoring_ && !closing_) {
        if (event->type() == QEvent::Close) { sub->setProperty("closing", true); markDirty(); }
        if (mdi_->viewMode() == QMdiArea::SubWindowView && (event->type() == QEvent::Move || event->type() == QEvent::Resize || event->type() == QEvent::WindowStateChange)) {
            if (!sub->isMinimized() && !sub->isMaximized()) sub->setProperty("normalRect", sub->geometry());
            sub->setProperty("savedWindowState", sub->isMaximized() ? "maximized" : sub->isMinimized() ? "minimized" : "normal");
            markDirty();
        }
    }
    return QMainWindow::eventFilter(object, event);
}

void MainWindow::resizeEvent(QResizeEvent *event) { QMainWindow::resizeEvent(event); if (isVisible()) markDirty(); }

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    if (initialShowScheduled_) return;
    initialShowScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        restoring_ = true;
        const bool tabs = mdi_->viewMode() == QMdiArea::TabbedView;
        if (tabs) setTabbed(false);
        for (auto *sub : orderedWindows()) {
            for (const auto &value : project_.value("panes").toArray()) {
                const auto state = value.toObject();
                if (state.value("pane_id").toString() != sub->property("paneId").toString()) continue;
                sub->showNormal();
                sub->setGeometry(restoreRect(state.value("rect").toObject(), mdi_->viewport()->size()));
                sub->setProperty("normalRect", sub->geometry());
                if (state.value("window_state") == "maximized") sub->showMaximized();
                else if (state.value("window_state") == "minimized") sub->showMinimized();
            }
        }
        if (tabs) setTabbed(true);
        restoring_ = false;
        initialized_ = true;
        dirty_ = false;
        updateUi();
    });
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (operations()->busy()) {
        QMessageBox box(QMessageBox::Question, "ファイル操作中", "終了する前にファイル操作の完了を待ちます。", QMessageBox::NoButton, this);
        auto *wait = box.addButton("完了したら終了", QMessageBox::AcceptRole);
        auto *cancel = box.addButton("中断して終了", QMessageBox::DestructiveRole);
        box.addButton("戻る", QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == wait || box.clickedButton() == cancel) {
            closeWhenFinished_ = true;
            if (box.clickedButton() == cancel) operations()->cancel();
            if (!operations()->busy()) { closeWhenFinished_ = false; QTimer::singleShot(0, this, &MainWindow::close); }
        }
        event->ignore();
        return;
    }
    if (dirty_) {
        const auto choice = QMessageBox::question(this, "プロジェクトの変更", "変更したフォルダ構成・配置を保存しますか？", QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (choice == QMessageBox::Cancel || (choice == QMessageBox::Save && !save(false))) { event->ignore(); return; }
    }
    closing_ = true;
    recoveryDebounce_.stop();
    recoveryDeadline_.stop();
    if (recovery_) recovery_->clear();
    event->accept();
}

void MainWindow::rebuildWindowMenu() {
    windowMenu_->clear();
    for (const auto &id : {"pane.new", "workspace.new", "pane.close", "pane.next", "pane.previous"}) windowMenu_->addAction(commands_->get(id));
    windowMenu_->addSeparator();
    for (const auto &id : {"layout.tabs", "layout.cascade", "layout.vertical", "layout.horizontal", "layout.grid"}) windowMenu_->addAction(commands_->get(id));
    windowMenu_->addSeparator();
    int index = 0;
    for (auto *sub : orderedWindows()) {
        auto *action = windowMenu_->addAction(QString("%1  %2").arg(++index).arg(sub->windowTitle().replace('&', "&&")));
        action->setCheckable(true);
        action->setChecked(sub == mdi_->activeSubWindow());
        connect(action, &QAction::triggered, sub, [this, sub] { mdi_->setActiveSubWindow(sub); });
    }
}

void MainWindow::setupCommands() {
    auto add = [this](const QString &id, const QString &text, const QStringList &keys, std::function<void()> run, bool pane = false, bool outsideText = false) {
        QList<QKeySequence> shortcuts;
        for (const auto &key : keys) shortcuts.append(QKeySequence(key));
        return commands_->add(id, text, shortcuts, std::move(run), pane, outsideText);
    };
    add("pane.new", "内部ファイラを追加(&N)", {"Ctrl+N", "Ctrl+T"}, [this] { addPane(); });
    add("pane.close", "内部ファイラを閉じる(&W)", {"Ctrl+W", "Ctrl+F4"}, [this] { mdi_->activeSubWindow()->close(); }, true);
    auto cycle = [this](int direction) {
        const auto windows = orderedWindows();
        const int current = windows.indexOf(mdi_->activeSubWindow());
        mdi_->setActiveSubWindow(windows[(current + direction + windows.size()) % windows.size()]);
    };
    add("pane.next", "次の内部ファイラ", {"Ctrl+Tab", "Ctrl+F6"}, [cycle] { cycle(1); }, true);
    add("pane.previous", "前の内部ファイラ", {"Ctrl+Shift+Tab", "Ctrl+Shift+F6"}, [cycle] { cycle(-1); }, true);
    add("workspace.new", "新しい業務ウィンドウ", {"Ctrl+Alt+N"}, [this] { launchParent(); });
    add("workspace.close", "業務ウィンドウを閉じる", {"Alt+F4"}, [this] { close(); });
    add("project.open", "プロジェクトを開く(&O)…", {"Ctrl+O"}, [this] {
        const QString directory = store_->path().isEmpty() ? QDir(privateDataDirectory()).filePath("projects") : QFileInfo(store_->path()).absolutePath();
        const QString path = QFileDialog::getOpenFileName(this, "プロジェクトを新しい業務ウィンドウで開く", directory, "Explorer project (*.mlex-project.json *.json)");
        if (path.isEmpty()) return;
        QFile file(path);
        QJsonObject project;
        QString error;
        if (!file.open(QIODevice::ReadOnly) || !decodeProject(file.read(MaxProjectBytes + 1), &project, &error)) {
            QMessageBox::warning(this, "開けません", error.isEmpty() ? "プロジェクトを読み取れません。" : error);
            return;
        }
        launchParent({"--project", path});
    });
    add("project.save", "プロジェクトを保存(&S)", {"Ctrl+S"}, [this] { save(false); });
    add("project.save_as", "プロジェクトを別名で保存…", {"Ctrl+Shift+S"}, [this] { save(true); });
    add("view.fullscreen", "全画面", {"F11"}, [this] { isFullScreen() ? showNormal() : showFullScreen(); });
    add("navigate.back", "戻る", {"Alt+Left"}, [this] { activePane()->back(); }, true);
    add("navigate.backspace", "戻る", {"Backspace"}, [this] { activePane()->back(); }, true, true);
    add("navigate.forward", "進む", {"Alt+Right"}, [this] { activePane()->forward(); }, true);
    add("navigate.up", "上のフォルダ", {"Alt+Up"}, [this] { activePane()->up(); }, true);
    add("navigate.address", "場所を入力", {"Ctrl+L", "Alt+D"}, [this] { activePane()->focusAddress(); }, true);
    add("navigate.history", "移動履歴", {"F4"}, [this] { activePane()->focusAddress(true); }, true);
    add("navigate.search", "このフォルダを名前で検索", {"Ctrl+F", "Ctrl+E", "F3"}, [this] { activePane()->focusSearch(); }, true);
    add("navigate.refresh", "再読み込み", {"F5", "Ctrl+R"}, [this] { refreshFiles(); }, true);
    add("navigate.expand", "ツリーで現在の場所を表示", {"Ctrl+Shift+E"}, [this] { activePane()->expandAncestors(); }, true);
    add("focus.next", "次の領域", {"F6"}, [this] { activePane()->focusNextRegion(false); }, true);
    add("focus.previous", "前の領域", {"Shift+F6"}, [this] { activePane()->focusNextRegion(true); }, true);
    add("navigate.cancel", "入力を戻す／検索を解除", {"Escape"}, [this] { activePane()->clearSearch(); }, true);
    add("selection.all", "すべて選択", {"Ctrl+A"}, [this] { activePane()->selectAll(); }, true, true);
    add("file.properties", "プロパティ", {"Alt+Return"}, [this] { activePane()->showProperties(); }, true, true);
    add("view.icons", "アイコン表示", {}, [this] { activePane()->setViewMode(activePane()->icons() ? "details" : "icons"); }, true)->setCheckable(true);
    add("view.hidden", "隠しファイルを表示", {"Ctrl+H"}, [this] { activePane()->setHidden(!activePane()->hidden()); }, true)->setCheckable(true);
    add("view.tree", "フォルダツリーを表示", {}, [this] { activePane()->setTreeVisible(!activePane()->treeVisible()); }, true)->setCheckable(true);
    add("layout.tabs", "タブ表示", {}, [this] { setTabbed(mdi_->viewMode() != QMdiArea::TabbedView); })->setCheckable(true);
    add("layout.cascade", "重ねて表示", {}, [this] { arrange("free"); });
    add("layout.vertical", "左右に並べる", {}, [this] { arrange("tile_vertical"); });
    add("layout.horizontal", "上下に並べる", {}, [this] { arrange("tile_horizontal"); });
    add("layout.grid", "格子状に並べる", {}, [this] { arrange("tile_grid"); });
    auto *file = menuBar()->addMenu("ファイル(&F)");
    for (const auto &id : {"pane.new", "workspace.new", "project.open", "project.save", "project.save_as"}) file->addAction(commands_->get(id));
    file->addSeparator();
    file->addAction(commands_->get("file.properties"));
    file->addAction(commands_->get("workspace.close"));
    auto *view = menuBar()->addMenu("表示(&V)");
    for (const auto &id : {"view.icons", "view.hidden", "view.tree", "navigate.refresh", "view.fullscreen"}) view->addAction(commands_->get(id));
    auto *go = menuBar()->addMenu("移動(&G)");
    for (const auto &id : {"navigate.back", "navigate.forward", "navigate.up", "navigate.address", "navigate.history", "navigate.search"}) go->addAction(commands_->get(id));
    windowMenu_ = menuBar()->addMenu("ウィンドウ(&W)");
    connect(windowMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildWindowMenu);
    auto *help = menuBar()->addMenu("ヘルプ(&H)");
    help->addAction("このアプリについて", this, [this] {
        QMessageBox::about(this, "moooyooo Linux Explorer", "MDIファイラ・プロジェクト保存\nCtrl+N: 内部ファイラを追加（ホーム）\nCtrl+Alt+N: 新しい業務ウィンドウ\nCtrl+S: フォルダ構成を保存\n\nF2: 名前変更 / Ctrl+Shift+N: フォルダ作成\nCtrl+C・X・V: コピー・切り取り・貼り付け\nDelete: ごみ箱 / Shift+Delete: 完全削除\nCtrl+Z・Y: 取り消し・やり直し\n\nごみ箱・完全削除・異なるファイルシステムへの移動は取り消し対象外です。");
    });
}
}
