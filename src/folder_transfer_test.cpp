// Headless functional test of whole-folder transfer — recursive
// enumeration, mirrored directory structure creation on the
// destination, and file transfer for everything found — against a real
// LocalBackend and a real nested directory tree, not mocked state.
//
// Structure under test:
//   myfolder/
//     a.txt
//     subdir1/
//       b.txt
//       c.txt
//     subdir2/
//       nested/
//         d.txt
//     emptydir/          (genuinely empty — no files, no subdirs)
//
// This specifically covers the two things most likely to have a subtle
// bug: that FolderEnumerator's parent-before-child ordering guarantee
// actually holds (so a child directory's creation is never attempted
// before its parent exists), and that a directory containing no files
// anywhere in its own subtree (emptydir) still gets created on the
// destination rather than being silently skipped because nothing
// "needed" it.
//
// SftpBackend's implementation of listDirectoryForEnumeration() is NOT
// covered here — no live SFTP server is available in this environment,
// the same limitation already flagged elsewhere in this project.
//
// Also covers a regression for a real bug found by code review:
// enqueueFolder() used to stash its pending root-conflict-check state
// (requestId/sourcePane/destPane/folderName) in single shared scalar
// members instead of a per-request map — the same bug class Move's own
// conflict-check tracking already hit and fixed once (see
// TransferManager.h's m_pendingFolderConflictChecks comment). Dragging
// two folders onto a pane at once (MainWindow's drop handling calls
// enqueueFolder() once per folder in a plain synchronous loop) let the
// second call silently clobber the first's stashed state before its own
// async checkExists() response ever arrived, dropping the first folder's
// transfer entirely with no error shown. Reproduced below the same way
// move_entry_test.cpp reproduces the identical Move bug: two
// enqueueFolder() calls back to back, synchronously, with no event-loop
// turn in between — exactly what the real drop-handling loop produces.
//
// This same scenario caught a SECOND, previously-unreachable bug along
// the way: FolderEnumerator numbered its listDirectoryForEnumeration()
// requests with a per-INSTANCE counter starting at 1, but connects
// directly to the shared backend's directoryEnumerated signal — two
// enumerators walking concurrently against the same source pane's
// backend (only possible once the enqueueFolder() bug above stopped
// silently dropping the second folder) could both be waiting on request
// id 1 at once, so each instance's response-filtering accepted the
// OTHER instance's response as its own, corrupting both enumerations'
// results together. Fixed by making the id counter static (shared
// across every instance) instead of per-instance — see
// FolderEnumerator.h's own comment on s_nextRequestId.
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"
#include "transfer/TransferManager.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const QString srcBase = "/tmp/folder_transfer_test/src";
    const QString dstBase = "/tmp/folder_transfer_test/dst";
    // A stale "myfolder" left over on the destination from a PREVIOUS run
    // (this test never used to clean up after itself) makes
    // TransferManager::enqueueFolder() correctly detect a real destination
    // conflict and pop a real, unanswered QMessageBox — nobody in this
    // headless test drives it, which corrupts the rest of this test's
    // timing and pass/fail results in a way that looked like a feature
    // bug but wasn't. Removing it first makes every run start from the
    // same clean slate the first-ever run gets.
    QDir(dstBase).removeRecursively();
    QDir().mkpath(srcBase + "/myfolder/subdir1");
    QDir().mkpath(srcBase + "/myfolder/subdir2/nested");
    QDir().mkpath(srcBase + "/myfolder/emptydir");
    // Two more source folders, for the concurrent-enqueueFolder() regression
    // scenario below — deliberately separate from myfolder so that
    // scenario doesn't interact with the timing/counts of this one.
    QDir().mkpath(srcBase + "/folderA");
    QDir().mkpath(srcBase + "/folderB");
    QDir().mkpath(dstBase);

    auto writeFile = [](const QString &path, const QString &content) {
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(content.toUtf8());
    };
    writeFile(srcBase + "/myfolder/a.txt", "hi from a");
    writeFile(srcBase + "/myfolder/subdir1/b.txt", "hi from b");
    writeFile(srcBase + "/myfolder/subdir1/c.txt", "hi from c");
    writeFile(srcBase + "/myfolder/subdir2/nested/d.txt", "hi from d");
    writeFile(srcBase + "/folderA/fileA.txt", "hi from A");
    writeFile(srcBase + "/folderB/fileB.txt", "hi from B");

    auto readFile = [](const QString &path) -> QString {
        QFile f(path);
        f.open(QIODevice::ReadOnly);
        return QString::fromUtf8(f.readAll());
    };

    auto *srcPane = new FilePaneWidget(new LocalBackend());
    auto *dstPane = new FilePaneWidget(new LocalBackend());
    auto *manager = new TransferManager(&app);

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    // Per-name tracking (not single scalars) — this test now drives THREE
    // separate folder transfers (myfolder, then folderA+folderB together),
    // so each signal needs to record which folder it fired for rather than
    // assuming there's only ever one in flight.
    QSet<QString> startedFolders;
    QHash<QString, int> finishedFolderFileCounts;
    int itemsAdded = 0;
    int itemsDone = 0;

    QObject::connect(manager, &TransferManager::folderTransferStarted, &app,
                      [&](const QString &name) {
        startedFolders.insert(name);
    });
    QObject::connect(manager, &TransferManager::folderTransferFailed, &app,
                      [&](const QString &name, const QString &reason) {
        qDebug() << "[test] UNEXPECTED folderTransferFailed:" << name << reason;
        allPass = false;
    });
    QObject::connect(manager, &TransferManager::folderTransferFinished, &app,
                      [&](const QString &name, int fileCount) {
        finishedFolderFileCounts.insert(name, fileCount);
    });
    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &) {
        itemsAdded++;
    });
    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.status == TransferStatus::Done)
            itemsDone++;
    });

    // Both panes need to finish their initial (home-directory) listing
    // before navigating, then actually navigate to the real test
    // directories, before enqueueFolder() can be called meaningfully.
    QTimer::singleShot(200, &app, [&]() {
        srcPane->navigateTo(srcBase);
        dstPane->navigateTo(dstBase);
    });

    QTimer::singleShot(400, &app, [&]() {
        check("source pane is actually at the test source directory",
              srcPane->currentDirectory() == srcBase);
        check("dest pane is actually at the test destination directory",
              dstPane->currentDirectory() == dstBase);

        manager->enqueueFolder(srcPane, dstPane, "myfolder");
    });

    // Enumeration + directory creation + enqueueing all happen async;
    // give it real time to walk the tree and for the (small, local, fast)
    // file transfers to actually complete.
    QTimer::singleShot(2000, &app, [&]() {
        check("folderTransferStarted fired", startedFolders.contains("myfolder"));
        check("folderTransferFinished reported exactly 4 files (a,b,c,d — not counting directories)",
              finishedFolderFileCounts.value("myfolder", -1) == 4);
        check("exactly 4 transfer queue items were actually added", itemsAdded == 4);
        check("all 4 queued items reached Done", itemsDone == 4);

        // --- Directory structure, mirrored correctly on the destination ---
        const QString dst = dstBase + "/myfolder";
        check("root folder created", QDir(dst).exists());
        check("subdir1 created", QDir(dst + "/subdir1").exists());
        check("subdir2 created", QDir(dst + "/subdir2").exists());
        check("nested (subdir2/nested) created — proves multi-level nesting, not just one level",
              QDir(dst + "/subdir2/nested").exists());
        check("emptydir created despite containing zero files anywhere in its subtree — "
              "the case most likely to be silently skipped",
              QDir(dst + "/emptydir").exists());

        // --- Files, with correct content (not just existence) ---
        check("a.txt transferred with correct content", readFile(dst + "/a.txt") == "hi from a");
        check("subdir1/b.txt transferred with correct content",
              readFile(dst + "/subdir1/b.txt") == "hi from b");
        check("subdir1/c.txt transferred with correct content",
              readFile(dst + "/subdir1/c.txt") == "hi from c");
        check("subdir2/nested/d.txt transferred with correct content (deepest nesting level)",
              readFile(dst + "/subdir2/nested/d.txt") == "hi from d");
    });

    // ---------- Concurrent enqueueFolder() regression: two folders
    // enqueued back to back, synchronously, with no event-loop turn in
    // between — the exact pattern MainWindow's drop-handling loop
    // produces for a multi-folder drag. See this file's header comment
    // for the real bug this proves fixed. ----------
    QTimer::singleShot(2200, &app, [&]() {
        manager->enqueueFolder(srcPane, dstPane, "folderA");
        manager->enqueueFolder(srcPane, dstPane, "folderB");
    });

    QTimer::singleShot(4200, &app, [&]() {
        check("concurrent enqueueFolder: folderA's folderTransferStarted fired "
              "(dropped entirely under the old bug — its checkExists() response "
              "arrived after folderB's call had already overwritten the shared "
              "pending-check state)",
              startedFolders.contains("folderA"));
        check("concurrent enqueueFolder: folderB's folderTransferStarted fired",
              startedFolders.contains("folderB"));
        check("concurrent enqueueFolder: folderA finished with its 1 file",
              finishedFolderFileCounts.value("folderA", -1) == 1);
        check("concurrent enqueueFolder: folderB finished with its 1 file",
              finishedFolderFileCounts.value("folderB", -1) == 1);
        check("concurrent enqueueFolder: folderA's file transferred with correct content",
              readFile(dstBase + "/folderA/fileA.txt") == "hi from A");
        check("concurrent enqueueFolder: folderB's file transferred with correct content",
              readFile(dstBase + "/folderB/fileB.txt") == "hi from B");

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}
