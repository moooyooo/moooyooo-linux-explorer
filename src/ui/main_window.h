#pragma once
#include <QMainWindow>
#include <QJsonObject>
#include <QTimer>
#include <memory>

class QMdiArea;
class QMdiSubWindow;
class QFileSystemModel;
class QMenu;
class QCloseEvent;
class QResizeEvent;

namespace mlex {
class FolderPane;
class CommandRegistry;
class ProjectStore;
class RecoveryStore;
class FileCommands;
class OperationService;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(const QJsonObject &project, std::unique_ptr<ProjectStore> store = {}, bool recoveryEnabled = true);
    ~MainWindow() override;
    QJsonObject snapshot() const;
    FolderPane *activePane() const;
    QMdiArea *mdiArea() const { return mdi_; }
    bool dirty() const { return dirty_; }
    void addPane();
    void setTabbed(bool tabbed);
    void arrange(const QString &mode);
    bool saveTo(const QString &path, QString *error);
    void offerRecovery();
    void applyOperationResult(const QJsonObject &result);
    OperationService *operations() const;
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
private:
    QMdiSubWindow *appendPane(const QJsonObject &state);
    void restore(const QJsonObject &project);
    void setupCommands();
    void refreshFiles();
    void markDirty();
    void updateUi();
    void writeRecovery();
    void rebuildWindowMenu();
    QList<QMdiSubWindow *> orderedWindows() const;
    void launchParent(const QStringList &arguments = {});
    bool save(bool as);
    static QFileSystemModel *createModel(QObject *parent);
    QJsonObject project_;
    QMdiArea *mdi_;
    QFileSystemModel *model_;
    CommandRegistry *commands_;
    FileCommands *fileCommands_ = nullptr;
    QMenu *windowMenu_;
    std::unique_ptr<ProjectStore> store_;
    std::unique_ptr<RecoveryStore> recovery_;
    QTimer recoveryDebounce_;
    QTimer recoveryDeadline_;
    bool dirty_ = false;
    bool restoring_ = false;
    bool closing_ = false;
    bool initialized_ = false;
    bool initialShowScheduled_ = false;
    bool closeWhenFinished_ = false;
    QString arrangement_ = "free";
    QStringList paneOrder_;
    QString recoveryError_;
};
}
