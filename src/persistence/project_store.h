#pragma once
#include <QJsonObject>
#include <QLockFile>
#include <memory>

namespace mlex {
class ProjectStore {
public:
    bool load(const QString &path, QJsonObject *project, QString *error);
    bool save(QJsonObject *project, QString *error);
    bool saveAs(const QString &path, QJsonObject *project, QString *error);
    QString path() const { return path_; }
    bool readOnly() const { return !path_.isEmpty() && !lock_; }
private:
    bool commit(const QString &path, const QJsonObject &project, bool replacing, QString *error);
    QString path_;
    QByteArray digest_;
    int revision_ = 0;
    quint64 device_ = 0;
    quint64 inode_ = 0;
    std::unique_ptr<QLockFile> lock_;
};
}
