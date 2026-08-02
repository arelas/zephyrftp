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
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
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

    auto *srcPane = new FilePaneWidget(new LocalBackend());
    auto *dstPane = new FilePaneWidget(new LocalBackend());
    auto *manager = new TransferManager(&app);

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    bool sawFolderStarted = false;
    int folderFinishedFileCount = -1;
    int itemsAdded = 0;
    int itemsDone = 0;

    QObject::connect(manager, &TransferManager::folderTransferStarted, &app,
                      [&](const QString &name) {
        check("folderTransferStarted fired with the right folder name", name == "myfolder");
        sawFolderStarted = true;
    });
    QObject::connect(manager, &TransferManager::folderTransferFailed, &app,
                      [&](const QString &name, const QString &reason) {
        qDebug() << "[test] UNEXPECTED folderTransferFailed:" << name << reason;
        allPass = false;
    });
    QObject::connect(manager, &TransferManager::folderTransferFinished, &app,
                      [&](const QString &name, int fileCount) {
        check("folderTransferFinished fired with the right folder name", name == "myfolder");
        folderFinishedFileCount = fileCount;
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
        check("folderTransferStarted fired", sawFolderStarted);
        check("folderTransferFinished reported exactly 4 files (a,b,c,d — not counting directories)",
              folderFinishedFileCount == 4);
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
        auto readFile = [](const QString &path) -> QString {
            QFile f(path);
            f.open(QIODevice::ReadOnly);
            return QString::fromUtf8(f.readAll());
        };
        check("a.txt transferred with correct content", readFile(dst + "/a.txt") == "hi from a");
        check("subdir1/b.txt transferred with correct content",
              readFile(dst + "/subdir1/b.txt") == "hi from b");
        check("subdir1/c.txt transferred with correct content",
              readFile(dst + "/subdir1/c.txt") == "hi from c");
        check("subdir2/nested/d.txt transferred with correct content (deepest nesting level)",
              readFile(dst + "/subdir2/nested/d.txt") == "hi from d");

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}
