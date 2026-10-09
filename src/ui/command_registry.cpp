#include "command_registry.h"
#include <QApplication>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QWidget>

namespace mlex {
CommandRegistry::CommandRegistry(QWidget *window, std::function<bool()> hasPane)
    : QObject(window), window_(window), hasPane_(std::move(hasPane)) {
    qApp->installEventFilter(this);
    connect(qApp, &QApplication::focusChanged, this, [this] { composing_ = false; update(); });
}

QAction *CommandRegistry::add(const QString &id, const QString &label, const QList<QKeySequence> &keys,
                            std::function<void()> run, bool needsPane, bool outsideText) {
    Q_ASSERT(!commands_.contains(id));
    auto *action = new QAction(label, this);
    action->setObjectName(id);
    action->setShortcuts(keys);
    action->setShortcutContext(Qt::WindowShortcut);
    window_->addAction(action);
    connect(action, &QAction::triggered, this, [this, run = std::move(run)] { if (!composing_) run(); });
    commands_.insert(id, {action, needsPane, outsideText, {}});
    return action;
}

void CommandRegistry::setCondition(const QString &id, std::function<bool()> condition) {
    Q_ASSERT(commands_.contains(id));
    commands_[id].condition = std::move(condition);
}

void CommandRegistry::update() {
    auto *focus = QApplication::focusWidget();
    const bool text = qobject_cast<QLineEdit *>(focus) || qobject_cast<QTextEdit *>(focus) || qobject_cast<QPlainTextEdit *>(focus);
    for (const auto &command : commands_)
        command.action->setEnabled(!composing_ && (!command.needsPane || hasPane_()) && (!command.outsideText || !text) && (!command.condition || command.condition()));
}

bool CommandRegistry::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::InputMethod && object == QApplication::focusWidget()) {
        composing_ = !static_cast<QInputMethodEvent *>(event)->preeditString().isEmpty();
        update();
    }
    return QObject::eventFilter(object, event);
}
}
