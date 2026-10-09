#include "paths.h"
#include <QDir>
#include <QFileInfo>

namespace mlex {
bool insideRepository(const QString &path) {
    QFileInfo info(path);
    QString existing = info.absoluteFilePath();
    while (!QFileInfo::exists(existing)) {
        const QString parent = QFileInfo(existing).absolutePath();
        if (parent == existing) break;
        existing = parent;
    }
    info.setFile(existing);
    QDir dir(info.isDir() ? info.canonicalFilePath() : info.canonicalPath());
    do {
        const QFileInfo gitEntry(dir.filePath(".git"));
        if (gitEntry.isFile() || dir.exists(".git/HEAD") ||
            (dir.exists("schemas/project.schema.json") && dir.exists("AGENTS.md"))) return true;
    } while (dir.cdUp());
    return false;
}

bool preparePrivateDirectory(const QString &path, QString *error) {
    if (insideRepository(path) || QFileInfo(path).isSymLink()) {
        if (error) *error = "リポジトリ内やリンク先には実行時データを保存できません。";
        return false;
    }
    if (!QDir().mkpath(path) || !QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        if (error) *error = "専用の保存フォルダを準備できません。";
        return false;
    }
    return true;
}

static QString privateDirectory(const char *variable, const QString &fallback, QString *error) {
    QString base = qEnvironmentVariable(variable);
    if (!QDir::isAbsolutePath(base)) base = QDir::home().filePath(fallback);
    const QString path = QDir(base).filePath("moooyooo-linux-explorer");
    return preparePrivateDirectory(path, error) ? path : QString();
}

QString privateDataDirectory(QString *error) { return privateDirectory("XDG_DATA_HOME", ".local/share", error); }
QString privateStateDirectory(QString *error) { return privateDirectory("XDG_STATE_HOME", ".local/state", error); }
}
