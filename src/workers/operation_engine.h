#pragma once
#include <QJsonObject>
#include <atomic>
#include <functional>

typedef struct _GCancellable GCancellable;
namespace mlex {
struct OperationControl {
    std::atomic_bool cancelled{false};
    GCancellable *gioCancellation = nullptr;
    std::function<void(const QString &, qint64)> progress;
};
QJsonObject executeOperation(const QJsonObject &request, OperationControl &control);
}
