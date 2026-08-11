// Exercises TransferManager::moveEntry()/moveFolder() against a REAL FTP
// server — the FTP counterpart to verify_sftp_move.cpp, confirming the
// one thing that harness (and move-entry-test's fake backends) can't:
// that FTP's RNFR/RNTO pair genuinely relocates a DIRECTORY server-side,
// not just a file, over the FTP protocol specifically. ARCHITECTURE.md's
// Verification status entry for Move names this exact gap: "FTP's
// RNFR/RNTO directory-rename equivalent — verify-sftp-move only exercises
// SFTP, and this project's FTP live-server harnesses don't yet drive
// TransferManager/moveEntry() the way this one does."
//
// Two independent FtpBackend connections to the SAME server (same host/
// port/username, hence the same real connectionIdentity()), one pane
// staying at the server's root, the other navigated into its "uploads"
// subdirectory — a real cross-directory move, same shape as
// verify_sftp_move.cpp. Unlike SftpBackend, FtpBackend is fully async/
// event-driven (QTcpSocket/QSslSocket) with no blocking calls, so — same
// choice verify_ftp_live.cpp already made — no worker QThread is needed
// here; TransferManager's own QMetaObject::invokeMethod(...,
// Qt::QueuedConnection) dispatch works correctly either way. No host-key-
// style trust prompt either: plain FTP authenticates the server not at
// all, so there's nothing to confirm/accept the way SFTP's host key or
// FTPS's certificate would need.
//
// The server's own start-ftp.sh fixture only ships sample.txt +
// uploads/ (no nested folder to move) — this harness creates its own
// "movetest" folder with nested content directly on the server's real
// filesystem before connecting, same idempotent-fixture-reset approach
// verify_sftp_move.cpp uses for sample.txt, so repeated runs against an
// already-running server don't hit a stale-destination conflict this
// headless main() has no way to dismiss.
//
// Needs tools/local-test-servers/start-ftp.sh already running; NOT one
// of the self-contained EXCLUDE_FROM_ALL targets (external
// precondition), so not part of that suite or CI.
//
// Run with:
//   tools/local-test-servers/start-ftp.sh
//   cmake --build build --target verify-ftp-move
//   QT_QPA_PLATFORM=offscreen FTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/ftp \
//       ./build/verify-ftp-move
#include <QApplication>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstdio>
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"
#include "backends/FtpBackend.h"
#include "backends/FtpCredentials.h"
#include "transfer/TransferManager.h"

namespace {
bool allPass = true;

void check(const char *label, bool condition)
{
    fprintf(stderr, "[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) allPass = false;
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    const QString scratch = qEnvironmentVariable(
        "FTP_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/ftp");
    const int port = qEnvironmentVariable("FTP_TEST_PORT", "2121").toInt();
    const QString serverRoot = scratch + "/root";

    // Idempotent-fixture reset — same reasoning and the same real bug
    // class verify_sftp_move.cpp's own header comment documents: this
    // harness MOVES fixtures out of the root into uploads/ as part of
    // what it's testing, so re-running it against an already-running
    // server (a completely reasonable thing to do) would otherwise find
    // them already moved, with a stale copy already sitting at the
    // destination — a real conflict this headless harness has no way to
    // dismiss. Restoring both fixtures up front makes this safe to
    // re-run without restarting the server each time.
    QFile::remove(serverRoot + "/uploads/sample.txt");
    QDir(serverRoot + "/uploads/movetest").removeRecursively();
    if (!QFile::exists(serverRoot + "/sample.txt")) {
        QFile f(serverRoot + "/sample.txt");
        f.open(QIODevice::WriteOnly);
        f.write("hello from the local ftp test server\n");
    }
    if (!QDir(serverRoot + "/movetest").exists()) {
        QDir().mkpath(serverRoot + "/movetest/subdir");
        QDir().mkpath(serverRoot + "/movetest/emptydir");
        auto writeFixtureFile = [](const QString &path, const char *content) {
            QFile f(path);
            f.open(QIODevice::WriteOnly);
            f.write(content);
        };
        writeFixtureFile(serverRoot + "/movetest/a.txt", "hi from a\n");
        writeFixtureFile(serverRoot + "/movetest/subdir/b.txt", "hi from b\n");
    }

    FtpCredentials creds;
    creds.host = "127.0.0.1";
    creds.port = port;
    creds.username = QStringLiteral("ftpuser");
    creds.password = QStringLiteral("ftppass");
    creds.ftpsMode = FtpsMode::None;

    // Two independent connections to the SAME server — real cross-pane
    // eligibility discovered at runtime via connectionIdentity(), not
    // simulated. No QThread — see this file's header comment on why
    // FtpBackend doesn't need one the way SftpBackend does.
    auto *sourceBackend = new FtpBackend(creds);
    auto *destBackend = new FtpBackend(creds);

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

    // Same sequencing discipline as verify_sftp_move.cpp: gate on each
    // pane's own real directoryListed count, not a guessed delay.
    auto maybeAdvance = [&]() {
        if (stage != Stage::Connecting)
            return;
        if (sourceListedCount < 1 || destListedCount < 1)
            return;
        if (!navigatedDestToUploads) {
            navigatedDestToUploads = true;
            // Deferred, not called synchronously from inside this SAME
            // directoryListed emission — a real bug found by running this
            // for real: this lambda is connected directly to destBackend's
            // directoryListed BEFORE destPane->setBackend() runs (further
            // down in main()), so it fires FIRST, ahead of
            // FilePaneWidget::onDirectoryListed() (connected second) —
            // which is the only thing that clears m_navigationInFlight
            // after the initial connect-triggered navigation this same
            // signal is delivering. Calling navigateTo() synchronously
            // here hits that still-true guard and silently no-ops,
            // leaving nothing to ever produce a second directoryListed —
            // observed as a full hang, not a fast, diagnosable failure.
            // Deferring to the next event-loop turn lets
            // onDirectoryListed() run first, exactly matching how a real,
            // separate user action (not nested inside signal delivery)
            // would naturally call navigateTo().
            QTimer::singleShot(0, [destPane]() {
                destPane->navigateTo(destPane->currentDirectory() + "/uploads");
            });
            return;
        }
        if (destListedCount < 2)
            return;

        check("moveEligible(): true for two real FtpBackend connections to the SAME server",
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
                check("single-file move: real content preserved (not truncated/corrupted by RNFR/RNTO)",
                      moved.readAll() == "hello from the local ftp test server\n");
            }

            stage = Stage::WaitFolderMoveAdded;
            manager->moveFolder(sourcePane, destPane, QStringLiteral("movetest"));
            break;
        }
        case Stage::WaitFolderMoveDone: {
            check("folder move: old path is gone from the server's real disk",
                  !QDir(serverRoot + "/movetest").exists());
            check("folder move: new path exists on the server's real disk — "
                  "the specific claim ARCHITECTURE.md flagged as unconfirmed for FTP",
                  QDir(serverRoot + "/uploads/movetest").exists());
            check("folder move: nested file content survived the rename",
                  QFile::exists(serverRoot + "/uploads/movetest/a.txt")
                      && QFile::exists(serverRoot + "/uploads/movetest/subdir/b.txt"));
            check("folder move: an empty nested subdirectory survived the rename too",
                  QDir(serverRoot + "/uploads/movetest/emptydir").exists());

            stage = Stage::Done;
            qApp->quit();
            break;
        }
        default:
            break;
        }
    });

    sourcePane->setBackend(sourceBackend, nullptr);
    destPane->setBackend(destBackend, nullptr);

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
