// Real end-to-end test of recursive delete (with a warning) —
// FilePaneWidget::deleteEntriesAt(..., /*offerRecursiveDeleteOnFailure=*/true)
// is the same public entry point CompareSyncExecutor already calls (with
// the flag defaulted to false, deliberately unaffected by any of this —
// see compare-sync-test for that side), so this test drives it directly
// rather than simulating the context menu's own "Delete N items?"
// confirmation — that dialog is unchanged and untested at this level
// already (file_operations_test.cpp exercises deleteEntry() directly on
// the raw backend instead), matching this project's existing convention
// of testing at the seam that actually changed.
//
// This drives the REAL QMessageBox, same technique
// conflict_resolution_test.cpp already established: a QTimer fires while
// a dialog's still-blocking exec() call is pumping the event loop, finds
// it via QApplication::activeModalWidget(), and clicks a button by text.
#include <QApplication>
#include <QTimer>
#include <QElapsedTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QAbstractButton>
#include <functional>
#include <memory>
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"

namespace {
bool writeFile(const QString &path, const QString &content)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content.toUtf8());
    return true;
}

// Finds the currently-open QMessageBox (if any) and clicks whichever
// button's text contains buttonText (case-insensitive). Returns false
// (and does nothing) if no QMessageBox is currently open.
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
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Without this, closing the first real dialog (the only visible
    // window in this test — the pane is constructed but never shown)
    // triggers Qt's default "quit when the last window closes" behavior,
    // ending the test's event loop before later phases run.
    app.setQuitOnLastWindowClosed(false);

    const QString base = "/tmp/recursive_delete_test";
    QDir(base).removeRecursively();
    QDir().mkpath(base + "/pane");

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    // Wall-clock deadline for Phase E's own poll below — lets it exit as
    // soon as the recursive delete is actually confirmed done instead of
    // always waiting out the full 20800ms this test used to take
    // unconditionally, while keeping that same 20800ms as an unchanged
    // safety-net ceiling.
    QElapsedTimer clock;
    clock.start();

    auto *pane = new FilePaneWidget(new LocalBackend());

    // ===================================================================
    // Fixtures for every phase below, all created up front.
    // ===================================================================
    QDir().mkpath(base + "/pane/emptyfolder");
    writeFile(base + "/pane/singlefile.txt", "just a file");

    QDir().mkpath(base + "/pane/declinefolder/subdir");
    writeFile(base + "/pane/declinefolder/file1.txt", "keep me");
    writeFile(base + "/pane/declinefolder/subdir/file2.txt", "keep me too");

    QDir().mkpath(base + "/pane/bigfolder/sub1");
    QDir().mkpath(base + "/pane/bigfolder/sub2/nested");
    writeFile(base + "/pane/bigfolder/file1.txt", "a");
    writeFile(base + "/pane/bigfolder/file2.txt", "b");
    writeFile(base + "/pane/bigfolder/sub1/file3.txt", "c");
    writeFile(base + "/pane/bigfolder/sub2/nested/file4.txt", "d");

    QDir().mkpath(base + "/pane/mixed_empty");
    QDir().mkpath(base + "/pane/mixed_full");
    writeFile(base + "/pane/mixed_full/onlyfile.txt", "e");

    QTimer::singleShot(200, &app, [&]() {
        pane->navigateTo(base + "/pane");
    });

    // ===================================================================
    // Phase A: deleting an EMPTY folder is completely unaffected — no
    // new dialog, deleted immediately, same as before this feature.
    // ===================================================================
    QTimer::singleShot(1000, &app, [&]() {
        pane->deleteEntriesAt({{base + "/pane/emptyfolder", true}},
                               /*offerRecursiveDeleteOnFailure=*/true);
    });

    QTimer::singleShot(2500, &app, [&]() {
        const bool dialogOpen = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()) != nullptr;
        check("Phase A: no dialog appeared for an empty folder", !dialogOpen);
        check("Phase A: the empty folder is gone", !QDir(base + "/pane/emptyfolder").exists());
    });

    // ===================================================================
    // Phase B: deleting a single FILE is completely unaffected either —
    // files never populate m_recursiveDeleteCandidates at all.
    // ===================================================================
    QTimer::singleShot(3000, &app, [&]() {
        pane->deleteEntriesAt({{base + "/pane/singlefile.txt", false}},
                               /*offerRecursiveDeleteOnFailure=*/true);
    });

    QTimer::singleShot(4500, &app, [&]() {
        const bool dialogOpen = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()) != nullptr;
        check("Phase B: no dialog appeared for a single file", !dialogOpen);
        check("Phase B: the file is gone", !QFile::exists(base + "/pane/singlefile.txt"));
    });

    // ===================================================================
    // Phase C: a non-empty folder's plain delete fails, the recursive-
    // delete warning appears, and DECLINING it (No) leaves the folder
    // and everything inside completely untouched.
    // ===================================================================
    QTimer::singleShot(5000, &app, [&]() {
        pane->deleteEntriesAt({{base + "/pane/declinefolder", true}},
                               /*offerRecursiveDeleteOnFailure=*/true);
    });

    QTimer::singleShot(6800, &app, [&]() {
        const bool clicked = clickDialogButton("No");
        check("Phase C: recursive-delete warning appeared for declinefolder", clicked);
    });

    QTimer::singleShot(8500, &app, [&]() {
        check("Phase C: declining left the folder itself in place",
              QDir(base + "/pane/declinefolder").exists());
        check("Phase C: declining left its file untouched",
              QFile::exists(base + "/pane/declinefolder/file1.txt"));
        check("Phase C: declining left its nested subfolder's file untouched",
              QFile::exists(base + "/pane/declinefolder/subdir/file2.txt"));
    });

    // ===================================================================
    // Phase D: a non-empty, multi-level folder's plain delete fails, the
    // warning appears, and ACCEPTING it (Yes) deletes everything —
    // files at every depth, subfolders deepest-first, and finally the
    // root folder itself.
    // ===================================================================
    QTimer::singleShot(9500, &app, [&]() {
        pane->deleteEntriesAt({{base + "/pane/bigfolder", true}},
                               /*offerRecursiveDeleteOnFailure=*/true);
    });

    QTimer::singleShot(11300, &app, [&]() {
        const bool clicked = clickDialogButton("Yes");
        check("Phase D: recursive-delete warning appeared for bigfolder", clicked);
    });

    QTimer::singleShot(14000, &app, [&]() {
        check("Phase D: accepting deleted the root folder itself",
              !QDir(base + "/pane/bigfolder").exists());
        check("Phase D: accepting deleted every file at every depth",
              !QFile::exists(base + "/pane/bigfolder/file1.txt")
              && !QFile::exists(base + "/pane/bigfolder/file2.txt")
              && !QFile::exists(base + "/pane/bigfolder/sub1/file3.txt")
              && !QFile::exists(base + "/pane/bigfolder/sub2/nested/file4.txt"));
    });

    // ===================================================================
    // Phase E: a mixed multi-select batch — one empty folder, one
    // non-empty folder, deleted together in ONE deleteEntriesAt() call.
    // The empty one must delete immediately with no dialog; the
    // non-empty one gets its own follow-up warning. Also directly
    // exercises the generalized m_modalDialogInProgress guard: this
    // phase's own warning fires while nothing else is open (a plain
    // sequential case, not a true simultaneous burst — Phase H in
    // conflict_resolution_test.cpp already covers the guard's core
    // reentrancy proof; this phase confirms the SAME guard correctly
    // handles a mix of a directory that resolves with no dialog at all
    // alongside one that needs its own).
    // ===================================================================
    QTimer::singleShot(15000, &app, [&]() {
        pane->deleteEntriesAt({{base + "/pane/mixed_empty", true},
                                {base + "/pane/mixed_full", true}},
                               /*offerRecursiveDeleteOnFailure=*/true);
    });

    QTimer::singleShot(16800, &app, [&]() {
        check("Phase E: the empty folder in the mixed batch is already gone",
              !QDir(base + "/pane/mixed_empty").exists());
    });

    QTimer::singleShot(18600, &app, [&]() {
        const bool clicked = clickDialogButton("Yes");
        check("Phase E: recursive-delete warning appeared for the non-empty folder "
              "in the mixed batch", clicked);
    });

    // The delete itself is a fast local-filesystem op once the dialog is
    // answered — poll every 100ms (via the top-level event loop's own
    // self-rescheduling QTimer, the same safe technique
    // conflict-resolution-test's own Phase H poll uses — NOT a nested
    // processEvents() spin, which that file's own Phase E comment
    // documents as unsound for waiting on a still-open modal dialog;
    // this poll only waits on plain filesystem state, so that concern
    // doesn't apply here) and exit as soon as both conditions are true,
    // falling back to the original 20800ms total as an unchanged
    // safety-net ceiling if it's ever genuinely slower.
    auto pollPhaseE = std::make_shared<std::function<void()>>();
    *pollPhaseE = [&app, &allPass, &check, &clock, base, pollPhaseE]() {
        const bool folderGone = !QDir(base + "/pane/mixed_full").exists();
        const bool fileGone = !QFile::exists(base + "/pane/mixed_full/onlyfile.txt");
        if (!(folderGone && fileGone) && clock.elapsed() < 20800) {
            QTimer::singleShot(100, &app, *pollPhaseE);
            return;
        }

        check("Phase E: accepting deleted the non-empty folder too", folderGone);
        check("Phase E: accepting deleted its file", fileGone);

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    };
    QTimer::singleShot(18800, &app, *pollPhaseE);

    return app.exec();
}
