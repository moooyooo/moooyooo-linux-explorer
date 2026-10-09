#include "domain/project.h"
#include "persistence/project_store.h"
#include "persistence/recovery_store.h"
#include "platform/paths.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace mlex;
class ProjectTest : public QObject {
    Q_OBJECT
private slots:
    void fixtureAndRoundTrip() {
        QFile file(MLEX_SAMPLE_FILE);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject project, roundTrip;
        QString error;
        QVERIFY2(decodeProject(file.readAll(), &project, &error), qPrintable(error));
        QVERIFY(decodeProject(encodeProject(project), &roundTrip, &error));
        QCOMPARE(roundTrip, project);
        QVERIFY(validateProject(makeProject(), &error));
    }
    void rejectsMalformedInput() {
        QString error;
        QJsonObject result;
        const QByteArray valid = encodeProject(makeProject());
        auto duplicate = valid;
        duplicate.replace("\"schema_version\": 1", "\"schema_version\": 1, \"schema_versio\\u006e\": 1");
        QVERIFY(!decodeProject(duplicate, &result, &error));
        for (const QByteArray &bad : {QByteArray("{\"x\":"), QByteArray("[NaN]"), QByteArray("{\"x\":\"\xff\"}"), QByteArray(MaxProjectBytes + 1, ' '), QByteArray(33, '[') + QByteArray(33, ']')})
            QVERIFY(!decodeProject(bad, &result, &error));
        auto project = makeProject();
        project["schema_version"] = true;
        QVERIFY(!validateProject(project, &error));
        project["schema_version"] = 2;
        QVERIFY(!validateProject(project, &error));
        project = makeProject({"/tmp/sample"});
        auto panes = project["panes"].toArray();
        auto pane = panes.first().toObject();
        pane["location"] = QJsonObject{{"kind", "home_relative"}, {"path", "../escape"}};
        panes[0] = pane;
        project["panes"] = panes;
        QVERIFY(!validateProject(project, &error));
        project = makeProject({"/tmp/sample"});
        panes = project["panes"].toArray();
        panes.append(panes.first());
        project["panes"] = panes;
        QVERIFY(!validateProject(project, &error));
        project = makeProject();
        project["active_pane_id"] = newId();
        QVERIFY(!validateProject(project, &error));
        project = makeProject();
        project["unexpected"] = true;
        QVERIFY(!validateProject(project, &error));
    }
    void atomicSaveLockAndConflict() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("business.mlex-project.json");
        auto project = makeProject({temporary.path()});
        QString error;
        const auto originalId = project.value("project_id");
        ProjectStore first;
        QVERIFY2(first.saveAs(path, &project, &error), qPrintable(error));
        QVERIFY(project.value("project_id") != originalId);
        QCOMPARE(project.value("revision").toInt(), 1);
        QCOMPARE(QFileInfo(path).permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther), QFile::Permissions{});
        ProjectStore second;
        QJsonObject copy;
        QVERIFY(second.load(path, &copy, &error));
        QVERIFY(second.readOnly());
        QVERIFY(!second.save(&copy, &error));
        QVERIFY(second.saveAs(temporary.filePath("copy.mlex-project.json"), &copy, &error));
        QVERIFY(copy.value("project_id") != project.value("project_id"));
        QVERIFY(first.save(&project, &error));
        QCOMPARE(project.value("revision").toInt(), 2);
        QFile external(path);
        QVERIFY(external.open(QIODevice::Append));
        external.write(" ");
        external.close();
        QVERIFY(!first.save(&project, &error));
        QCOMPARE(project.value("revision").toInt(), 2);
        QVERIFY(external.open(QIODevice::ReadOnly));
        QVERIFY(external.readAll().endsWith(" "));
    }
    void loadAndReplaceIdentity() {
        QTemporaryDir temporary;
        const QString path = temporary.filePath("project.json");
        auto project = makeProject();
        QString error;
        {
            ProjectStore writer;
            QVERIFY(writer.saveAs(path, &project, &error));
        }
        ProjectStore reader;
        QVERIFY(reader.load(path, &project, &error));
        QVERIFY(!reader.readOnly());
        QFile original(path);
        QVERIFY(original.rename(path + ".old"));
        QVERIFY(original.copy(path));
        QVERIFY(!reader.save(&project, &error));
    }
    void repositoryAndSymlinkProtection() {
        QTemporaryDir temporary;
        QDir root(temporary.path());
        QVERIFY(root.mkdir(".git"));
        QFile head(root.filePath(".git/HEAD"));
        QVERIFY(head.open(QIODevice::WriteOnly));
        head.write("ref: refs/heads/main\n");
        head.close();
        auto project = makeProject();
        QString error;
        ProjectStore store;
        QVERIFY(insideRepository(root.filePath("new/child.json")));
        QVERIFY(!store.saveAs(root.filePath("project.json"), &project, &error));
        ProjectStore example;
        QVERIFY(example.load(MLEX_SAMPLE_FILE, &project, &error));
        QVERIFY(example.readOnly());
        QVERIFY(!QFileInfo::exists(QString(MLEX_SAMPLE_FILE) + ".lock"));
        QTemporaryDir other;
        QFile target(other.filePath("target.json"));
        QVERIFY(target.open(QIODevice::WriteOnly));
        target.write(encodeProject(project));
        target.close();
        QVERIFY(target.link(other.filePath("link.json")));
        QVERIFY(!store.saveAs(other.filePath("link.json"), &project, &error));
    }
    void recoveryOwnershipAndCleanup() {
        QTemporaryDir temporary;
        const auto project = makeProject();
        QString error;
        QString abandoned;
        {
            RecoveryStore first(temporary.path());
            RecoveryStore second(temporary.path());
            QVERIFY(first.write(project, &error));
            const auto candidates = second.candidates();
            QCOMPARE(candidates.size(), 1);
            abandoned = candidates.first();
            QVERIFY(!RecoveryStore::claim(abandoned));
        }
        auto lock = RecoveryStore::claim(abandoned);
        QVERIFY(lock != nullptr);
        QFile file(abandoned);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject decoded;
        QVERIFY(decodeProject(file.readAll(), &decoded, &error));
        QCOMPARE(decoded, project);
        RecoveryStore clean(temporary.path());
        QVERIFY(clean.write(project, &error));
        clean.clear();
        QCOMPARE(QDir(temporary.path()).entryList({"*.json"}, QDir::Files).size(), 1);
    }
};
QTEST_GUILESS_MAIN(ProjectTest)
#include "test_project.moc"
