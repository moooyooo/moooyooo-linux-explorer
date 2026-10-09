#pragma once
#include <QJsonObject>
#include <QLockFile>
#include <memory>

namespace mlex {
class RecoveryStore {
public:
    explicit RecoveryStore(const QString &directory);
    bool write(const QJsonObject &project, QString *error);
    void clear();
    QStringList candidates() const;
    // Hold a candidate's lock while the caller offers restore/discard.
    static std::unique_ptr<QLockFile> claim(const QString &path);
private:
    QString directory_;
    QString path_;
    std::unique_ptr<QLockFile> lock_;
};
}
