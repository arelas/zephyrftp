// End-to-end test of the keyboard shortcuts added to FileTreeView/
// FilePaneWidget/MainWindow: Delete (confirmAndDelete), F2 (rename), F5
// (refresh), Ctrl+A (select all), and Ctrl+C/Ctrl+V (cross-pane
// copy-then-paste, built entirely on the existing enqueueEntries()
// TransferManager plumbing — see MainWindow::onPasteRequested()'s own
// doc comment).
//
// Drives a REAL MainWindow (same pattern smoke-test/sync-browsing-test/
// transfer-queue-test already establish for constructing one directly)
// with two real LocalBackend panes, real temp directories, and real
// QKeyEvents via QTest::keyClick() — not simulated by calling private
// slots directly, so this actually proves FileTreeView::keyPressEvent()
// itself dispatches correctly, not just that the handler methods work
// in isolation. Delete/Rename pop real QMessageBox/QInputDialog
// instances, driven via the same QTimer-fires-during-exec() technique
// conflict_resolution_test.cpp/recursive_delete_test.cpp/navigation_test.cpp
// already established.
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTest>
#include <QMessageBox>
#include <QInputDialog>
#include <QAbstractButton>
#include <QTreeView>
#include <QStatusBar>
#include "ui/MainWindow.h"
#include "ui/FilePaneWidget.h"
#include "transfer/TransferManager.h"

namespace {
bool writeFile(const QString &path, const QString &content)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content.toUtf8());
    return true;
}

// Same helper recursive_delete_test.cpp already established.
bool clickDialogButton(const QString &buttonText)
{
    auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (!box)
        return false;
    for (QAbstractButton *button : box->buttons()) {
        if (button->text().contains(buttonText, Qt::CaseInsensitive)) {
            button->click();
            return true;
        }
    }
    return false;
}

bool fillInputDialog(const QString &text)
{
    auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
    if (!dialog)
        return false;
    dialog->setTextValue(text);
    dialog->accept();
    return true;
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    const QString base = QStringLiteral("/tmp/keyboard_shortcuts_test");
    QDir(base).removeRecursively();
    QDir().mkpath(base + "/left");
    QDir().mkpath(base + "/right");

    writeFile(base + "/left/toDelete.txt", "delete me");
    writeFile(base + "/left/toRename.txt", "rename me");
    writeFile(base + "/left/toCopy.txt", "copy me");
    writeFile(base + "/left/toCopySamePane.txt", "copy me too");
    writeFile(base + "/left/toCopyStale.txt", "copy me stale");
    QDir().mkpath(base + "/left/elsewhere");

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    MainWindow window;
    window.resize(1200, 800);

    auto *leftPane = window.findChild<FilePaneWidget *>("leftPane");
    auto *rightPane = window.findChild<FilePaneWidget *>("rightPane");
    auto *manager = window.findChild<TransferManager *>();
    check("found leftPane, rightPane, and TransferManager", leftPane && rightPane && manager);
    if (!leftPane || !rightPane || !manager) {
        qDebug() << "[test] SETUP FAILED — aborting";
        return 1;
    }

    auto *leftView = leftPane->findChild<QTreeView *>();
    auto *rightView = rightPane->findChild<QTreeView *>();
    check("found both panes' QTreeViews", leftView && rightView);

    int itemsAdded = 0;
    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &) {
        itemsAdded++;
    });

    QTimer::singleShot(200, &app, [&]() {
        leftPane->navigateTo(base + "/left");
        rightPane->navigateTo(base + "/right");
    });

    // ---------- Phase A: Delete key ----------
    QTimer::singleShot(500, &app, [&]() {
        check("setup: left pane at its own root", leftPane->currentDirectory() == base + "/left");

        bool found = false;
        for (const RemoteEntry &e : leftPane->selectedEntries())
            Q_UNUSED(e);
        // Select toDelete.txt via the model directly (same technique
        // sort_and_commands_test.cpp uses) rather than a real mouse
        // click — selection mechanics aren't what this test is proving.
        auto *model = leftView->model();
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->data(model->index(row, 0), Qt::DisplayRole).toString() == QStringLiteral("toDelete.txt")) {
                leftView->selectionModel()->select(
                    model->index(row, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                found = true;
                break;
            }
        }
        check("Phase A: found and selected toDelete.txt", found);

        auto *dialogPump = new QTimer(&app);
        dialogPump->setInterval(50);
        int attempts = 0;
        QObject::connect(dialogPump, &QTimer::timeout, &app, [&, dialogPump]() mutable {
            if (++attempts > 40) { dialogPump->stop(); dialogPump->deleteLater(); return; }
            if (clickDialogButton(QStringLiteral("Yes"))) {
                dialogPump->stop();
                dialogPump->deleteLater();
            }
        });
        dialogPump->start();

        QTest::keyClick(leftView, Qt::Key_Delete);
    });

    QTimer::singleShot(1200, &app, [&]() {
        check("Phase A: Delete key removed toDelete.txt", !QFile::exists(base + "/left/toDelete.txt"));
    });

    // ---------- Phase B: F2 rename ----------
    QTimer::singleShot(1400, &app, [&]() {
        auto *model = leftView->model();
        bool found = false;
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->data(model->index(row, 0), Qt::DisplayRole).toString() == QStringLiteral("toRename.txt")) {
                leftView->selectionModel()->select(
                    model->index(row, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                found = true;
                break;
            }
        }
        check("Phase B: found and selected toRename.txt", found);

        auto *dialogPump = new QTimer(&app);
        dialogPump->setInterval(50);
        int attempts = 0;
        QObject::connect(dialogPump, &QTimer::timeout, &app, [&, dialogPump]() mutable {
            if (++attempts > 40) { dialogPump->stop(); dialogPump->deleteLater(); return; }
            if (fillInputDialog(QStringLiteral("renamedByF2.txt"))) {
                dialogPump->stop();
                dialogPump->deleteLater();
            }
        });
        dialogPump->start();

        QTest::keyClick(leftView, Qt::Key_F2);
    });

    QTimer::singleShot(2100, &app, [&]() {
        check("Phase B: F2 renamed toRename.txt to renamedByF2.txt",
              !QFile::exists(base + "/left/toRename.txt")
                  && QFile::exists(base + "/left/renamedByF2.txt"));
    });

    // ---------- Phase C: F5 refresh ----------
    QTimer::singleShot(2300, &app, [&]() {
        // Written directly to disk, bypassing the pane entirely — the
        // model shouldn't know about this until a real refresh happens.
        writeFile(base + "/left/addedOnDisk.txt", "appeared via F5");

        auto *model = leftView->model();
        bool visibleBeforeRefresh = false;
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->data(model->index(row, 0), Qt::DisplayRole).toString() == QStringLiteral("addedOnDisk.txt"))
                visibleBeforeRefresh = true;
        }
        check("Phase C: addedOnDisk.txt not yet visible before F5 (model is stale, as expected)",
              !visibleBeforeRefresh);

        QTest::keyClick(leftView, Qt::Key_F5);
    });

    QTimer::singleShot(2800, &app, [&]() {
        auto *model = leftView->model();
        bool visibleAfterRefresh = false;
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->data(model->index(row, 0), Qt::DisplayRole).toString() == QStringLiteral("addedOnDisk.txt"))
                visibleAfterRefresh = true;
        }
        check("Phase C: F5 refreshed the listing — addedOnDisk.txt now visible", visibleAfterRefresh);
    });

    // ---------- Phase D: Ctrl+A select all ----------
    QTimer::singleShot(3000, &app, [&]() {
        leftView->selectionModel()->clearSelection();
        QTest::keyClick(leftView, Qt::Key_A, Qt::ControlModifier);
        const int totalRows = leftView->model()->rowCount();
        const int selectedRows = leftView->selectionModel()->selectedRows().size();
        check("Phase D: Ctrl+A selected every row", totalRows > 0 && selectedRows == totalRows);
    });

    // ---------- Phase E: Ctrl+C then Ctrl+V into the OTHER pane ----------
    QTimer::singleShot(3200, &app, [&]() {
        auto *model = leftView->model();
        bool found = false;
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->data(model->index(row, 0), Qt::DisplayRole).toString() == QStringLiteral("toCopy.txt")) {
                leftView->selectionModel()->select(
                    model->index(row, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                found = true;
                break;
            }
        }
        check("Phase E: found and selected toCopy.txt", found);

        QTest::keyClick(leftView, Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(rightView, Qt::Key_V, Qt::ControlModifier);
    });

    QTimer::singleShot(4200, &app, [&]() {
        check("Phase E: Ctrl+C / Ctrl+V transferred toCopy.txt into the right pane",
              QFile::exists(base + "/right/toCopy.txt"));
        check("Phase E: at least one transfer item was actually added", itemsAdded >= 1);
    });

    // ---------- Phase F: paste refused into the SAME pane it was copied from ----------
    QTimer::singleShot(4400, &app, [&]() {
        auto *model = leftView->model();
        bool found = false;
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->data(model->index(row, 0), Qt::DisplayRole).toString() == QStringLiteral("toCopySamePane.txt")) {
                leftView->selectionModel()->select(
                    model->index(row, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                found = true;
                break;
            }
        }
        check("Phase F: found and selected toCopySamePane.txt", found);

        QTest::keyClick(leftView, Qt::Key_C, Qt::ControlModifier);
        itemsAdded = 0;   // reset the counter right before the same-pane paste attempt
        QTest::keyClick(leftView, Qt::Key_V, Qt::ControlModifier);
    });

    QTimer::singleShot(4700, &app, [&]() {
        check("Phase F: pasting into the same pane it was copied from added NO transfer item",
              itemsAdded == 0);
        check("Phase F: the status bar explained why",
              window.statusBar()->currentMessage().contains(QStringLiteral("same pane"), Qt::CaseInsensitive));
    });

    // ---------- Phase G: paste refused once the source pane has navigated away ----------
    QTimer::singleShot(4900, &app, [&]() {
        auto *model = leftView->model();
        bool found = false;
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->data(model->index(row, 0), Qt::DisplayRole).toString() == QStringLiteral("toCopyStale.txt")) {
                leftView->selectionModel()->select(
                    model->index(row, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                found = true;
                break;
            }
        }
        check("Phase G: found and selected toCopyStale.txt", found);

        QTest::keyClick(leftView, Qt::Key_C, Qt::ControlModifier);
        leftPane->navigateTo(base + "/left/elsewhere");
    });

    QTimer::singleShot(5400, &app, [&]() {
        check("Phase G: setup — left pane genuinely navigated away",
              leftPane->currentDirectory() == base + "/left/elsewhere");

        itemsAdded = 0;
        QTest::keyClick(rightView, Qt::Key_V, Qt::ControlModifier);
    });

    QTimer::singleShot(5700, &app, [&]() {
        check("Phase G: paste after the source pane navigated away added NO transfer item",
              itemsAdded == 0);
        check("Phase G: toCopyStale.txt was NOT transferred into the right pane",
              !QFile::exists(base + "/right/toCopyStale.txt"));
        check("Phase G: the status bar explained why",
              window.statusBar()->currentMessage().contains(QStringLiteral("changed"), Qt::CaseInsensitive));

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}
