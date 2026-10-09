#include "folder_pane.h"
#include "domain/project.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCollator>
#include <QCompleter>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QMenu>
#include <QStatusBar>
#include <QKeyEvent>
#include <limits>
#include <QMimeDatabase>
#include <QPointer>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QTreeView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace mlex {
namespace {
const QStringList columns{"name", "size", "type", "modified"};
QToolButton *button(QWidget *parent, QStyle::StandardPixmap icon, const QString &tip) {
    auto *result = new QToolButton(parent);
    result->setIcon(parent->style()->standardIcon(icon));
    result->setToolTip(tip);
    result->setAutoRaise(true);
    return result;
}
}

DirectoryProxy::DirectoryProxy(bool tree, QObject *parent) : QSortFilterProxyModel(parent), tree_(tree) {
    setDynamicSortFilter(true);
    // Qt's C-locale collator does not support numeric mode.
    if (collator_.locale().language() == QLocale::C) collator_.setLocale(QLocale(QLocale::English, QLocale::UnitedStates));
    collator_.setNumericMode(true);
    collator_.setCaseSensitivity(Qt::CaseInsensitive);
}

void DirectoryProxy::configure(const QString &root, bool hidden, bool directoriesFirst, const QString &search) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif
    root_ = root;
    hidden_ = hidden;
    directoriesFirst_ = directoriesFirst;
    search_ = search;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    invalidateFilter();
#endif
}

bool DirectoryProxy::filterAcceptsRow(int row, const QModelIndex &parent) const {
    const auto *model = qobject_cast<const QFileSystemModel *>(sourceModel());
    const auto info = model->fileInfo(model->index(row, 0, parent));
    const QString path = info.absoluteFilePath();
    if (path == root_ || root_.startsWith(path.endsWith('/') ? path : path + '/')) return true;
    if (!hidden_ && info.isHidden()) return false;
    if (tree_) return info.isDir();
    if (model->filePath(parent) == root_ && !search_.isEmpty())
        return info.fileName().contains(search_, Qt::CaseInsensitive);
    return true;
}

bool DirectoryProxy::lessThan(const QModelIndex &left, const QModelIndex &right) const {
    const auto *model = qobject_cast<const QFileSystemModel *>(sourceModel());
    const auto a = model->fileInfo(left), b = model->fileInfo(right);
    if (directoriesFirst_ && a.isDir() != b.isDir()) return sortOrder() == Qt::AscendingOrder ? a.isDir() : !a.isDir();
    if (left.column() == 1 && a.size() != b.size()) return a.size() < b.size();
    if (left.column() == 3 && a.lastModified() != b.lastModified()) return a.lastModified() < b.lastModified();
    if (left.column() == 2) {
        const int type = collator_.compare(model->type(left), model->type(right));
        if (type != 0) return type < 0;
    }
    const int name = collator_.compare(a.fileName(), b.fileName());
    return name == 0 ? a.fileName() < b.fileName() : name < 0;
}

QVariant DirectoryProxy::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        const QStringList labels{"名前", "サイズ", "種類", "更新日時"};
        if (section >= 0 && section < labels.size()) return labels[section];
    }
    return QSortFilterProxyModel::headerData(section, orientation, role);
}

QVariant DirectoryProxy::data(const QModelIndex &index, int role) const {
    const auto base = QSortFilterProxyModel::data(index, role);
    if (role == Qt::DecorationRole && index.column() == 0 && qvariant_cast<QIcon>(base).isNull()) {
        const auto *model = qobject_cast<const QFileSystemModel *>(sourceModel());
        return QApplication::style()->standardIcon(model->isDir(mapToSource(index)) ? QStyle::SP_DirIcon : QStyle::SP_FileIcon);
    }
    if (role == Qt::DisplayRole && index.column() == 1) {
        const auto *model = qobject_cast<const QFileSystemModel *>(sourceModel());
        const auto source = mapToSource(index);
        if (model->isDir(source)) return {};
        const auto size = model->size(source);
        if (size < 1024) return QString::number(size) + " B";
        if (size < 1024 * 1024) return QString::number(size / 1024.0, 'f', 1) + " KiB";
        if (size < 1024LL * 1024 * 1024) return QString::number(size / (1024.0 * 1024), 'f', 1) + " MiB";
        return QString::number(size / (1024.0 * 1024 * 1024), 'f', 1) + " GiB";
    }
    return base;
}

FolderPane::FolderPane(QFileSystemModel *model, const QJsonObject &state, QWidget *parent)
    : QWidget(parent), id_(state.value("pane_id").toString()), view_(state.value("view").toObject()),
      listProxy_(new DirectoryProxy(false, this)), treeProxy_(new DirectoryProxy(true, this)) {
    setObjectName("folderPane");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(3);
    auto *navigation = new QHBoxLayout;
    navigation->setSpacing(2);
    back_ = button(this, QStyle::SP_ArrowBack, "戻る (Alt+←)");
    forward_ = button(this, QStyle::SP_ArrowForward, "進む (Alt+→)");
    auto *parentButton = button(this, QStyle::SP_ArrowUp, "上へ (Alt+↑)");
    auto *refresh = button(this, QStyle::SP_BrowserReload, "再読み込み (F5)");
    address_ = new QLineEdit(this);
    address_->setObjectName("address");
    address_->setAccessibleName("フォルダの場所");
    address_->setPlaceholderText("フォルダの場所 (Ctrl+L)");
    address_->setMinimumWidth(80);
    search_ = new QLineEdit(this);
    search_->setObjectName("search");
    search_->setAccessibleName("このフォルダの名前検索");
    search_->setPlaceholderText("名前検索 (Ctrl+F)");
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(90);
    search_->setMaximumWidth(240);
    for (auto *item : {back_, forward_, parentButton, refresh}) navigation->addWidget(item);
    navigation->addWidget(address_, 3);
    navigation->addWidget(search_, 1);
    layout->addLayout(navigation);
    error_ = new QLabel(this);
    error_->setObjectName("locationError");
    error_->setWordWrap(true);
    error_->setTextFormat(Qt::PlainText);
    error_->hide();
    layout->addWidget(error_);
    splitter_ = new QSplitter(this);
    tree_ = new QTreeView(splitter_);
    tree_->setObjectName("folderTree");
    tree_->setHeaderHidden(true);
    tree_->setModel(treeProxy_);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setUniformRowHeights(true);
    tree_->setMinimumWidth(80);
    views_ = new QStackedWidget(splitter_);
    details_ = new QTreeView(views_);
    details_->setObjectName("details");
    details_->setRootIsDecorated(false);
    details_->setItemsExpandable(false);
    details_->setUniformRowHeights(true);
    details_->setSortingEnabled(true);
    details_->header()->setSectionsMovable(true);
    details_->header()->setStretchLastSection(false);
    icons_ = new QListView(views_);
    icons_->setObjectName("icons");
    icons_->setViewMode(QListView::IconMode);
    icons_->setResizeMode(QListView::Adjust);
    icons_->setMovement(QListView::Static);
    icons_->setWrapping(true);
    icons_->setWordWrap(true);
    icons_->setUniformItemSizes(true);
    for (QAbstractItemView *item : {static_cast<QAbstractItemView *>(details_), static_cast<QAbstractItemView *>(icons_)}) {
        item->setModel(listProxy_);
        item->setSelectionMode(QAbstractItemView::ExtendedSelection);
        item->setSelectionBehavior(QAbstractItemView::SelectRows);
        item->setEditTriggers(QAbstractItemView::NoEditTriggers);
        item->setDragDropMode(QAbstractItemView::NoDragDrop);
        item->viewport()->installEventFilter(this);
        connect(item, &QAbstractItemView::activated, this, &FolderPane::activate);
    }
    icons_->setSelectionModel(details_->selectionModel());
    connect(details_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &FolderPane::selectionChanged);
    views_->addWidget(details_);
    views_->addWidget(icons_);
    splitter_->setStretchFactor(1, 1);
    splitter_->setChildrenCollapsible(false);
    layout->addWidget(splitter_, 1);
    status_ = new QStatusBar(this);
    status_->setObjectName("paneStatusBar");
    status_->setSizeGripEnabled(false);
    summary_ = new QLabel(status_);
    summary_->setObjectName("paneSummary");
    summary_->setTextFormat(Qt::PlainText);
    summary_->setMinimumWidth(0);
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    status_->addWidget(summary_, 1);
    layout->addWidget(status_);
    statusUpdate_.setSingleShot(true);
    connect(&statusUpdate_, &QTimer::timeout, this, &FolderPane::updateStatus);
    auto queueStatus = [this] { if (!statusUpdate_.isActive()) statusUpdate_.start(0); };
    connect(this, &FolderPane::selectionChanged, this, queueStatus);
    connect(listProxy_, &QAbstractItemModel::rowsInserted, this, queueStatus);
    connect(listProxy_, &QAbstractItemModel::rowsRemoved, this, queueStatus);
    connect(listProxy_, &QAbstractItemModel::layoutChanged, this, queueStatus);
    connect(listProxy_, &QAbstractItemModel::modelReset, this, queueStatus);
    connect(listProxy_, &QAbstractItemModel::dataChanged, this, queueStatus);
    for (QAbstractItemView *view : {static_cast<QAbstractItemView *>(tree_), static_cast<QAbstractItemView *>(details_), static_cast<QAbstractItemView *>(icons_)}) {
        view->installEventFilter(this);
        view->viewport()->installEventFilter(this);
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(view, &QWidget::customContextMenuRequested, this, [this, view](const QPoint &position) {
            view->setFocus();
            const auto index = view->indexAt(position);
            if (index.isValid() && !view->selectionModel()->isSelected(index))
                view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            else if (!index.isValid()) view->selectionModel()->clearSelection();
            emit contextMenuRequested(view->viewport()->mapToGlobal(position));
        });
    }
    connect(tree_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &FolderPane::selectionChanged);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *focus) {
        if (!focus || !isAncestorOf(focus)) return;
        lastFileContext_ = focus == tree_ || tree_->isAncestorOf(focus) || focus == details_ || details_->isAncestorOf(focus) || focus == icons_ || icons_->isAncestorOf(focus);
        if (lastFileContext_) treeFocused_ = focus == tree_ || tree_->isAncestorOf(focus);
        emit selectionChanged();
    });
    connect(back_, &QToolButton::clicked, this, &FolderPane::back);
    connect(forward_, &QToolButton::clicked, this, &FolderPane::forward);
    connect(parentButton, &QToolButton::clicked, this, &FolderPane::up);
    connect(refresh, &QToolButton::clicked, this, &FolderPane::refreshRequested);
    connect(address_, &QLineEdit::returnPressed, this, [this] {
        navigate(QDir::isAbsolutePath(address_->text()) ? address_->text() : QDir(path_).absoluteFilePath(address_->text()));
    });
    connect(search_, &QLineEdit::textChanged, this, [this] { updateRoots(); });
    connect(tree_, &QTreeView::clicked, this, [this](const QModelIndex &index) { navigate(model_->filePath(treeProxy_->mapToSource(index))); });
    connect(details_->header(), &QHeaderView::sortIndicatorChanged, this, [this](int column, Qt::SortOrder order) {
        if (applying_ || column < 0 || column >= columns.size()) return;
        view_["sort_by"] = columns[column];
        view_["sort_order"] = order == Qt::AscendingOrder ? "asc" : "desc";
        emit stateChanged();
    });
    connect(details_->header(), &QHeaderView::sectionMoved, this, [this] {
        if (!applying_) { view_["columns"] = this->state().value("view").toObject().value("columns"); emit stateChanged(); }
    });
    connect(splitter_, &QSplitter::splitterMoved, this, [this] {
        if (!applying_ && treeVisible()) { view_["tree_width"] = qBound(120, splitter_->sizes().value(0), 600); emit stateChanged(); }
    });
    setFileSystemModel(model);
    applyView();
    navigate(resolveLocation(state.value("location").toObject()));
}

QString FolderPane::title() const { const QString name = QFileInfo(path_).fileName(); return name.isEmpty() ? path_ : name; }

FolderPane::~FolderPane() {
    statusUpdate_.stop();
    disconnect(qApp, nullptr, this, nullptr);
    if (model_) disconnect(model_, nullptr, this, nullptr);
    for (auto *child : findChildren<QObject *>()) disconnect(child, nullptr, this, nullptr);
}

void FolderPane::setFileSystemModel(QFileSystemModel *model) {
    if (model_) {
        pendingSelection_ = selectedPaths();
        disconnect(model_, nullptr, this, nullptr);
    }
    model_ = model;
    listProxy_->setSourceModel(model);
    treeProxy_->setSourceModel(model);
    for (int i = 1; i < 4; ++i) tree_->hideColumn(i);
    connect(model, &QFileSystemModel::directoryLoaded, this, [this](const QString &path) {
        if (QDir::cleanPath(path) == path_) {
            updateRoots(); restoreSelection(); emit selectionChanged();
            QTimer::singleShot(0, this, &FolderPane::expandAncestors);
        }
    });
    if (!path_.isEmpty()) { updateRoots(); expandAncestors(); }
}

void FolderPane::restoreSelection() {
    if (pendingSelection_.isEmpty()) return;
    for (const auto &path : pendingSelection_) {
        const auto index = listProxy_->mapFromSource(model_->index(path));
        if (index.isValid()) details_->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    pendingSelection_.clear();
}

void FolderPane::navigate(const QString &path, bool record) {
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const bool changed = path_ != clean;
    path_ = clean;
    if (record && (historyIndex_ < 0 || history_.value(historyIndex_) != clean)) {
        history_ = history_.mid(0, historyIndex_ + 1);
        history_.append(clean);
        if (history_.size() > 100) history_.removeFirst();
        historyIndex_ = history_.size() - 1;
    }
    address_->setText(path_);
    address_->setCursorPosition(0);
    if (changed) { search_->clear(); pendingSelection_.clear(); details_->selectionModel()->clear(); }
    back_->setEnabled(historyIndex_ > 0);
    forward_->setEnabled(historyIndex_ + 1 < history_.size());
    const QFileInfo info(path_);
    const bool available = info.exists() && info.isDir() && info.isReadable();
    error_->setVisible(!available);
    error_->setText("フォルダが見つからないか、読み取れません。場所を変更するか、F5で再試行してください。");
    views_->setVisible(available);
    updateRoots();
    expandAncestors();
    if (changed) emit stateChanged();
    emit selectionChanged();
}

void FolderPane::updateRoots() {
    listProxy_->configure(path_, hidden(), view_.value("directories_first").toBool(), search_->text());
    treeProxy_->configure(path_, hidden(), true, {});
    if (path_.isEmpty()) return;
    const auto source = model_->index(path_);
    if (source.isValid() && model_->canFetchMore(source)) model_->fetchMore(source);
    const auto index = listProxy_->mapFromSource(source);
    details_->setRootIndex(index);
    icons_->setRootIndex(index);
    if (!statusUpdate_.isActive()) statusUpdate_.start(0);
}

void FolderPane::back() { if (historyIndex_ > 0) navigate(history_[--historyIndex_], false); }
void FolderPane::forward() { if (historyIndex_ + 1 < history_.size()) navigate(history_[++historyIndex_], false); }
void FolderPane::up() { QDir dir(path_); if (dir.cdUp()) navigate(dir.absolutePath()); }

void FolderPane::focusAddress(bool history) {
    address_->setFocus();
    address_->selectAll();
    if (history) {
        QPointer<QCompleter> old = address_->completer();
        auto *completer = new QCompleter(history_, address_);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        address_->setCompleter(completer);
        if (old) old->deleteLater();
        completer->setCompletionPrefix({});
        completer->complete();
    }
}

void FolderPane::focusSearch() { search_->setFocus(); search_->selectAll(); }
void FolderPane::clearSearch() {
    if (address_->hasFocus()) { address_->setText(path_); views_->currentWidget()->setFocus(); }
    else { search_->clear(); views_->currentWidget()->setFocus(); }
}
void FolderPane::focusNextRegion(bool backwards) {
    QList<QWidget *> regions;
    if (!tree_->isHidden()) regions.append(tree_);
    regions << views_->currentWidget() << address_ << search_;
    int index = -1;
    for (int i = 0; i < regions.size(); ++i)
        if (regions[i]->hasFocus() || regions[i]->isAncestorOf(focusWidget())) index = i;
    regions[(index + (backwards ? regions.size() - 1 : 1)) % regions.size()]->setFocus();
}

void FolderPane::selectAll() { if (!treeFocused_) static_cast<QAbstractItemView *>(views_->currentWidget())->selectAll(); }
void FolderPane::setViewMode(const QString &mode) { view_["mode"] = mode; applyView(); emit stateChanged(); }
void FolderPane::setHidden(bool show) { view_["show_hidden"] = show; updateRoots(); emit stateChanged(); }
void FolderPane::setTreeVisible(bool show) { view_["tree_visible"] = show; tree_->setVisible(show); emit stateChanged(); }

void FolderPane::applyView() {
    applying_ = true;
    views_->setCurrentWidget(icons() ? static_cast<QWidget *>(icons_) : details_);
    setFocusProxy(views_->currentWidget());
    const int size = view_.value("icon_size").toInt();
    icons_->setIconSize(QSize(size, size));
    icons_->setGridSize(QSize(qMax(100, size + 36), size + 48));
    tree_->setVisible(treeVisible());
    splitter_->setSizes({view_.value("tree_width").toInt(), 600});
    const auto visible = view_.value("columns").toArray();
    for (int i = 0; i < columns.size(); ++i) {
        details_->setColumnHidden(i, !visible.contains(columns[i]));
        details_->setColumnWidth(i, i == 0 ? 250 : i == 3 ? 160 : 95);
    }
    for (int visual = 0; visual < visible.size(); ++visual) {
        const int logical = columns.indexOf(visible[visual].toString());
        details_->header()->moveSection(details_->header()->visualIndex(logical), visual);
    }
    details_->sortByColumn(columns.indexOf(view_.value("sort_by").toString()), view_.value("sort_order") == "desc" ? Qt::DescendingOrder : Qt::AscendingOrder);
    applying_ = false;
}

QJsonObject FolderPane::state() const {
    QJsonObject view = view_;
    if (treeVisible()) view["tree_width"] = qBound(120, splitter_->sizes().value(0), 600);
    QJsonArray visible;
    for (int i = 0; i < columns.size(); ++i) {
        const int logical = details_->header()->logicalIndex(i);
        if (!details_->isColumnHidden(logical)) visible.append(columns[logical]);
    }
    view["columns"] = visible;
    return {{"pane_id", id_}, {"location", encodeLocation(path_)}, {"view", view}};
}

void FolderPane::expandAncestors() {
    const auto index = treeProxy_->mapFromSource(model_->index(path_));
    for (auto ancestor = index.parent(); ancestor.isValid(); ancestor = ancestor.parent()) tree_->expand(ancestor);
    tree_->setCurrentIndex(index);
    tree_->scrollTo(index);
}

QStringList FolderPane::selectedPaths() const {
    QStringList paths;
    for (const auto &index : details_->selectionModel()->selectedIndexes()) {
        if (index.column() == 0) paths.append(model_->filePath(listProxy_->mapToSource(index)));
    }
    return paths;
}

QStringList FolderPane::operationPaths() const {
    if (!treeFocused_) return selectedPaths();
    const auto index = tree_->currentIndex();
    return index.isValid() && tree_->selectionModel()->isSelected(index) ? QStringList{model_->filePath(treeProxy_->mapToSource(index))} : QStringList{};
}

QString FolderPane::operationDirectory() const {
    if (!treeFocused_) return path_;
    const auto paths = operationPaths();
    return paths.isEmpty() ? QString() : paths.first();
}

bool FolderPane::fileViewFocused() const {
    const QWidget *focus = QApplication::focusWidget();
    if (qobject_cast<const QMenu *>(focus) || qobject_cast<QMenu *>(QApplication::activePopupWidget())) return lastFileContext_;
    return focus && (focus == tree_ || tree_->isAncestorOf(focus) || focus == details_ || details_->isAncestorOf(focus) ||
                     focus == icons_ || icons_->isAncestorOf(focus));
}

void FolderPane::selectPaths(const QStringList &paths) {
    pendingSelection_ = paths;
    details_->selectionModel()->clear();
    restoreSelection();
    if (selectedPaths().size() != paths.size()) pendingSelection_ = paths;
}

void FolderPane::showOperationStatus(const QString &message, int timeout) { status_->showMessage(message, timeout); }

void FolderPane::updateStatus() {
    const int count = views_->isHidden() ? 0 : listProxy_->rowCount(details_->rootIndex());
    const auto selected = details_->selectionModel()->selectedIndexes();
    qsizetype selectionCount = 0;
    qint64 bytes = 0;
    bool hasFile = false;
    for (const auto &index : selected) {
        if (index.column() != 0) continue;
        ++selectionCount;
        const auto source = listProxy_->mapToSource(index);
        if (!model_->isDir(source)) { bytes += qMin(std::numeric_limits<qint64>::max() - bytes, qMax(qint64(0), model_->size(source))); hasFile = true; }
    }
    QString text = QString("%L1 項目  ·  %L2 件選択").arg(count).arg(selectionCount);
    if (hasFile) text += "  ·  " + QLocale().formattedDataSize(bytes);
    if (views_->isHidden()) text = "フォルダを読み取れません";
    summary_->setText(text);
    summary_->setToolTip(text + "\n" + path_);
}

void FolderPane::openCurrent() { activate(static_cast<QAbstractItemView *>(views_->currentWidget())->currentIndex()); }
void FolderPane::activate(const QModelIndex &index) {
    if (!index.isValid()) return;
    const QFileInfo info = model_->fileInfo(listProxy_->mapToSource(index));
    if (info.isDir()) { navigate(info.absoluteFilePath()); return; }
    if (!info.isFile()) return;
    const QString mime = QMimeDatabase().mimeTypeForFile(info, QMimeDatabase::MatchExtension).name();
    if (info.isExecutable() || info.suffix().compare("desktop", Qt::CaseInsensitive) == 0 ||
        mime.contains("executable") || mime.contains("shellscript") || mime.contains("python") ||
        QStringList{"sh", "bash", "zsh", "fish", "py", "pl", "rb", "js", "run", "appimage"}.contains(info.suffix().toLower())) {
        QMessageBox::information(this, "ファイルを開く", "この初期版では、実行ファイル・スクリプト・アプリ起動用ファイルを開きません。");
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath())))
        QMessageBox::warning(this, "ファイルを開く", "既定のアプリを起動できませんでした。");
}

void FolderPane::showProperties() {
    const auto paths = operationPaths();
    const QFileInfo info(paths.isEmpty() ? path_ : paths.first());
    QMessageBox box(QMessageBox::Information, "プロパティ", {}, QMessageBox::Ok, this);
    box.setTextFormat(Qt::PlainText);
    box.setText(QString("名前: %1\n場所: %2\n種類: %3\n更新: %4\nサイズ: %5")
        .arg(info.fileName(), info.absolutePath(), info.isDir() ? "フォルダ" : "ファイル",
             info.lastModified().toString(Qt::ISODate), info.isDir() ? "自動集計しません" : QString::number(info.size()) + " bytes"));
    box.exec();
}

bool FolderPane::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::FocusIn) {
        treeFocused_ = object == tree_ || object == tree_->viewport();
        lastFileContext_ = true;
        emit selectionChanged();
    }
    if (event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Menu || (key->key() == Qt::Key_F10 && key->modifiers() == Qt::ShiftModifier)) {
            auto *view = treeFocused_ ? static_cast<QAbstractItemView *>(tree_) : icons() ? static_cast<QAbstractItemView *>(icons_) : details_;
            const auto rect = view->visualRect(view->currentIndex());
            emit contextMenuRequested(view->viewport()->mapToGlobal(rect.isValid() ? rect.center() : QPoint(20, 20)));
            return true;
        }
    }
    if ((object == icons_->viewport() || object == details_->viewport()) && event->type() == QEvent::Wheel) {
        const auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->modifiers() == Qt::ControlModifier) {
            view_["icon_size"] = qBound(16, view_.value("icon_size").toInt() + (wheel->angleDelta().y() > 0 ? 8 : -8), 128);
            view_["mode"] = "icons";
            applyView();
            emit stateChanged();
            return true;
        }
    }
    return QWidget::eventFilter(object, event);
}
}
