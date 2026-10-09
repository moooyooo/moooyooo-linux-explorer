#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QString>
#include <memory>

class QMimeData;
namespace mlex {
struct FileTransfer {
    QJsonArray items;
    bool cut = false;
    QByteArray fingerprint;
    QString error;
};
FileTransfer readFileTransfer(const QMimeData *mime);
std::unique_ptr<QMimeData> makeFileTransfer(const QJsonArray &items, bool cut);
QJsonArray captureItems(const QStringList &paths);
}
