#include "domain/project.h"
#include "persistence/project_store.h"
#include "ui/folder_pane.h"
#include "ui/main_window.h"
#include "ui/file_clipboard.h"
#include "application/operation_service.h"
#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QTemporaryDir>
#include <QTabBar>
#include <QInputMethodEvent>
#include <QHeaderView>
#include <QClipboard>
#include <QInputDialog>
#include <QMessageBox>
#include <QMenu>
#include <QMimeData>
#include <QStatusBar>
#include <QScopeGuard>
#include <QLabel>
#include <QTreeView>
#include <QtTest>

using namespace mlex;
class WorkspaceTest : public QObject {
    Q_OBJECT
    QTemporaryDir fixture_;
    QTemporaryDir privateData_;
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
        QVERIFY(privateData_.isValid());
        QVERIFY(QDir(privateData_.path()).mkdir("home"));
        qputenv("HOME", privateData_.filePath("home").toUtf8());
        qputenv("XDG_STATE_HOME", privateData_.filePath("state").toUtf8());
        qputenv("XDG_DATA_HOME", privateData_.filePath("data").toUtf8());
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
        QCOMPARE(second->path(), QDir::homePath());
        QVERIFY(second->path() != first->path());
        second->navigate(fixture_.filePath("業務サンプル"));
        QCOMPARE(first->path(), fixture_.path());
        second->back();
        QCOMPARE(second->path(), QDir::homePath());
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
    void perPaneStatusAndSafeTextCommands_data() {
        QTest::addColumn<QString>("localeName");
        QTest::newRow("japanese") << QString("ja_JP");
        QTest::newRow("english") << QString("en_US");
    }
    void perPaneStatusAndSafeTextCommands() {
        QFETCH(QString, localeName);
        const QLocale previousLocale;
        const auto restoreLocale = qScopeGuard([previousLocale] { QLocale::setDefault(previousLocale); });
        QLocale::setDefault(QLocale(localeName));
        MainWindow window(makeProject({fixture_.path(), fixture_.filePath("業務サンプル")}), {}, false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto children = window.mdiArea()->subWindowList();
        window.mdiArea()->setActiveSubWindow(children.first());
        auto *pane = window.activePane();
        auto *details = pane->findChild<QTreeView *>("details");
        auto *summary = pane->findChild<QLabel *>("paneSummary");
        QVERIFY(pane->findChild<QStatusBar *>("paneStatusBar"));
        QTRY_VERIFY(summary->text().contains("6 項目"));
        pane->selectPaths({fixture_.filePath("item2.txt")});
        details->setFocus();
        QTRY_VERIFY(summary->text().contains("1 件選択"));
        const auto selectedSize = QFileInfo(fixture_.filePath("item2.txt")).size();
        QTRY_VERIFY(summary->text().endsWith(QLocale().formattedDataSize(selectedSize)));
        auto *other = qobject_cast<FolderPane *>(children.last()->widget());
        QTRY_VERIFY(other->findChild<QLabel *>("paneSummary")->text().contains("2 項目"));
        QVERIFY(other->findChild<QLabel *>("paneSummary")->text().contains("0 件選択"));
        auto *address = pane->findChild<QLineEdit *>("address");
        address->setFocus();
        address->setText("text editing");
        address->selectAll();
        for (const auto *name : {"file.rename", "file.copy", "file.cut", "file.paste", "file.trash", "file.delete", "file.undo", "file.redo"})
            QVERIFY2(!window.findChild<QAction *>(name)->isEnabled(), name);
        QTest::keyClick(address, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), QString("text editing"));
        QTest::keyClick(address, Qt::Key_Delete);
        QCOMPARE(address->text(), QString());
        QTest::keyClick(address, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(address->text(), QString("text editing"));
        QVERIFY(QFileInfo::exists(fixture_.filePath("item2.txt")));
        QMenu popup(&window);
        popup.popup(window.mapToGlobal(QPoint(20, 20)));
        QApplication::processEvents();
        QVERIFY(!pane->fileViewFocused());
        popup.close();
        details->setFocus();
        QApplication::processEvents();
        QVERIFY(window.findChild<QAction *>("file.rename")->isEnabled());
        QInputMethodEvent composing("なまえ", {});
        QApplication::sendEvent(details, &composing);
        QVERIFY(!window.findChild<QAction *>("file.rename")->isEnabled());
        QVERIFY(!window.findChild<QAction *>("file.trash")->isEnabled());
        QInputMethodEvent commit;
        QApplication::sendEvent(details, &commit);
        pane->findChild<QLineEdit *>("search")->setText("item10");
        QTRY_VERIFY(summary->text().contains("1 項目"));
    }
    void fileOperationKeyboardWorkflow() {
        QTemporaryDir tmp;
        QVERIFY(QDir(tmp.path()).mkdir("target"));
        QFile file(tmp.filePath("example.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Synthetic keyboard fixture\n");
        file.close();
        MainWindow window(makeProject({tmp.path(), tmp.filePath("target")}), {}, false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto children = window.mdiArea()->subWindowList();
        window.mdiArea()->setActiveSubWindow(children.first());
        auto *pane = window.activePane();
        auto *details = pane->findChild<QTreeView *>("details");
        QTRY_COMPARE(details->model()->rowCount(details->rootIndex()), 2);
        pane->selectPaths({file.fileName()});
        details->setFocus();
        QSignalSpy finished(window.operations(), &OperationService::finished);
        auto answerName = [&window](const QString &name, bool checkStem) {
            QTimer::singleShot(0, &window, [name, checkStem] {
                auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                const auto initiallySelected = dialog->findChild<QLineEdit *>()->selectedText();
                dialog->setTextValue(name);
                dialog->accept();
                if (checkStem) QCOMPARE(initiallySelected, QString("example"));
            });
        };
        answerName("renamed.txt", true);
        QTest::keyClick(details, Qt::Key_F2);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY(QFileInfo::exists(tmp.filePath("renamed.txt")));
        QTRY_COMPARE(pane->selectedPaths(), QStringList{tmp.filePath("renamed.txt")});
        QVERIFY(window.operations()->canUndo());
        // Offscreen has no window manager to reactivate the owner after a dialog.
        window.activateWindow();
        details->setFocus();
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(details));
        QVERIFY(window.findChild<QAction *>("file.undo")->isEnabled());
        QTest::keyClick(details, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
        QVERIFY(QFileInfo::exists(file.fileName()));
        QTest::keyClick(details, Qt::Key_Y, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 10000);
        QVERIFY(QFileInfo::exists(tmp.filePath("renamed.txt")));
        answerName("new folder", false);
        QTest::keyClick(details, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 4, 10000);
        QVERIFY(QFileInfo(tmp.filePath("new folder")).isDir());
        QCOMPARE(window.mdiArea()->subWindowList().size(), 2);
        window.activateWindow();
        details->setFocus();
        QTRY_VERIFY(details->hasFocus());
        pane->selectPaths({tmp.filePath("renamed.txt")});
        QTest::keyClick(details, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(!readFileTransfer(QApplication::clipboard()->mimeData()).cut);
        window.mdiArea()->setActiveSubWindow(children.last());
        auto *target = window.activePane();
        auto *targetView = target->findChild<QTreeView *>("details");
        targetView->setFocus();
        QTest::keyClick(targetView, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 5, 10000);
        QVERIFY(QFileInfo::exists(tmp.filePath("target/renamed.txt")));
        QTest::keyClick(targetView, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 6, 10000);
        QVERIFY(!QFileInfo::exists(tmp.filePath("target/renamed.txt")));
        window.mdiArea()->setActiveSubWindow(children.first());
        details->setFocus();
        pane->selectPaths({tmp.filePath("renamed.txt")});
        QTest::keyClick(details, Qt::Key_X, Qt::ControlModifier);
        QVERIFY(readFileTransfer(QApplication::clipboard()->mimeData()).cut);
        QVERIFY(QFileInfo::exists(tmp.filePath("renamed.txt")));
        window.mdiArea()->setActiveSubWindow(children.last());
        targetView->setFocus();
        QTest::keyClick(targetView, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 7, 10000);
        QVERIFY(!QFileInfo::exists(tmp.filePath("renamed.txt")));
        QVERIFY(QFileInfo::exists(tmp.filePath("target/renamed.txt")));
        QVERIFY(readFileTransfer(QApplication::clipboard()->mimeData()).items.isEmpty());
        QTRY_COMPARE(target->selectedPaths(), QStringList{tmp.filePath("target/renamed.txt")});
        auto deletionChoice = [&window](bool accept) {
            QTimer::singleShot(0, &window, [accept] {
                auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                dialog->done(accept ? QMessageBox::Ok : QMessageBox::Cancel);
            });
        };
        deletionChoice(false);
        QTest::keyClick(targetView, Qt::Key_Delete, Qt::ShiftModifier);
        QCOMPARE(finished.size(), 7);
        QVERIFY(QFileInfo::exists(tmp.filePath("target/renamed.txt")));
        window.activateWindow();
        targetView->setFocus();
        QTRY_VERIFY(targetView->hasFocus());
        deletionChoice(true);
        QTest::keyClick(targetView, Qt::Key_Delete);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 8, 10000);
        QVERIFY(!QFileInfo::exists(tmp.filePath("target/renamed.txt")));
        QVERIFY(QFileInfo::exists(privateData_.filePath("data/Trash/files/renamed.txt")));
        QVERIFY(!window.operations()->canUndo());
    }
    void clipboardFormatsAndStaleCut() {
        QTemporaryDir tmp;
        QFile file(tmp.filePath("space ' 日本語.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("Synthetic clipboard data"); file.close();
        auto mime = makeFileTransfer(captureItems({file.fileName()}), true);
        const auto original = readFileTransfer(mime.get());
        QVERIFY(original.error.isEmpty());
        QVERIFY(original.cut);
        QMimeData gnome;
        gnome.setData("x-special/gnome-copied-files", mime->data("x-special/gnome-copied-files"));
        QCOMPARE(readFileTransfer(&gnome).items.first().toObject().value("path").toString(), file.fileName());
        QVERIFY(readFileTransfer(&gnome).cut);
        QMimeData kde;
        kde.setUrls(mime->urls());
        kde.setData("application/x-kde-cutselection", "1");
        QVERIFY(readFileTransfer(&kde).cut);
        QMimeData localhost;
        auto localUrl = QUrl::fromLocalFile(file.fileName());
        localUrl.setHost("localhost");
        localhost.setUrls({localUrl});
        QVERIFY(readFileTransfer(&localhost).error.isEmpty());
        for (const auto &uri : {"https://example.invalid/file", "file://example.invalid/file", "file:///tmp/example?query", "file:///tmp/example#fragment", "file://user@localhost/tmp/example"}) {
            QMimeData invalid;
            invalid.setUrls({QUrl(uri)});
            QVERIFY(!readFileTransfer(&invalid).error.isEmpty());
        }
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("Changed after cut"); file.close();
        const auto capturedAgain = readFileTransfer(mime.get());
        QCOMPARE(capturedAgain.items, original.items);
        QVERIFY(captureItems({file.fileName()}) != original.items);
        QMimeData unknown;
        unknown.setText(file.fileName());
        QVERIFY(!readFileTransfer(&unknown).error.isEmpty());
        QApplication::clipboard()->clear();
    }
    void treeEnterOpensAndExpandsFolder() {
        MainWindow window(makeProject({fixture_.path(), fixture_.path()}), {}, false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto children = window.mdiArea()->subWindowList();
        window.mdiArea()->setActiveSubWindow(children.first());
        auto *pane = window.activePane();
        auto *other = qobject_cast<FolderPane *>(children.last()->widget());
        auto *tree = pane->findChild<QTreeView *>("folderTree");
        auto *details = pane->findChild<QTreeView *>("details");
        auto *treeProxy = static_cast<DirectoryProxy *>(tree->model());
        auto *listProxy = static_cast<DirectoryProxy *>(details->model());
        auto *model = static_cast<QFileSystemModel *>(treeProxy->sourceModel());
        const QString target = fixture_.filePath("業務サンプル");
        auto index = [&] { return treeProxy->mapFromSource(model->index(target)); };
        QTRY_VERIFY(index().isValid());
        QTRY_COMPARE(details->model()->rowCount(details->rootIndex()), 6);
        for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
            pane->navigate(fixture_.path());
            tree->setFocus();
            QTRY_VERIFY(tree->hasFocus());
            tree->collapse(index());
            tree->setCurrentIndex(index());
            QCOMPARE(pane->path(), fixture_.path());
            QVERIFY(!tree->isExpanded(index()));
            const auto modifiers = key == Qt::Key_Enter ? Qt::KeypadModifier : Qt::NoModifier;
            QTest::keyClick(tree, key, modifiers);
            QTRY_COMPARE(pane->path(), target);
            QCOMPARE(model->filePath(listProxy->mapToSource(details->rootIndex())), target);
            QTRY_COMPARE(details->model()->rowCount(details->rootIndex()), 2);
            QVERIFY(tree->isExpanded(index()));
            QVERIFY(tree->hasFocus());
            QCOMPARE(other->path(), fixture_.path());
            QTest::keyClick(tree, key, modifiers);
            QVERIFY(tree->isExpanded(index()));
            QCOMPARE(pane->path(), target);
            pane->back();
            QCOMPARE(pane->path(), fixture_.path());
        }
    }
    void treeTargetsAndOpenFolderRename() {
        QTemporaryDir tmp;
        QVERIFY(QDir(tmp.path()).mkpath("folder/sub"));
        MainWindow window(makeProject({tmp.path(), tmp.filePath("folder/sub")}), {}, false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto children = window.mdiArea()->subWindowList();
        window.mdiArea()->setActiveSubWindow(children.first());
        auto *pane = window.activePane();
        auto *tree = pane->findChild<QTreeView *>("folderTree");
        auto *proxy = static_cast<DirectoryProxy *>(tree->model());
        auto *model = static_cast<QFileSystemModel *>(proxy->sourceModel());
        auto index = [&] { return proxy->mapFromSource(model->index(tmp.filePath("folder"))); };
        QTRY_VERIFY(index().isValid());
        tree->setFocus();
        tree->setCurrentIndex(index());
        QCOMPARE(pane->path(), tmp.path());
        QCOMPARE(pane->operationDirectory(), tmp.filePath("folder"));
        QSignalSpy finished(window.operations(), &OperationService::finished);
        QTimer::singleShot(0, &window, [] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->setTextValue("created in tree");
            dialog->accept();
        });
        QTest::keyClick(tree, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(QFileInfo(tmp.filePath("folder/created in tree")).isDir());
        QVERIFY(!QFileInfo::exists(tmp.filePath("created in tree")));
        window.activateWindow();
        auto *details = pane->findChild<QTreeView *>("details");
        details->setFocus();
        QTRY_VERIFY(details->hasFocus());
        pane->selectPaths({tmp.filePath("folder")});
        QTRY_COMPARE(pane->selectedPaths().size(), 1);
        QTimer::singleShot(0, &window, [] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->setTextValue("renamed folder");
            dialog->accept();
        });
        QTest::keyClick(details, Qt::Key_F2);
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(qobject_cast<FolderPane *>(children.last()->widget())->path(), tmp.filePath("renamed folder/sub"));
        window.activateWindow();
        details->setFocus();
        QTRY_VERIFY(details->hasFocus());
        bool contextRenameEnabled = true;
        QTimer::singleShot(0, &window, [&] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                contextRenameEnabled = window.findChild<QAction *>("file.rename")->isEnabled();
                menu->close();
            }
        });
        // A right-click in empty space must not target the previous selection.
        emit details->customContextMenuRequested(details->viewport()->rect().bottomRight() - QPoint(4, 4));
        QVERIFY(pane->selectedPaths().isEmpty());
        QVERIFY(!contextRenameEnabled);
    }
    void closeWaitsForRunningOperation() {
        QTemporaryDir tmp;
        MainWindow window(makeProject({tmp.path()}), {}, false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QSignalSpy finished(window.operations(), &OperationService::finished);
        QVERIFY(window.operations()->start(QJsonObject{{"kind", "mkdir"}, {"destination", tmp.path()}, {"name", "created before close"}}));
        QTimer::singleShot(0, &window, [] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            for (auto *button : dialog->buttons()) if (button->text() == "完了したら終了") { button->click(); return; }
            dialog->reject();
            QFAIL("Missing wait-for-operation choice");
        });
        QVERIFY(!window.close());
        QTRY_COMPARE(finished.size(), 1);
        QTRY_VERIFY(!window.isVisible());
        QVERIFY(QFileInfo(tmp.filePath("created before close")).isDir());
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
