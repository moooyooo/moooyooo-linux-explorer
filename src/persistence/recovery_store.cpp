#include "recovery_store.h"
#include "domain/project.h"
#include "platform/paths.h"
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

namespace mlex {
RecoveryStore::RecoveryStore(const QString &directory) : directory_(directory) {
    if (directory_.isEmpty() || !preparePrivateDirectory(directory_)) return;
    path_ = QDir(directory_).filePath(newId() + ".json");
    lock_ = claim(path_);
}

std::unique_ptr<QLockFile> RecoveryStore::claim(const QString &path) {
    if (insideRepository(path) || QFileInfo(path).isSymLink()) return {};
    auto lock = std::make_unique<QLockFile>(path + ".lock");
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) return {};
    QFile::setPermissions(path + ".lock", QFile::ReadOwner | QFile::WriteOwner);
    return lock;
}

bool RecoveryStore::write(const QJsonObject &project, QString *error) {
    if (!lock_) { if (error) *error = "復元記録の保存先を準備できません。"; return false; }
    if (!validateProject(project, error)) return false;
    const auto bytes = encodeProject(project);
    if (bytes.size() > MaxProjectBytes) { if (error) *error = "復元記録の上限は2MiBです。"; return false; }
    QSaveFile file(path_);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = "復元記録を保存できません。手動でプロジェクトを保存してください。";
        return false;
    }
    return true;
}

void RecoveryStore::clear() { if (lock_) QFile::remove(path_); }

QStringList RecoveryStore::candidates() const {
    QStringList result;
    if (directory_.isEmpty()) return result;
    const QDir dir(directory_);
    for (const auto &name : dir.entryList({"*.json"}, QDir::Files | QDir::NoSymLinks, QDir::Time)) {
        const QString candidate = dir.filePath(name);
        if (candidate != path_) result.append(candidate);
    }
    return result;
}
}
