#pragma once
#include <QJsonObject>
#include <QString>

namespace mlex {
constexpr qsizetype MaxOperationMessage = 1024 * 1024;
constexpr int MaxOperationItems = 256;
bool validFileName(const QString &name);
bool validOperationPath(const QString &path);
// lstat: identity refers to a link itself, never to its target.
QJsonObject fileStamp(const QString &path);
bool matchesStamp(const QString &path, const QJsonObject &stamp, bool identityOnly = false);
}
