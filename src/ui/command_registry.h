#pragma once
#include <QAction>
#include <QHash>
#include <QObject>
#include <functional>

class QWidget;
namespace mlex {
class CommandRegistry : public QObject {
    Q_OBJECT
public:
    CommandRegistry(QWidget *window, std::function<bool()> hasPane);
    QAction *add(const QString &id, const QString &label, const QList<QKeySequence> &keys,
                 std::function<void()> run, bool needsPane = false, bool outsideText = false);
    QAction *get(const QString &id) const { return commands_.value(id).action; }
    void setCondition(const QString &id, std::function<bool()> condition);
    void update();
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    struct Command { QAction *action = nullptr; bool needsPane = false; bool outsideText = false; std::function<bool()> condition; };
    QHash<QString, Command> commands_;
    QWidget *window_;
    std::function<bool()> hasPane_;
    bool composing_ = false;
};
}
