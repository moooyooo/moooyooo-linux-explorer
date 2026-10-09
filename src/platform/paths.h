#pragma once
#include <QString>

namespace mlex {
bool insideRepository(const QString &path);
QString privateDataDirectory(QString *error = nullptr);
QString privateStateDirectory(QString *error = nullptr);
bool preparePrivateDirectory(const QString &path, QString *error = nullptr);
}
