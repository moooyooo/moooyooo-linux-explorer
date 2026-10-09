#pragma once
#include <QFileSystemModel>
#include <QCollator>
#include <QJsonObject>
#include <QSortFilterProxyModel>
#include <QWidget>

class QLineEdit;
class QTreeView;
class QListView;
class QStackedWidget;
class QSplitter;
class QLabel;
class QToolButton;

namespace mlex {
class DirectoryProxy : public QSortFilterProxyModel {
public:
    explicit DirectoryProxy(bool tree, QObject *parent);
    void configure(const QString &root, bool hidden, bool directoriesFirst, const QString &search);
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QVariant data(const QModelIndex &index, int role) const override;
protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;
private:
    bool tree_;
    QString root_;
    bool hidden_ = false;
    bool directoriesFirst_ = true;
    QString search_;
    QCollator collator_;
};

class FolderPane : public QWidget {
    Q_OBJECT
public:
    FolderPane(QFileSystemModel *model, const QJsonObject &state, QWidget *parent = nullptr);
    QString id() const { return id_; }
    QString path() const { return path_; }
    QString title() const;
    QStringList selectedPaths() const;
    QJsonObject state() const;
    void setFileSystemModel(QFileSystemModel *model);
    void navigate(const QString &path, bool history = true);
    void back();
    void forward();
    void up();
    void focusAddress(bool history = false);
    void focusSearch();
    void focusNextRegion(bool backwards);
    void clearSearch();
    void selectAll();
    void setViewMode(const QString &mode);
    void setHidden(bool show);
    void setTreeVisible(bool show);
    void expandAncestors();
    void openCurrent();
    void showProperties();
    bool hidden() const { return view_.value("show_hidden").toBool(); }
    bool treeVisible() const { return view_.value("tree_visible").toBool(); }
    bool icons() const { return view_.value("mode") == "icons"; }
signals:
    void stateChanged();
    void selectionChanged();
    void refreshRequested();
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    void applyView();
    void updateRoots();
    void activate(const QModelIndex &index);
    void restoreSelection();
    QString id_;
    QString path_;
    QStringList history_;
    int historyIndex_ = -1;
    QJsonObject view_;
    QFileSystemModel *model_ = nullptr;
    DirectoryProxy *listProxy_;
    DirectoryProxy *treeProxy_;
    QLineEdit *address_;
    QLineEdit *search_;
    QTreeView *tree_;
    QTreeView *details_;
    QListView *icons_;
    QSplitter *splitter_;
    QStackedWidget *views_;
    QLabel *error_;
    QToolButton *back_;
    QToolButton *forward_;
    QStringList pendingSelection_;
    bool applying_ = false;
};
}
