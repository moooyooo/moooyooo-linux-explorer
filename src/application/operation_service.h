#pragma once
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QProcess>

namespace mlex {
class OperationService : public QObject {
    Q_OBJECT
public:
    explicit OperationService(QObject *parent = nullptr, const QString &workerPath = {}, const QString &journalRoot = {});
    ~OperationService() override;
    bool start(const QJsonObject &request, QString *error = nullptr);
    bool undo(QString *error = nullptr);
    bool redo(QString *error = nullptr);
    void cancel();
    bool busy() const { return busy_; }
    bool cancelling() const { return cancelling_; }
    bool canUndo() const { return !busy_ && !undo_.isEmpty(); }
    bool canRedo() const { return !busy_ && !redo_.isEmpty(); }
signals:
    void stateChanged();
    void progress(const QString &phase, qint64 bytes);
    void finished(const QJsonObject &result);
private:
    enum class Mode { Normal, Undo, Redo };
    bool launch(const QJsonObject &request, Mode mode, QString *error);
    bool record(const QJsonObject &document, QString *error = nullptr);
    void readOutput();
    void complete();
    void sendCancel();
    QString workerPath_, journalRoot_, instance_, jobId_, journalPath_;
    QProcess *process_ = nullptr;
    QByteArray output_;
    QJsonObject request_, result_;
    QList<QJsonObject> undo_, redo_;
    Mode mode_ = Mode::Normal;
    bool busy_ = false, cancelling_ = false, received_ = false;
};
}
