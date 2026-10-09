#include <gio/gio.h>
#include "operation_engine.h"
#include "domain/file_operation.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QUuid>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

// Paths are received only through stdin. No user filenames enter command-line
// arguments or diagnostic output, and this process never loads Qt Widgets.
int main(int argc, char **argv) {
    ::umask(0077);
    std::signal(SIGPIPE, SIG_IGN);
    QCoreApplication app(argc, argv);
    if (argc != 1) return 2;
    mlex::OperationControl control;
    control.gioCancellation = g_cancellable_new();
    QByteArray input;
    QString jobId;
    std::thread job;
    bool started = false;
    auto cancel = [&] { control.cancelled = true; g_cancellable_cancel(control.gioCancellation); };
    auto send = [&](QJsonObject message) {
        message["protocol_version"] = 1;
        message["job_id"] = jobId;
        const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
        if (bytes.size() > mlex::MaxOperationMessage) { cancel(); return; }
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::write(STDOUT_FILENO, bytes.constData() + offset, size_t(bytes.size() - offset));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { cancel(); return; }
            offset += count;
        }
    };
    ::fcntl(STDIN_FILENO, F_SETFL, ::fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    QSocketNotifier reader(STDIN_FILENO, QSocketNotifier::Read);
    auto stopInput = [&] { reader.setEnabled(false); cancel(); if (!started) app.exit(2); };
    QObject::connect(&reader, &QSocketNotifier::activated, &app, [&] {
        char buffer[16384];
        while (true) {
            const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (count <= 0) { stopInput(); return; }
            input.append(buffer, count);
            if (input.size() > mlex::MaxOperationMessage) { stopInput(); return; }
            while (input.contains('\n')) {
                const auto end = input.indexOf('\n');
                const auto document = QJsonDocument::fromJson(input.first(end));
                input.remove(0, end + 1);
                const auto message = document.object();
                if (!document.isObject() || message.value("protocol_version").toInt() != 1) { stopInput(); return; }
                if (started) {
                    if (message.value("type") != "cancel" || message.value("job_id") != jobId) { stopInput(); return; }
                    cancel();
                    continue;
                }
                jobId = message.value("job_id").toString();
                if (message.value("type") != "request" || QUuid(jobId).isNull() || !message.value("request").isObject()) { stopInput(); return; }
                started = true;
                const auto request = message.value("request").toObject();
                job = std::thread([&, request] {
                    QElapsedTimer throttle;
                    throttle.start();
                    control.progress = [&](const QString &phase, qint64 bytes) {
                        if (throttle.elapsed() < 100) return;
                        throttle.restart();
                        send({{"type", "progress"}, {"phase", phase}, {"bytes", QString::number(bytes)}});
                    };
                    QJsonObject result;
                    try { result = mlex::executeOperation(request, control); }
                    catch (...) {
                        // The parent keeps the pending journal and treats this
                        // outcome as uncertain; it must not offer stale Undo.
                        result = {{"uncertain", true}};
                    }
                    send({{"type", "completed"}, {"result", result}});
                    QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
                });
            }
        }
    });
    const int result = app.exec();
    cancel();
    if (job.joinable()) job.join();
    g_object_unref(control.gioCancellation);
    return result;
}
