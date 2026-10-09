#include "operation_service.h"
#include "domain/file_operation.h"
#include "platform/paths.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>

namespace mlex {
namespace {
using StampChanges = QHash<QByteArray, QJsonObject>;
QJsonValue rebindRecordedStamp(const QJsonValue &value, const StampChanges &changes) {
    if (value.isObject()) {
        auto object = value.toObject();
        if (object.contains("inode") && object.contains("ctime")) {
            for (int step = 0; step < changes.size(); ++step) {
                const auto it = changes.constFind(QJsonDocument(object).toJson(QJsonDocument::Compact));
                if (it == changes.cend() || it.value() == object) break;
                object = it.value();
            }
            return object;
        }
        for (auto it = object.begin(); it != object.end(); ++it) it.value() = rebindRecordedStamp(it.value(), changes);
        return object;
    }
    if (value.isArray()) {
        auto array = value.toArray();
        for (int i = 0; i < array.size(); ++i) array[i] = rebindRecordedStamp(array[i], changes);
        return array;
    }
    return value;
}
}
OperationService::OperationService(QObject *parent, const QString &workerPath, const QString &journalRoot)
    : QObject(parent), workerPath_(workerPath.isEmpty() ? QDir(QCoreApplication::applicationDirPath()).filePath("mlex-file-worker") : workerPath),
      journalRoot_(journalRoot), instance_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

OperationService::~OperationService() {
    if (!process_) return;
    disconnect(process_, nullptr, this, nullptr);
    if (busy_) {
        sendCancel();
        process_->closeWriteChannel();
        // Normal window closing waits for completion. Forced application
        // teardown leaves the pending journal for manual inspection.
        if (!process_->waitForFinished(3000)) { process_->kill(); process_->waitForFinished(1000); }
    }
}

bool OperationService::record(const QJsonObject &document, QString *error) {
    QSaveFile file(journalPath_);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = "操作記録を保存できません。状態領域のアクセス権と空き容量を確認してください。";
        return false;
    }
    return true;
}

bool OperationService::start(const QJsonObject &request, QString *error) { return launch(request, Mode::Normal, error); }
bool OperationService::undo(QString *error) { return canUndo() && launch(undo_.last(), Mode::Undo, error); }
bool OperationService::redo(QString *error) { return canRedo() && launch(redo_.last(), Mode::Redo, error); }

bool OperationService::launch(const QJsonObject &request, Mode mode, QString *error) {
    if (busy_) { if (error) *error = "この業務ウィンドウでは別のファイル操作を実行中です。"; return false; }
    if (QJsonDocument(request).toJson(QJsonDocument::Compact).size() > MaxOperationMessage * 3 / 4) {
        if (error) *error = "操作要求が大きすぎます。項目を分けてください。";
        return false;
    }
    if (journalRoot_.isEmpty()) {
        const auto state = privateStateDirectory(error);
        if (state.isEmpty()) return false;
        journalRoot_ = QDir(state).filePath("jobs");
    }
    const auto directory = QDir(journalRoot_).filePath(instance_);
    if (!preparePrivateDirectory(journalRoot_, error) || !preparePrivateDirectory(directory, error)) return false;
    jobId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    journalPath_ = QDir(directory).filePath(jobId_ + ".json");
    request_ = request;
    if (!record({{"protocol_version", 1}, {"job_id", jobId_}, {"state", "pending"}, {"request", request_}}, error)) return false;
    mode_ = mode;
    result_ = {};
    output_.clear();
    received_ = cancelling_ = false;
    busy_ = true;
    process_ = new QProcess(this);
    process_->setProgram(workerPath_);
    process_->setArguments({});
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process_, &QProcess::started, this, [this] {
        process_->write(QJsonDocument(QJsonObject{{"protocol_version", 1}, {"type", "request"}, {"job_id", jobId_}, {"request", request_}}).toJson(QJsonDocument::Compact) + '\n');
        if (cancelling_) sendCancel();
    });
    connect(process_, &QProcess::readyReadStandardOutput, this, &OperationService::readOutput);
    connect(process_, &QProcess::readyReadStandardError, this, [this] { process_->readAllStandardError(); });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) complete();
    });
    connect(process_, &QProcess::finished, this, [this] { readOutput(); complete(); });
    emit stateChanged();
    process_->start();
    return true;
}

void OperationService::sendCancel() {
    if (process_ && process_->state() == QProcess::Running)
        process_->write(QJsonDocument(QJsonObject{{"protocol_version", 1}, {"type", "cancel"}, {"job_id", jobId_}}).toJson(QJsonDocument::Compact) + '\n');
}
void OperationService::cancel() {
    if (!busy_ || cancelling_) return;
    cancelling_ = true;
    sendCancel();
    emit progress("停止要求中（完了した項目は残ります）", 0);
    emit stateChanged();
}

void OperationService::readOutput() {
    if (!process_ || !busy_) return;
    output_ += process_->readAllStandardOutput();
    while (output_.contains('\n')) {
        const auto end = output_.indexOf('\n');
        if (end >= MaxOperationMessage) { output_.clear(); cancel(); return; }
        const auto doc = QJsonDocument::fromJson(output_.first(end));
        output_.remove(0, end + 1);
        const auto message = doc.object();
        if (!doc.isObject() || message.value("protocol_version").toInt() != 1 || message.value("job_id") != jobId_) { cancel(); continue; }
        if (message.value("type") == "completed" && message.value("result").isObject()) {
            result_ = message.value("result").toObject();
            received_ = true;
        } else if (message.value("type") == "progress" && !cancelling_) {
            emit progress(message.value("phase").toString(), message.value("bytes").toString().toLongLong());
        }
    }
    if (output_.size() > MaxOperationMessage) { output_.clear(); cancel(); }
}

void OperationService::complete() {
    if (!busy_) return;
    if (!received_ || result_.value("uncertain").toBool()) {
        result_ = {{"uncertain", true}, {"changed", true}, {"failures", QJsonArray{QJsonObject{{"error", "操作ワーカーが正常に完了しませんでした。実際のファイルと操作記録を確認してください。"}}}}};
    }
    const auto inverse = result_.value("undo").toObject();
    const bool success = !inverse.isEmpty() && result_.value("failures").toArray().isEmpty() && !result_.value("cancelled").toBool();
    if (success) {
        // Only rebind stamps that exactly match a state the worker validated.
        // External changes remain mismatches; our own renames and redo copies
        // must not invalidate earlier undo entries merely by changing ctime.
        StampChanges changes;
        for (const auto &value : result_.value("metadata_changes").toArray()) {
            const auto change = value.toObject();
            const auto before = change.value("before").toObject(), after = change.value("after").toObject();
            if (before.isEmpty() || after.isEmpty()) continue;
            changes.insert(QJsonDocument(before).toJson(QJsonDocument::Compact), after);
        }
        for (auto &entry : undo_) entry = rebindRecordedStamp(entry, changes).toObject();
        for (auto &entry : redo_) entry = rebindRecordedStamp(entry, changes).toObject();
        if (mode_ == Mode::Undo) { undo_.removeLast(); redo_.append(inverse); }
        else if (mode_ == Mode::Redo) { redo_.removeLast(); undo_.append(inverse); }
        else { undo_.append(inverse); redo_.clear(); }
        while (undo_.size() > 20) undo_.removeFirst();
        while (redo_.size() > 20) redo_.removeFirst();
    } else if (result_.value("changed").toBool()) { undo_.clear(); redo_.clear(); }
    QString journalError;
    if (!record({{"protocol_version", 1}, {"job_id", jobId_}, {"state", result_.value("uncertain").toBool() ? "uncertain" : "completed"}, {"request", request_}, {"result", result_}}, &journalError)) {
        auto warnings = result_.value("warnings").toArray();
        warnings.append(journalError);
        result_["warnings"] = warnings;
    }
    // Bound our own completed logs. Never remove other parents' pending jobs.
    QDir directory(QFileInfo(journalPath_).absolutePath());
    const auto logs = directory.entryInfoList({"*.json"}, QDir::Files, QDir::Time);
    for (int i = 20; i < logs.size(); ++i) {
        QFile file(logs[i].absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        const auto state = QJsonDocument::fromJson(file.read(MaxOperationMessage * 2)).object().value("state");
        file.close();
        if (state == "completed") file.remove();
    }
    auto *finishedProcess = process_;
    process_ = nullptr;
    disconnect(finishedProcess, nullptr, this, nullptr);
    finishedProcess->deleteLater();
    busy_ = false;
    emit stateChanged();
    emit finished(result_);
}
}
