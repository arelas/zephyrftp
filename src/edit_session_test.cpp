// Tests EditSessionManager end to end: downloading a remote file to a
// local temp copy, detecting a real local save via QFileSystemWatcher,
// re-uploading it, re-editing the same file without re-downloading, and
// cleaning up (temp file deleted) on session teardown.
//
// Uses a real LocalBackend pointed at a real temp directory as the
// stand-in "remote" -- TransferManager::startEditDownload()/
// startEditUpload() call RemoteBackend::downloadFile()/uploadFile()
// exactly the same way regardless of which concrete backend is behind
// the interface (see EditSessionManager's own class doc comment on why
// it never talks to a backend directly), so this exercises the same
// dispatch code path a real SftpBackend/FtpBackend would, just via
// LocalBackend's QFile::copy() instead of network I/O -- the same
// "real, simple implementation over a mock" approach navigation-test/
// transfer-pause-test already use for their own LocalBackend-backed
// panes.
//
// Editor-launching itself (QProcess::startDetached()/
// QDesktopServices::openUrl()) isn't something a headless test can
// verify beyond "the right command was invoked" -- AppSettings' editor
// command is pointed at /bin/true here specifically so launchEditor()
// exercises its QProcess::startDetached() path without spawning
// anything that could hang or need a display, not to prove real editor
// integration. See CONTRIBUTING.md's own edit-session-test subsection
// for what's verified manually instead.
//
// Run with:
//   QT_QPA_PLATFORM=offscreen ./build/edit-session-test
#include <QApplication>
#include <QTimer>
#include <QEventLoop>
#include <QDebug>
#include <QDir>
#include <QFile>
#include "ui/FilePaneWidget.h"
#include "ui/EditSessionManager.h"
#include "AppSettings.h"
#include "backends/LocalBackend.h"
#include "backends/RemoteEntry.h"
#include "transfer/TransferManager.h"
#include "transfer/TransferItem.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const QString testDir = QStringLiteral("/tmp/edit_session_test");
    QDir().mkpath(testDir);
    // Wipe any leftovers from a previous run -- same "own its own
    // fixtures" convention transfer-queue-test/sort-and-commands-test use.
    {
        QDir d(testDir);
        for (const QString &name : d.entryList(QDir::Files))
            QFile::remove(d.filePath(name));
    }
    const QString sourceFilePath = testDir + QStringLiteral("/document.txt");
    {
        QFile f(sourceFilePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("original content");
    }

    auto *settings = new AppSettings(&app);
    // See this file's own header comment on why /bin/true specifically.
    settings->setExternalEditorCommand(QStringLiteral("/bin/true"));

    auto *pane = new FilePaneWidget(new LocalBackend());
    auto *manager = new TransferManager(&app);
    auto *sessions = new EditSessionManager(manager, settings, &app);

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    // Same polling idiom move-entry-test.cpp already establishes: react
    // to real state within a bounded timeout rather than guessing a
    // fixed delay is enough -- this path crosses real OS filesystem-watch
    // delivery and a 400ms debounce, neither of which has a single Qt
    // signal the rest of this test could just wait on directly.
    auto waitFor = [&](bool &flag, int timeoutMs = 5000) {
        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeoutTimer.start(timeoutMs);
        while (!flag && timeoutTimer.isActive())
            loop.processEvents(QEventLoop::AllEvents, 20);
        return flag;
    };

    int downloadItemCount = 0;
    QString capturedTempPath;
    bool downloadDone = false;
    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.direction != TransferDirection::EditDownload)
            return;
        if (item.status == TransferStatus::Done) {
            capturedTempPath = item.destPath;
            downloadDone = true;
        }
    });
    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        if (item.direction == TransferDirection::EditDownload)
            ++downloadItemCount;
    });

    bool uploadSucceeded = false;
    QString uploadedFileName;
    QObject::connect(sessions, &EditSessionManager::editUploadSucceeded, &app,
                      [&](const QString &fileName) {
        uploadSucceeded = true;
        uploadedFileName = fileName;
    });

    // navigateTo() dispatches asynchronously (see LocalBackend's own
    // doc comment on running listDirectory() on the GUI thread but still
    // through a queued invocation) -- waitFor() below the very next call
    // gives it a real chance to land before startEditing() reads
    // pane->currentDirectory(), same ordering transfer_pause_test.cpp's
    // own navigateTo() call sites rely on.
    bool navigated = false;
    pane->navigateTo(testDir);
    QTimer::singleShot(100, &app, [&]() { navigated = true; });
    waitFor(navigated, 2000);

    RemoteEntry entry;
    entry.name = QStringLiteral("document.txt");
    entry.isDir = false;

    // --- Phase 1: Edit downloads to a real local temp file ---
    sessions->startEditing(pane, entry);
    check("download item was created", waitFor(downloadDone) && downloadItemCount == 1);
    check("temp file path was captured", !capturedTempPath.isEmpty());
    {
        QFile f(capturedTempPath);
        check("temp file exists on disk", f.exists());
        f.open(QIODevice::ReadOnly);
        check("temp file has the source file's exact content", f.readAll() == "original content");
    }

    // --- Phase 2: simulate a save (write new content directly to the
    // temp path, same as an external editor would) -- a REAL
    // QFileSystemWatcher is watching this path, so this should trigger a
    // real fileChanged signal, the 400ms debounce, and an automatic
    // re-upload back to sourceFilePath. ---
    {
        QFile f(capturedTempPath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("edited content");
    }
    check("edited content was re-uploaded back to the server",
          waitFor(uploadSucceeded) && uploadedFileName == QStringLiteral("document.txt"));
    {
        QFile f(sourceFilePath);
        f.open(QIODevice::ReadOnly);
        check("the original file now has the edited content", f.readAll() == "edited content");
    }

    // --- Phase 3: re-editing the same file while the session is still
    // open must NOT re-download ---
    uploadSucceeded = false;
    sessions->startEditing(pane, entry);
    // No new download is expected -- give any (incorrect) second
    // download a real chance to happen before checking, rather than
    // checking instantaneously.
    {
        bool settled = false;
        QTimer::singleShot(300, &app, [&]() { settled = true; });
        waitFor(settled, 1000);
    }
    check("re-editing the same file did not trigger a second download", downloadItemCount == 1);

    // --- Phase 4: teardown deletes the temp file ---
    sessions->endSessionsForPane(pane);
    check("temp file was deleted on session teardown", !QFile::exists(capturedTempPath));

    qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
