#include "domain/project.h"
#include "persistence/project_store.h"
#include "ui/folder_pane.h"
#include "ui/main_window.h"
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QLineEdit>
#include <QListView>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QTemporaryDir>
#include <QTabBar>
#include <QInputMethodEvent>
#include <QHeaderView>
#include <QTreeView>
#include <QtTest>

using namespace mlex;
class WorkspaceTest : public QObject {
    Q_OBJECT
    QTemporaryDir fixture_;
    void trigger(MainWindow &window, const char *name) {
        auto *action = window.findChild<QAction *>(name);
        QVERIFY(action);
        QVERIFY(action->isEnabled());
        action->trigger();
        QApplication::processEvents();
    }
private slots:
    void initTestCase() {
        QVERIFY(fixture_.isValid());
        QDir root(fixture_.path());
        QVERIFY(root.mkpath("業務サンプル/資料"));
        QVERIFY(root.mkpath("業務サンプル/成果物"));
        for (const auto &name : {"見積書.txt", "議事録.txt", "item2.txt", "item10.txt", ".hidden.txt", "space ' quote.txt"}) {
            QFile file(root.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("Synthetic test data.\n");
        }
    }
    void childrenAndIndependentNavigation() {
        MainWindow window(makeProject({fixture_.path()}), {}, false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *first = window.activePane();
        QVERIFY(first);
        QTest::keyClick(&window, Qt::Key_N, Qt::ControlModifier);
        QTRY_COMPARE(window.mdiArea()->subWindowList().size(), 2);
        auto *second = window.activePane();
        QVERIFY(second != first);
        QCOMPARE(second->path(), first->path());
        second->navigate(fixture_.filePath("業務サンプル"));
        QCOMPARE(first->path(), fixture_.path());
        second->back();
        QCOMPARE(second->path(), fixture_.path());
        second->forward();
        QCOMPARE(second->path(), fixture_.filePath("業務サンプル"));
        trigger(window, "pane.close");
        QTRY_COMPARE(window.mdiArea()->subWindowList().size(), 1);
        trigger(window, "pane.close");
        QTRY_COMPARE(window.mdiArea()->subWindowList().size(), 0);
        QVERIFY(window.isVisible());
        QString error;
        QVERIFY2(validateProject(window.snapshot(), &error), qPrintable(error));
    }
    void searchHiddenAndTextShortcuts() {
        MainWindow window(makeProject({fixture_.path()}), {}, false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *pane = window.activePane();
        auto *details = pane->findChild<QTreeView *>("details");
        auto rows = [&] { return details->model()->rowCount(details->rootIndex()); };
        QTRY_COMPARE(rows(), 6);
        pane->setHidden(true);
        QTRY_COMPARE(rows(), 7);
        auto *search = pane->findChild<QLineEdit *>("search");
        search->setText("ITEM");
        QTRY_COMPARE(rows(), 2);
        QCOMPARE(details->model()->index(0, 0, details->rootIndex()).data().toString(), QString("item2.txt"));
        search->setFocus();
        QTest::keyClick(search, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(search, Qt::Key_Backspace);
        QCOMPARE(search->text(), QString());
        QCOMPARE(pane->path(), fixture_.path());
        QTRY_COMPARE(rows(), 7);
        const auto path = pane->path();
        auto *address = pane->findChild<QLineEdit *>("address");
        trigger(window, "navigate.address");
        QCOMPARE(address->selectedText(), path);
        QTest::keyClick(address, Qt::Key_Backspace);
        QCOMPARE(address->text(), QString());
        QCOMPARE(pane->path(), path);
        QVERIFY(QFileInfo::exists(fixture_.filePath("item2.txt")));
    }
    void saveRestoreLayoutsAndMissingFolders() {
        QTemporaryDir storage;
        MainWindow first(makeProject({fixture_.path(), fixture_.filePath("業務サンプル"), fixture_.filePath("missing")}), {}, false);
        first.show();
        QVERIFY(QTest::qWaitForWindowExposed(&first));
        first.arrange("tile_grid");
        first.setTabbed(true);
        QCOMPARE(first.mdiArea()->subWindowList().size(), 3);
        first.setTabbed(false);
        first.activePane()->setViewMode("icons");
        first.activePane()->setHidden(true);
        QString error;
        const QString path = storage.filePath("saved.mlex-project.json");
        QVERIFY2(first.saveTo(path, &error), qPrintable(error));
        QVERIFY(!first.dirty());
        auto store = std::make_unique<ProjectStore>();
        QJsonObject project;
        QVERIFY(store->load(path, &project, &error));
        MainWindow second(project, std::move(store), false);
        second.show();
        QVERIFY(QTest::qWaitForWindowExposed(&second));
        QCOMPARE(second.mdiArea()->subWindowList().size(), 3);
        QCOMPARE(second.activePane()->path(), first.activePane()->path());
        QCOMPARE(second.activePane()->icons(), first.activePane()->icons());
        QVERIFY(!second.dirty());
        QVERIFY2(validateProject(second.snapshot(), &error), qPrintable(error));
        QCOMPARE(second.snapshot()["panes"].toArray()[2].toObject()["location"], project["panes"].toArray()[2].toObject()["location"]);
    }
    void geometryTabsOrderAndIme() {
        auto project = makeProject({fixture_.path(), fixture_.filePath("業務サンプル")});
        auto panes = project["panes"].toArray();
        auto edge = panes[0].toObject();
        edge["rect"] = QJsonObject{{"x", 0.99}, {"y", 0.99}, {"width", 0.01}, {"height", 0.01}};
        panes[0] = edge;
        project["panes"] = panes;
        MainWindow window(project, {}, false);
        window.resize(1280, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.mdiArea()->viewport()->rect().contains(window.mdiArea()->subWindowList().first()->geometry()));
        window.arrange("tile_vertical");
        const auto before = window.snapshot()["panes"].toArray();
        window.setTabbed(true);
        auto *bar = window.mdiArea()->findChild<QTabBar *>();
        QVERIFY(bar);
        bar->moveTab(0, 1);
        const auto tabs = window.snapshot()["panes"].toArray();
        QCOMPARE(tabs[0].toObject()["pane_id"], before[1].toObject()["pane_id"]);
        QCOMPARE(tabs[1].toObject()["rect"], before[0].toObject()["rect"]);
        window.setTabbed(false);
        const auto after = window.snapshot()["panes"].toArray();
        QCOMPARE(after[1].toObject()["rect"], before[0].toObject()["rect"]);
        auto *address = window.activePane()->findChild<QLineEdit *>("address");
        address->setFocus();
        QInputMethodEvent preedit("にほん", {});
        QApplication::sendEvent(address, &preedit);
        QVERIFY(!window.findChild<QAction *>("pane.new")->isEnabled());
        QInputMethodEvent commit;
        commit.setCommitString("日本");
        QApplication::sendEvent(address, &commit);
        QVERIFY(window.findChild<QAction *>("pane.new")->isEnabled());
        window.activePane()->focusAddress(true);
        window.activePane()->focusAddress(true);
        auto *details = window.activePane()->findChild<QTreeView *>("details");
        details->header()->moveSection(1, 2);
        const QJsonValue columnOrder = window.activePane()->state().value("view").toObject().value("columns");
        window.activePane()->setViewMode("icons");
        window.activePane()->setViewMode("details");
        QCOMPARE(window.activePane()->state()["view"].toObject()["columns"], columnOrder);
    }
    void refreshPreservesSelectionAndState() {
        MainWindow window(makeProject({fixture_.path()}), {}, false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *pane = window.activePane();
        auto *details = pane->findChild<QTreeView *>("details");
        QTRY_COMPARE(details->model()->rowCount(details->rootIndex()), 6);
        auto *search = pane->findChild<QLineEdit *>("search");
        search->setText("item2");
        QTRY_COMPARE(details->model()->rowCount(details->rootIndex()), 1);
        details->selectionModel()->select(details->model()->index(0, 0, details->rootIndex()), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        const auto selected = pane->selectedPaths();
        QCOMPARE(selected, QStringList{fixture_.filePath("item2.txt")});
        trigger(window, "navigate.refresh");
        QTRY_COMPARE(pane->selectedPaths(), selected);
        QCOMPARE(search->text(), QString("item2"));
        QCOMPARE(pane->path(), fixture_.path());
        // Search and selection do not create an unsaved project change.
        QVERIFY(!window.dirty());
    }
    void keyboardCycleAndIconSelection() {
        MainWindow window(makeProject({fixture_.path(), fixture_.path(), fixture_.path()}), {}, false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto windows = window.mdiArea()->subWindowList(QMdiArea::CreationOrder);
        window.mdiArea()->setActiveSubWindow(windows[0]);
        for (int expected : {1, 2, 0}) {
            window.activePane()->focusAddress();
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab, Qt::ControlModifier);
            QCOMPARE(window.mdiArea()->activeSubWindow(), windows[expected]);
        }
        window.setTabbed(true);
        auto *bar = window.mdiArea()->findChild<QTabBar *>();
        bar->moveTab(0, 2);
        window.mdiArea()->setActiveSubWindow(windows[1]);
        window.activePane()->focusAddress();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab, Qt::ControlModifier);
        QCOMPARE(window.mdiArea()->activeSubWindow(), windows[2]);
        auto *pane = window.activePane();
        pane->setViewMode("icons");
        auto *icons = pane->findChild<QListView *>("icons");
        QTRY_VERIFY(icons->model()->rowCount(icons->rootIndex()) > 0);
        icons->selectionModel()->select(icons->model()->index(0, 0, icons->rootIndex()), QItemSelectionModel::ClearAndSelect);
        QCOMPARE(pane->selectedPaths().size(), 1);
    }
    void twentyPanesAndSyntheticPreview() {
        QStringList directories;
        for (int i = 0; i < 20; ++i) directories.append(fixture_.path());
        MainWindow window(makeProject(directories), {}, false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.mdiArea()->subWindowList().size(), 20);
        QString error;
        QVERIFY2(validateProject(window.snapshot(), &error), qPrintable(error));
        const QString capture = qEnvironmentVariable("MLEX_TEST_CAPTURE");
        if (!capture.isEmpty()) {
            MainWindow preview(makeProject({fixture_.path(), fixture_.filePath("業務サンプル")}), {}, false);
            preview.resize(1440, 820);
            preview.show();
            QTest::qWait(300);
            preview.arrange("tile_vertical");
            QTest::qWait(200);
            // Restrict only this capture's trees to synthetic fixtures.
            for (auto *pane : preview.findChildren<FolderPane *>()) {
                auto *tree = pane->findChild<QTreeView *>("folderTree");
                auto *proxy = static_cast<DirectoryProxy *>(tree->model());
                auto *model = static_cast<QFileSystemModel *>(proxy->sourceModel());
                tree->setRootIndex(proxy->mapFromSource(model->index(fixture_.path())));
            }
            QVERIFY(preview.grab().save(capture));
        }
    }
};
QTEST_MAIN(WorkspaceTest)
#include "test_workspace.moc"
