#include "domain/project.h"
#include "persistence/project_store.h"
#include "ui/main_window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QTextStream>
#include <QTimer>
#include <QTranslator>
#include <sys/stat.h>

namespace {
struct Options { QString project; QStringList directories; bool gui = true; int exitCode = 0; };
Options parse(int argc, char **argv) {
    QCoreApplication core(argc, argv);
    QCoreApplication::setApplicationName("moooyooo-linux-explorer");
    QCoreApplication::setApplicationVersion(MLEX_VERSION);
    QCommandLineParser parser;
    parser.setApplicationDescription("複数フォルダを業務ごとに保存できるLinux向けMDIファイラ（閲覧版）");
    auto help = parser.addHelpOption();
    auto version = parser.addVersionOption();
    parser.addOption({"project", "保存済みのプロジェクトを開く", "file"});
    parser.addPositionalArgument("directory", "開くフォルダ。複数指定可。省略時はホーム。", "[directory ...]");
    if (!parser.parse(core.arguments())) {
        QTextStream(stderr) << "引数が不正です。--help を参照してください。\n";
        return {{}, {}, false, 2};
    }
    if (parser.isSet(help) || parser.isSet("help-all")) { QTextStream(stdout) << parser.helpText(); return {{}, {}, false, 0}; }
    if (parser.isSet(version)) { QTextStream(stdout) << "moooyooo-linux-explorer " << MLEX_VERSION << '\n'; return {{}, {}, false, 0}; }
    Options options;
    options.project = parser.value("project");
    options.directories = parser.positionalArguments();
    if ((parser.isSet("project") && (options.project.isEmpty() || !options.directories.isEmpty())) || options.directories.size() > 64) {
        QTextStream(stderr) << "--project とフォルダは同時指定できません。フォルダは64個までです。\n";
        options.gui = false;
        options.exitCode = 2;
    }
    for (auto &path : options.directories) path = QDir::cleanPath(QDir::current().absoluteFilePath(path));
    if (!options.project.isEmpty()) options.project = QDir::cleanPath(QDir::current().absoluteFilePath(options.project));
    return options;
}
}

int main(int argc, char **argv) {
    ::umask(0077);
    const auto options = parse(argc, argv);
    if (!options.gui) return options.exitCode;
    auto store = std::make_unique<mlex::ProjectStore>();
    QJsonObject project;
    QString error;
    if (!options.project.isEmpty()) {
        if (!store->load(options.project, &project, &error)) { QTextStream(stderr) << error << '\n'; return 1; }
    } else {
        project = mlex::makeProject(options.directories.isEmpty() ? QStringList{QDir::homePath()} : options.directories);
        if (!mlex::validateProject(project, &error)) { QTextStream(stderr) << error << '\n'; return 1; }
    }
    QApplication app(argc, argv);
    app.setApplicationName("moooyooo-linux-explorer");
    app.setApplicationDisplayName("moooyooo Linux Explorer");
    app.setApplicationVersion(MLEX_VERSION);
    QTranslator translator;
    if (translator.load(QLocale(), "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath))) app.installTranslator(&translator);
    mlex::MainWindow window(project, std::move(store));
    window.show();
    if (options.project.isEmpty() && options.directories.isEmpty()) QTimer::singleShot(0, &window, &mlex::MainWindow::offerRecovery);
    return app.exec();
}
