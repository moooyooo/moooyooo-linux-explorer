#pragma once

#include <QJsonObject>
#include <QString>

namespace mlex {
constexpr qsizetype MaxProjectBytes = 2 * 1024 * 1024;
QString newId();
QJsonObject defaultView();
QJsonObject makePane(const QString &path);
QJsonObject makeProject(const QStringList &paths = {});
QJsonObject encodeLocation(const QString &path);
QString resolveLocation(const QJsonObject &location);
bool validateProject(const QJsonObject &project, QString *error);
bool decodeProject(const QByteArray &bytes, QJsonObject *project, QString *error);
QByteArray encodeProject(const QJsonObject &project);
}
