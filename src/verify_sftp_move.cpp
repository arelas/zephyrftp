// Exercises TransferManager::moveEntry()/moveFolder() against a REAL
// server — a real cross-pane server-side rename, both a single file and a
// whole folder, confirming the one thing move-entry-test's fake backends
// structurally can't: that libssh2_sftp_rename() genuinely relocates a
// DIRECTORY server-side, not just a file. ARCHITECTURE.md's Verification
// status entry for Move names this exact gap: "whether
// libssh2_sftp_rename()... genuinely relocate a directory (not just a
// file) server-side — expected... but unconfirmed against a real server
// the way the fake-backend test's directory case necessarily can't be."
//
// Two independent SftpBackend connections to the SAME server (same host/
// port/username, hence the same real connectionIdentity() —
// TransferManager::moveEligible() isn't told this is eligible, it
// discovers it), one pane staying at the server's root, the other
// navigated into its "uploads" subdirectory — a real cross-directory
// move, not a same-directory no-op. Needs
// tools/local-test-servers/start-sftp-pubkey.sh already running; NOT one
// of the self-contained EXCLUDE_FROM_ALL targets (external
// precondition), so not part of that suite or CI.
//
// NOT covered here (a deliberate scope cut, not an oversight): the
// "Write Into an existing folder fails" conflict path. Exercising it
// would mean driving a live askConflict() QMessageBox — the same
// category of manual-only gap conflict-resolution-test already accepts
// for the ordinary transfer path, and that dialog wiring is pure Qt/UI
// code with zero backend dependency, already exercised there with fakes.
// This harness's whole point is the backend-level rename call, which the
// conflict path doesn't change.
//
// Sequencing lesson applied directly from ARCHITECTURE.md's own account
// of an earlier (uncommitted) throwaway TransferManager/TransferQueueWidget
// screenshot harness: "FilePaneWidget::setBackend() fires its own
// automatic connectToHost() and an initial home-directory listing, which
// raced ahead of the harness's own sequencing" — gated here on each
// pane's own directoryListed count, not a guessed delay.
//
// Run with:
//   tools/local-test-servers/start-sftp-pubkey.sh
//   cmake --build build --target verify-sftp-move
//   QT_QPA_PLATFORM=offscreen SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp \
//       ./build/verify-sftp-move
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QAbstractButton>
#include <cstdio>
#include "ui/FilePaneWidget.h"
#include "ui/HostKeyVerifier.h"
#include "backends/LocalBackend.h"
#include "backends/SftpBackend.h"
#include "backends/SftpCredentials.h"
#include "transfer/TransferManager.h"

namespace {
bool allPass = true;

void check(const char *label, bool condition)
{
    fprintf(stderr, "[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) allPass = false;
}

// Unlike verify_sftp_pause_cancel.cpp/verify_sftp_pubkey.cpp (which each
// open exactly one connection, so accepting one dialog is the whole
// job), this harness opens TWO independent connections to the same
// host. Each SftpBackend's own worker thread blocks on its own
// confirmHostKey() call (Qt::BlockingQueuedConnection) — Qt's GUI-thread
// event loop processes them one at a time, so a second modal dialog can
// genuinely appear right after the first is dismissed, even though both
// connections target the identical host/fingerprint. Rescheduling
// unconditionally (not stopping after the first accept) is what makes
// this robust to that — a version that stopped after one dialog would
// silently deadlock waiting for a second one that never gets dismissed.
void autoAcceptHostKeyPrompt()
{
    QTimer::singleShot(50, qApp, [] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (box) {
            for (QAbstractButton *button : box->buttons()) {
                if (box->buttonRole(button) == QMessageBox::YesRole) {
                    button->click();
                    break;
                }
            }
        }
        QTimer::singleShot(50, qApp, &autoAcceptHostKeyPrompt);
    });
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Same reasoning as verify_sftp_pause_cancel.cpp: no QMainWindow is
    // ever shown, so the host-key dialogs are the only top-level windows
    // — Qt's default quitOnLastWindowClosed would auto-quit the instant
    // the first one closes.
    app.setQuitOnLastWindowClosed(false);

    const QString scratch = qEnvironmentVariable(
        "SFTP_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/sftp");
    const QString serverRoot = scratch + "/root";

    // Same category of bug found and fixed in
    // verify_remote_to_remote_live.cpp (see bug #3 there): this harness
    // MOVES start-sftp-pubkey.sh's own fixtures (sample.txt, testfolder)
    // out of the root into uploads/ as part of what it's testing, so
    // re-running it against the same already-running server (a
    // completely reasonable thing to do) would find them already gone
    // from the root and already sitting at the destination — a real
    // conflict this headless harness has no way to dismiss, hanging to
    // the timeout instead of failing clearly. Restoring both fixtures to
    // their pristine start-sftp-pubkey.sh-created state up front (removing
    // any leftover copy at the destination, recreating the source if a
    // previous run already moved it away) makes this idempotent across
    // repeated runs without requiring the server to be restarted each time.
    QFile::remove(serverRoot + "/uploads/sample.txt");
    QDir(serverRoot + "/uploads/testfolder").removeRecursively();
    if (!QFile::exists(serverRoot + "/sample.txt")) {
        QFile f(serverRoot + "/sample.txt");
        f.open(QIODevice::WriteOnly);
        f.write("hello from the local sftp test server\n");
    }
    if (!QDir(serverRoot + "/testfolder").exists()) {
        QDir().mkpath(serverRoot + "/testfolder/subdir1");
        QDir().mkpath(serverRoot + "/testfolder/subdir2/nested");
        QDir().mkpath(serverRoot + "/testfolder/emptydir");
        auto writeFixtureFile = [](const QString &path, const char *content) {
            QFile f(path);
            f.open(QIODevice::WriteOnly);
            f.write(content);
        };
        writeFixtureFile(serverRoot + "/testfolder/a.txt", "hi from a\n");
        writeFixtureFile(serverRoot + "/testfolder/subdir1/b.txt", "hi from b\n");
        writeFixtureFile(serverRoot + "/testfolder/subdir1/c.txt", "hi from c\n");
        writeFixtureFile(serverRoot + "/testfolder/subdir2/nested/d.txt", "hi from d\n");
    }

    SftpCredentials creds;
    creds.host = "127.0.0.1";
    creds.port = 2222;
    creds.username = qEnvironmentVariable("USER", "test");
    creds.authMethod = SftpAuthMethod::PublicKey;
    creds.privateKeyPath = scratch + "/client_key";

    auto *hostKeyVerifier = new HostKeyVerifier(&app);

    // Two independent connections to the SAME server — the real-world
    // shape of "both panes happen to be logged into the same box",
    // deliberately not simulated via two fakes hardcoded to report a
    // matching string the way move-entry-test does.
    auto *sourceBackend = new SftpBackend(creds, hostKeyVerifier);
    auto *destBackend = new SftpBackend(creds, hostKeyVerifier);
    auto *sourceThread = new QThread(&app);
    auto *destThread = new QThread(&app);
    sourceBackend->moveToThread(sourceThread);
    destBackend->moveToThread(destThread);

    auto *sourcePane = new FilePaneWidget(new LocalBackend());
    auto *destPane = new FilePaneWidget(new LocalBackend());
    auto *manager = new TransferManager(&app);

    QString failureReason;
    int sourceListedCount = 0;
    int destListedCount = 0;
    bool navigatedDestToUploads = false;

    enum class Stage {
        Connecting, WaitFileMoveAdded, WaitFileMoveDone,
        WaitFolderMoveAdded, WaitFolderMoveDone, Done
    };
    Stage stage = Stage::Connecting;
    int currentItemId = -1;

    auto fail = [&](const QString &reason) {
        if (failureReason.isEmpty())
            failureReason = reason;
        qApp->quit();
    };

    QObject::connect(sourceBackend, &RemoteBackend::connectionFailed, &app,
                      [&](const QString &reason) { fail("source: " + reason); });
    QObject::connect(destBackend, &RemoteBackend::connectionFailed, &app,
                      [&](const QString &reason) { fail("dest: " + reason); });

    // Once BOTH panes have completed their real initial directory
    // listing (proof connectToHost()+the automatic first listDirectory()
    // both finished, not just that the connected signal fired), navigate
    // the destination pane into "uploads" — a real, different directory
    // on the SAME server, so the move below is a genuine cross-directory
    // relocation, not a same-path no-op.
    auto maybeAdvance = [&]() {
        if (stage != Stage::Connecting)
            return;
        if (sourceListedCount < 1 || destListedCount < 1)
            return;
        if (!navigatedDestToUploads) {
            navigatedDestToUploads = true;
            destPane->navigateTo(destPane->currentDirectory() + "/uploads");
            return;
        }
        if (destListedCount < 2)
            return;   // still waiting for the uploads listing specifically

        check("moveEligible(): true for two real SftpBackend connections to the SAME server",
              TransferManager::moveEligible(sourcePane, destPane));

        stage = Stage::WaitFileMoveAdded;
        manager->moveEntry(sourcePane, destPane, QStringLiteral("sample.txt"));
    };

    QObject::connect(sourceBackend, &RemoteBackend::directoryListed, &app,
                      [&](const QString &, const QList<RemoteEntry> &) {
        ++sourceListedCount;
        maybeAdvance();
    });
    QObject::connect(destBackend, &RemoteBackend::directoryListed, &app,
                      [&](const QString &, const QList<RemoteEntry> &) {
        ++destListedCount;
        maybeAdvance();
    });

    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        currentItemId = item.id;
        if (stage == Stage::WaitFileMoveAdded) {
            check("single-file move: TransferItem direction is Move", item.direction == TransferDirection::Move);
            stage = Stage::WaitFileMoveDone;
        } else if (stage == Stage::WaitFolderMoveAdded) {
            check("folder move: TransferItem direction is Move", item.direction == TransferDirection::Move);
            stage = Stage::WaitFolderMoveDone;
        }
    });

    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.id != currentItemId)
            return;
        if (item.status == TransferStatus::Failed) {
            fail(QStringLiteral("move failed: %1").arg(item.errorMessage));
            return;
        }
        if (item.status != TransferStatus::Done)
            return;

        switch (stage) {
        case Stage::WaitFileMoveDone: {
            check("single-file move: old path is gone from the server's real disk",
                  !QFile::exists(serverRoot + "/sample.txt"));
            QFile moved(serverRoot + "/uploads/sample.txt");
            const bool exists = moved.exists();
            check("single-file move: new path exists on the server's real disk", exists);
            if (exists && moved.open(QIODevice::ReadOnly)) {
                check("single-file move: real content preserved (not truncated/corrupted by the rename)",
                      moved.readAll() == "hello from the local sftp test server\n");
            }

            stage = Stage::WaitFolderMoveAdded;
            manager->moveFolder(sourcePane, destPane, QStringLiteral("testfolder"));
            break;
        }
        case Stage::WaitFolderMoveDone: {
            check("folder move: old path is gone from the server's real disk",
                  !QDir(serverRoot + "/testfolder").exists());
            check("folder move: new path exists on the server's real disk — "
                  "the specific claim ARCHITECTURE.md flagged as unconfirmed against a real server",
                  QDir(serverRoot + "/uploads/testfolder").exists());
            check("folder move: nested file content survived the rename",
                  QFile::exists(serverRoot + "/uploads/testfolder/a.txt")
                      && QFile::exists(serverRoot + "/uploads/testfolder/subdir1/b.txt")
                      && QFile::exists(serverRoot + "/uploads/testfolder/subdir1/c.txt")
                      && QFile::exists(serverRoot + "/uploads/testfolder/subdir2/nested/d.txt"));
            check("folder move: an empty nested subdirectory survived the rename too",
                  QDir(serverRoot + "/uploads/testfolder/emptydir").exists());

            stage = Stage::Done;
            qApp->quit();
            break;
        }
        default:
            break;
        }
    });

    sourcePane->setBackend(sourceBackend, sourceThread);
    destPane->setBackend(destBackend, destThread);
    autoAcceptHostKeyPrompt();

    QTimer::singleShot(30000, &app, [&]() {
        if (stage == Stage::Done)
            return;
        fail(QStringLiteral("timed out after 30s in stage %1").arg(static_cast<int>(stage)));
    });

    app.exec();

    if (!failureReason.isEmpty()) {
        fprintf(stderr, "failure reason: %s\n", qPrintable(failureReason));
        allPass = false;
    }
    check("reached Done", stage == Stage::Done);

    sourcePane->setBackend(new LocalBackend(), nullptr);
    destPane->setBackend(new LocalBackend(), nullptr);

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
