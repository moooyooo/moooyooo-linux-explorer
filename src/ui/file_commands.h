#pragma once
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include "file_clipboard.h"

class QMenu;
class QLabel;
class QToolButton;
namespace mlex {
class MainWindow;
class FolderPane;
class CommandRegistry;
class OperationService;
class FileCommands : public QObject {
    Q_OBJECT
public:
    FileCommands(MainWindow *window, CommandRegistry *commands);
    OperationService *operations() const { return operations_; }
    void showContextMenu(FolderPane *pane, const QPoint &position);
private:
    bool fileContext() const;
    void rename();
    void createFolder();
    void copy(bool cut);
    void paste();
    void remove(bool permanent);
    void history(bool redo);
    void execute(const QJsonObject &request);
    void completed(const QJsonObject &result);
    void error(const QString &message);
    void fillMenu(QMenu *menu);
    MainWindow *window_;
    CommandRegistry *commands_;
    OperationService *operations_;
    QPointer<FolderPane> origin_;
    QString originPath_;
    FileTransfer pendingCut_;
    QLabel *jobLabel_;
    QToolButton *cancelButton_;
};
}
