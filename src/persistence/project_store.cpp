#include "project_store.h"
#include "domain/project.h"
#include "platform/paths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <sys/stat.h>

namespace mlex {
namespace {
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
QByteArray digest(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
bool read(const QString &path, QByteArray *bytes, QString *error) {
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) return fail(error, "プロジェクトを読み取れません。");
    *bytes = file.read(MaxProjectBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes->size() > MaxProjectBytes) return fail(error, "プロジェクトの読み取りに失敗したか、2MiBを超えています。");
    return true;
}
bool identity(const QString &path, quint64 *device, quint64 *inode) {
    struct stat info {};
    if (::lstat(QFile::encodeName(path).constData(), &info) != 0 || !S_ISREG(info.st_mode)) return false;
    *device = info.st_dev;
    *inode = info.st_ino;
    return true;
}
std::unique_ptr<QLockFile> lock(const QString &path) {
    // Never remove a live owner's lock merely because it is old.
    auto result = std::make_unique<QLockFile>(path + ".lock");
    result->setStaleLockTime(0);
    if (!result->tryLock(0)) return {};
    QFile::setPermissions(path + ".lock", QFile::ReadOwner | QFile::WriteOwner);
    return result;
}
}

bool ProjectStore::load(const QString &path, QJsonObject *project, QString *error) {
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    // Committed examples remain readable, without creating machine-specific locks.
    auto nextLock = insideRepository(absolute) || QFileInfo(absolute).isSymLink() ? nullptr : lock(absolute);
    QByteArray bytes;
    QJsonObject next;
    if (!read(absolute, &bytes, error) || !decodeProject(bytes, &next, error)) return false;
    quint64 device = 0, inode = 0;
    if (!identity(absolute, &device, &inode)) nextLock.reset();
    path_ = absolute;
    digest_ = digest(bytes);
    revision_ = next.value("revision").toInt();
    device_ = device;
    inode_ = inode;
    lock_ = std::move(nextLock);
    *project = next;
    return true;
}

bool ProjectStore::commit(const QString &path, const QJsonObject &project, bool replacing, QString *error) {
    if (insideRepository(path) || QFileInfo(path).isSymLink()) return fail(error, "リポジトリ内やシンボリックリンクには保存できません。");
    if (!validateProject(project, error)) return false;
    const auto bytes = encodeProject(project);
    if (bytes.size() > MaxProjectBytes) return fail(error, "プロジェクトの上限は2MiBです。");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) || file.write(bytes) != bytes.size())
        return fail(error, "一時ファイルに保存できません。元のプロジェクトは更新していません。");
    // Check again after the temporary file is fully written, immediately before rename.
    if (replacing) {
        QByteArray current;
        quint64 device = 0, inode = 0;
        if (!identity(path, &device, &inode) || device != device_ || inode != inode_ ||
            !read(path, &current, error) || digest(current) != digest_)
            return fail(error, "元のプロジェクトが外部で変更・削除されました。別名で保存してください。");
    } else if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
        return fail(error, "同名のファイルがあります。別の保存名を選んでください。");
    }
    if (!file.commit()) return fail(error, "保存を確定できません。変更内容は未保存のままです。");
    digest_ = digest(bytes);
    revision_ = project.value("revision").toInt();
    identity(path, &device_, &inode_);
    return true;
}

bool ProjectStore::save(QJsonObject *project, QString *error) {
    if (path_.isEmpty() || !lock_) return fail(error, "このプロジェクトは上書きできません。別名で保存してください。");
    if (revision_ == INT_MAX) return fail(error, "更新回数が上限に達しました。別名で保存してください。");
    QJsonObject next = *project;
    next["revision"] = revision_ + 1;
    if (!commit(path_, next, true, error)) return false;
    *project = next;
    return true;
}

bool ProjectStore::saveAs(const QString &path, QJsonObject *project, QString *error) {
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (insideRepository(absolute) || QFileInfo(absolute).isSymLink()) return fail(error, "ソースリポジトリの外に保存してください。リンクへの保存もできません。");
    if (QFileInfo::exists(absolute)) return fail(error, "同名のファイルがあります。新しい名前を指定してください。");
    auto nextLock = lock(absolute);
    if (!nextLock) return fail(error, "保存先をロックできません。保存先の権限と他のウィンドウを確認してください。");
    QJsonObject next = *project;
    next["project_id"] = newId();
    next["revision"] = 1;
    if (!commit(absolute, next, false, error)) return false;
    path_ = absolute;
    lock_ = std::move(nextLock);
    *project = next;
    return true;
}
}
