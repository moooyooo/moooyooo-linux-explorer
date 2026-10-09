#include "application/operation_service.h"
#include "domain/file_operation.h"
#include "workers/operation_engine.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>
#include <unistd.h>

using namespace mlex;
namespace {
bool writeFile(const QString &path, const QByteArray &content = "Synthetic test data\n") {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}
QJsonObject request(const QString &kind, const QStringList &paths = {}, const QString &destination = {}, const QString &name = {}) {
    QJsonArray items;
    for (const auto &path : paths) items.append(QJsonObject{{"path", path}, {"stamp", fileStamp(path)}});
    return {{"kind", kind}, {"items", items}, {"destination", destination}, {"name", name}, {"policy", "skip"}};
}
QJsonObject perform(const QJsonObject &request) { OperationControl control; return executeOperation(request, control); }
bool successful(const QJsonObject &result) { return result.value("failures").toArray().isEmpty() && !result.value("cancelled").toBool(); }
QString diagnostics(const QJsonObject &result) { return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)); }
}

class OperationsTest : public QObject {
    Q_OBJECT
    QTemporaryDir privateData_;
private slots:
    void initTestCase() {
        QVERIFY(privateData_.isValid());
        qputenv("XDG_DATA_HOME", privateData_.path().toUtf8());
        qputenv("HOME", privateData_.path().toUtf8());
    }
    void namesIdentityAndCollisions() {
        QTemporaryDir tmp;
        const auto a = tmp.filePath("a.txt"), b = tmp.filePath("b.txt");
        QVERIFY(writeFile(a));
        QVERIFY(writeFile(b, "Retain this file"));
        auto rename = request("rename", {a}, tmp.path(), "b.txt");
        QVERIFY(!successful(perform(rename)));
        QVERIFY(QFileInfo::exists(a));
        QFile unchanged(b); QVERIFY(unchanged.open(QIODevice::ReadOnly)); QCOMPARE(unchanged.readAll(), QByteArray("Retain this file"));
        for (const auto &name : {"", ".", "..", "../escape"}) {
            rename["name"] = name;
            QVERIFY(!successful(perform(rename)));
        }
        rename["name"] = "renamed.txt";
        QVERIFY(writeFile(a, "Changed after selection"));
        QVERIFY(!successful(perform(rename)));
        QVERIFY(!QFileInfo::exists(tmp.filePath("renamed.txt")));
        QVERIFY(!validOperationPath("/"));
        QVERIFY(!validOperationPath(tmp.path() + "/../elsewhere"));
    }
    void renameFolderAndUndoRedo() {
        QTemporaryDir tmp;
        QVERIFY(QDir(tmp.path()).mkpath("folder/child"));
        QVERIFY(writeFile(tmp.filePath("folder/child/document.txt")));
        auto result = perform(request("rename", {tmp.filePath("folder")}, tmp.path(), "new name"));
        QVERIFY2(successful(result), qPrintable(diagnostics(result)));
        QVERIFY(QFileInfo::exists(tmp.filePath("new name/child/document.txt")));
        for (int i = 0; i < 12; ++i) {
            QVERIFY(!result.value("undo").toObject().isEmpty());
            result = perform(result.value("undo").toObject());
            QVERIFY2(successful(result), qPrintable(diagnostics(result)));
        }
        QVERIFY(writeFile(tmp.filePath("new name/child/document.txt"), "An external edit"));
        result = perform(result.value("undo").toObject());
        QVERIFY(!successful(result));
        QVERIFY(QFileInfo::exists(tmp.filePath("new name/child/document.txt")));
    }
    void mkdirAndCopyUndoGuards() {
        QTemporaryDir tmp;
        auto result = perform(request("mkdir", {}, tmp.path(), "created"));
        QVERIFY(successful(result));
        QVERIFY(writeFile(tmp.filePath("created/new.txt")));
        QVERIFY(!successful(perform(result.value("undo").toObject())));
        QVERIFY(QFileInfo::exists(tmp.filePath("created/new.txt")));
        QVERIFY(writeFile(tmp.filePath("source.txt")));
        QVERIFY(QDir(tmp.path()).mkdir("destination"));
        result = perform(request("copy", {tmp.filePath("source.txt")}, tmp.filePath("destination")));
        QVERIFY2(successful(result), qPrintable(diagnostics(result)));
        QVERIFY(writeFile(tmp.filePath("destination/source.txt"), "Keep the user's edit"));
        QVERIFY(!successful(perform(result.value("undo").toObject())));
        QVERIFY(QFileInfo::exists(tmp.filePath("destination/source.txt")));
    }
    void recursiveCopyLinksAndRepeatedRedo() {
        QTemporaryDir tmp;
        QVERIFY(QDir(tmp.path()).mkpath("source/sub"));
        QVERIFY(QDir(tmp.path()).mkdir("destination"));
        QVERIFY(writeFile(tmp.filePath("source/sub/space ' quote.txt")));
        QVERIFY(::symlink("..", QFile::encodeName(tmp.filePath("source/sub/loop")).constData()) == 0);
        QVERIFY(::symlink("missing", QFile::encodeName(tmp.filePath("source/broken")).constData()) == 0);
        auto result = perform(request("copy", {tmp.filePath("source")}, tmp.filePath("destination")));
        QVERIFY2(successful(result), qPrintable(diagnostics(result)));
        QVERIFY(QFileInfo(tmp.filePath("destination/source/sub/loop")).isSymLink());
        QVERIFY(QFileInfo(tmp.filePath("destination/source/broken")).isSymLink());
        for (int i = 0; i < 10; ++i) {
            result = perform(result.value("undo").toObject());
            QVERIFY2(successful(result), qPrintable(diagnostics(result)));
            QCOMPARE(QFileInfo::exists(tmp.filePath("destination/source")), i % 2 == 1);
        }
        result = perform(request("copy", {tmp.filePath("source")}, tmp.path()));
        QVERIFY(successful(result));
        const auto target = result.value("successes").toArray().first().toObject().value("target").toString();
        QVERIFY(target != tmp.filePath("source"));
        result = perform(result.value("undo").toObject());
        QVERIFY(successful(result));
        result = perform(result.value("undo").toObject());
        QVERIFY2(successful(result), qPrintable(diagnostics(result)));
        QVERIFY(QFileInfo::exists(target));
    }
    void moveDeleteAndOverlaps() {
        QTemporaryDir tmp;
        QVERIFY(QDir(tmp.path()).mkpath("source/sub"));
        QVERIFY(QDir(tmp.path()).mkdir("destination"));
        QVERIFY(writeFile(tmp.filePath("source/sub/file.txt")));
        QVERIFY(!successful(perform(request("copy", {tmp.filePath("source")}, tmp.filePath("source/sub")))));
        QVERIFY(!successful(perform(request("move", {tmp.filePath("source"), tmp.filePath("source/sub")}, tmp.filePath("destination")))));
        auto result = perform(request("move", {tmp.filePath("source")}, tmp.filePath("destination")));
        QVERIFY(successful(result));
        QVERIFY(!QFileInfo::exists(tmp.filePath("source")));
        result = perform(result.value("undo").toObject());
        QVERIFY(successful(result));
        QVERIFY(QFileInfo::exists(tmp.filePath("source/sub/file.txt")));
        QVERIFY(::symlink("source", QFile::encodeName(tmp.filePath("link")).constData()) == 0);
        result = perform(request("remove", {tmp.filePath("link")}));
        QVERIFY(successful(result));
        QVERIFY(QFileInfo::exists(tmp.filePath("source/sub/file.txt")));
        QVERIFY(result.value("undo").toObject().isEmpty());
    }
    void cancellationAndSpecialFiles() {
        QTemporaryDir tmp;
        QVERIFY(QDir(tmp.path()).mkpath("source/readonly"));
        QVERIFY(QDir(tmp.path()).mkdir("destination"));
        QVERIFY(writeFile(tmp.filePath("source/readonly/a.txt")));
        QVERIFY(::chmod(QFile::encodeName(tmp.filePath("source/readonly")).constData(), 0555) == 0);
        QFile large(tmp.filePath("source/large.bin"));
        QVERIFY(large.open(QIODevice::WriteOnly));
        QVERIFY(large.resize(2 * 1024 * 1024));
        large.close();
        OperationControl control;
        control.progress = [&control](const QString &, qint64 bytes) { if (bytes >= 65536) control.cancelled = true; };
        const auto result = executeOperation(request("copy", {tmp.filePath("source")}, tmp.filePath("destination")), control);
        QVERIFY(result.value("cancelled").toBool());
        QVERIFY(QFileInfo::exists(tmp.filePath("source/large.bin")));
        QVERIFY(QDir(tmp.filePath("destination")).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(::chmod(QFile::encodeName(tmp.filePath("source/readonly")).constData(), 0700) == 0);
        QVERIFY(::mkfifo(QFile::encodeName(tmp.filePath("pipe")).constData(), 0600) == 0);
        QVERIFY(!successful(perform(request("copy", {tmp.filePath("pipe")}, tmp.filePath("destination")))));
    }
    void multiItemUndoChecksBeforeMutation() {
        QTemporaryDir tmp;
        QVERIFY(writeFile(tmp.filePath("a")));
        QVERIFY(writeFile(tmp.filePath("b")));
        QVERIFY(QDir(tmp.path()).mkdir("destination"));
        auto result = perform(request("copy", {tmp.filePath("a"), tmp.filePath("b")}, tmp.filePath("destination")));
        QVERIFY(successful(result));
        QVERIFY(writeFile(tmp.filePath("destination/a"), "External edit"));
        result = perform(result.value("undo").toObject());
        QVERIFY(!successful(result));
        QVERIFY(!result.value("changed").toBool());
        QVERIFY(QFileInfo::exists(tmp.filePath("destination/b")));
    }
    void partialCompletionRetainsCompletedItems() {
        QTemporaryDir tmp;
        QVERIFY(writeFile(tmp.filePath("a.txt")));
        QVERIFY(writeFile(tmp.filePath("b.txt")));
        QVERIFY(QDir(tmp.path()).mkdir("destination"));
        int startedItems = 0;
        OperationControl control;
        control.progress = [&](const QString &, qint64 bytes) { if (bytes == 0 && ++startedItems == 2) control.cancelled = true; };
        auto result = executeOperation(request("copy", {tmp.filePath("a.txt"), tmp.filePath("b.txt")}, tmp.filePath("destination")), control);
        QVERIFY(result.value("changed").toBool());
        QVERIFY(result.value("cancelled").toBool());
        QCOMPARE(result.value("successes").toArray().size(), 1);
        QVERIFY(result.value("undo").toObject().isEmpty());
        QVERIFY(QFileInfo::exists(tmp.filePath("destination/a.txt")));
        QVERIFY(!QFileInfo::exists(tmp.filePath("destination/b.txt")));
        QVERIFY(QFileInfo::exists(tmp.filePath("b.txt")));
        result = perform(request("copy", {tmp.filePath("a.txt"), tmp.filePath("b.txt")}, tmp.filePath("destination")));
        QCOMPARE(result.value("failures").toArray().size(), 1);
        QCOMPARE(result.value("successes").toArray().size(), 1);
        QVERIFY(result.value("undo").toObject().isEmpty());
        QVERIFY(QFileInfo::exists(tmp.filePath("destination/b.txt")));
    }
    void realWorkerJournalAndUndo() {
        QTemporaryDir files, logs;
        OperationService service(nullptr, {}, logs.path());
        QSignalSpy finished(&service, &OperationService::finished);
        QString error;
        QVERIFY2(service.start(request("mkdir", {}, files.path(), "created"), &error), qPrintable(error));
        QVERIFY(!service.start(request("mkdir", {}, files.path(), "overlap"), &error));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY2(successful(finished.last().first().toJsonObject()), qPrintable(diagnostics(finished.last().first().toJsonObject())));
        QVERIFY(service.canUndo());
        QVERIFY(service.undo(&error));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
        QVERIFY(!QFileInfo::exists(files.filePath("created")));
        QVERIFY(service.canRedo());
        QVERIFY(service.redo(&error));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 10000);
        QVERIFY(QFileInfo::exists(files.filePath("created")));
        const auto directories = QDir(logs.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(directories.size(), 1);
        const QDir directory(QDir(logs.path()).filePath(directories.first()));
        QCOMPARE(directory.entryList({"*.json"}).size(), 3);
        for (const auto &path : directory.entryList({"*.json"})) {
            QFile record(directory.filePath(path));
            QCOMPARE(record.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther), QFile::Permissions());
            QVERIFY(record.open(QIODevice::ReadOnly));
            QCOMPARE(QJsonDocument::fromJson(record.readAll()).object().value("state"), QJsonValue("completed"));
        }
        QVERIFY(writeFile(files.filePath("discard.txt")));
        QVERIFY(service.start(request("remove", {files.filePath("discard.txt")}), &error));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 4, 10000);
        QVERIFY(!service.canUndo());
        QVERIFY(!service.canRedo());
    }
    void realWorkerTrashAndCancel() {
        QTemporaryDir files, logs;
        QVERIFY(writeFile(files.filePath("trash-example.txt")));
        OperationService service(nullptr, {}, logs.path());
        QSignalSpy finished(&service, &OperationService::finished);
        QVERIFY(service.start(request("trash", {files.filePath("trash-example.txt")})));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        const auto result = finished.last().first().toJsonObject();
        QVERIFY2(successful(result), qPrintable(diagnostics(result)));
        QVERIFY(!QFileInfo::exists(files.filePath("trash-example.txt")));
        QVERIFY(QFileInfo::exists(privateData_.filePath("Trash/files/trash-example.txt")));
        QVERIFY(!service.canUndo());
        QVERIFY(QDir(files.path()).mkdir("destination"));
        QFile large(files.filePath("large.bin"));
        QVERIFY(large.open(QIODevice::WriteOnly));
        QVERIFY(large.resize(64 * 1024 * 1024));
        large.close();
        QVERIFY(service.start(request("copy", {large.fileName()}, files.filePath("destination"))));
        service.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
        QVERIFY(finished.last().first().toJsonObject().value("cancelled").toBool());
        QVERIFY(!QFileInfo::exists(files.filePath("destination/large.bin")));
        QVERIFY(QFileInfo::exists(large.fileName()));
    }
    void dependentUndoRedoHistory() {
        QTemporaryDir files, logs;
        QVERIFY(QDir(files.path()).mkpath("source/sub"));
        QVERIFY(QDir(files.path()).mkdir("destination"));
        QVERIFY(writeFile(files.filePath("source/sub/a.txt")));
        OperationService service(nullptr, {}, logs.path());
        QSignalSpy finished(&service, &OperationService::finished);
        QVERIFY(service.start(request("copy", {files.filePath("source")}, files.filePath("destination"))));
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(service.start(request("rename", {files.filePath("destination/source/sub/a.txt")}, files.filePath("destination/source/sub"), "b.txt")));
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(service.start(request("mkdir", {}, files.filePath("destination/source"), "new folder")));
        QTRY_COMPARE(finished.size(), 3);
        for (int i = 4; i <= 6; ++i) {
            QVERIFY(service.canUndo());
            QVERIFY(service.undo());
            QTRY_COMPARE(finished.size(), i);
            const auto result = finished.last().first().toJsonObject();
            QVERIFY2(successful(result), qPrintable(diagnostics(result)));
        }
        QVERIFY(!QFileInfo::exists(files.filePath("destination/source")));
        for (int i = 7; i <= 9; ++i) {
            QVERIFY(service.canRedo());
            QVERIFY(service.redo());
            QTRY_COMPARE(finished.size(), i);
            const auto result = finished.last().first().toJsonObject();
            QVERIFY2(successful(result), qPrintable(diagnostics(result)));
        }
        QVERIFY(QFileInfo::exists(files.filePath("destination/source/sub/b.txt")));
        QVERIFY(QFileInfo(files.filePath("destination/source/new folder")).isDir());
        // An external edit must not become trusted through a later rename.
        QVERIFY(writeFile(files.filePath("destination/source/sub/b.txt"), "External edit after copy"));
        QVERIFY(service.start(request("rename", {files.filePath("destination/source/sub/b.txt")}, files.filePath("destination/source/sub"), "c.txt")));
        QTRY_COMPARE(finished.size(), 10);
        QVERIFY(service.undo());
        QTRY_COMPARE(finished.size(), 11);
        QVERIFY(service.undo());
        QTRY_COMPARE(finished.size(), 12);
        QVERIFY(service.undo());
        QTRY_COMPARE(finished.size(), 13);
        QVERIFY(!successful(finished.last().first().toJsonObject()));
        QVERIFY(QFileInfo::exists(files.filePath("destination/source/sub/b.txt")));
    }
    void trashFailureNeverDeletesPermanently() {
        QTemporaryDir files, logs;
        QVERIFY(writeFile(files.filePath("blocked-data-directory")));
        QVERIFY(writeFile(files.filePath("keep.txt")));
        const auto previous = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", files.filePath("blocked-data-directory").toUtf8());
        OperationService service(nullptr, {}, logs.path());
        QSignalSpy finished(&service, &OperationService::finished);
        const bool started = service.start(request("trash", {files.filePath("keep.txt")}));
        // QProcess snapshots the environment during start(). Restore before any assertion.
        qputenv("XDG_DATA_HOME", previous);
        QVERIFY(started);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!successful(finished.last().first().toJsonObject()));
        QVERIFY(!finished.last().first().toJsonObject().value("changed").toBool());
        QVERIFY(QFileInfo::exists(files.filePath("keep.txt")));
    }
    void workerFailureAndInvalidProtocol() {
        QTemporaryDir files, logs;
        OperationService service(nullptr, files.filePath("absent-worker"), logs.path());
        QSignalSpy finished(&service, &OperationService::finished);
        QVERIFY(service.start(request("mkdir", {}, files.path(), "must-not-exist")));
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.last().first().toJsonObject().value("uncertain").toBool());
        QVERIFY(!QFileInfo::exists(files.filePath("must-not-exist")));
        QProcess worker;
        worker.start(QDir(QCoreApplication::applicationDirPath()).filePath("mlex-file-worker"), {});
        QVERIFY(worker.waitForStarted());
        worker.write("{\"protocol_version\":999}\n");
        QVERIFY(worker.waitForFinished(10000));
        QCOMPARE(worker.exitCode(), 2);
    }
};
QTEST_GUILESS_MAIN(OperationsTest)
#include "test_operations.moc"
