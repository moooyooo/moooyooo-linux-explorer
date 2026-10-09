#include "file_operation.h"
#include <QDir>
#include <QFile>
#include <sys/stat.h>

namespace mlex {
bool validFileName(const QString &name) {
    if (name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.contains(QChar::Null)) return false;
    return QString::fromUtf8(name.toUtf8()) == name;
}
bool validOperationPath(const QString &path) {
    return path.startsWith('/') && path != "/" && !path.contains(QChar::Null) &&
        path == QDir::cleanPath(path) && QString::fromUtf8(path.toUtf8()) == path;
}
QJsonObject fileStamp(const QString &path) {
    struct stat info {};
    if (!validOperationPath(path) || ::lstat(QFile::encodeName(path).constData(), &info) != 0) return {};
    return {{"device", QString::number(info.st_dev)}, {"inode", QString::number(info.st_ino)},
            {"mode", int(info.st_mode)}, {"size", QString::number(info.st_size)},
            {"mtime", QString::number(info.st_mtim.tv_sec) + ":" + QString::number(info.st_mtim.tv_nsec)},
            {"ctime", QString::number(info.st_ctim.tv_sec) + ":" + QString::number(info.st_ctim.tv_nsec)}};
}
bool matchesStamp(const QString &path, const QJsonObject &stamp, bool identityOnly) {
    if (stamp.isEmpty()) return false;
    const auto current = fileStamp(path);
    if (identityOnly) return !current.isEmpty() && current["device"] == stamp["device"] &&
        current["inode"] == stamp["inode"] && current["mode"] == stamp["mode"];
    return current == stamp;
}
}
