// Exercises TransferManager's remote-to-remote staged transfer end to end
// against TWO REAL, independent SFTP servers — closing the specific gap
// ARCHITECTURE.md's Verification status entry named as "not cleanly,
// repeatably confirmed": "a full small-file transfer completing to Done
// end-to-end against two real servers in one clean run." An earlier,
// uncommitted throwaway harness got most of the way there (a real
// download half completing with real chunked progress, the phase
// transition firing correctly, a real mid-transfer cancel producing the
// right result) but never a full clean completion, due to two real bugs
// in THAT harness (not TransferManager) — both deliberately fixed here:
//
//   1. It didn't wait for BOTH panes' connections to actually complete
//      before calling enqueue() — fixed below by gating on both
//      backends' own `connected` signal first.
//   2. Both test servers are independent instances of the exact same
//      start-sftp-pubkey.sh fixture script, so they each have their OWN
//      "sample.txt" at the root — enqueueing that name would hit a real
//      destination conflict (TransferManager correctly detecting it,
//      not a bug) that the old harness didn't handle, so nothing ever
//      reached Done. Fixed below by transferring a uniquely-named
//      fixture this harness creates itself, which only exists on the
//      SOURCE server.
//   3. A third instance of the exact same category of bug, found while
//      building THIS version: re-running the harness against the same
//      two already-running servers (a completely reasonable thing to
//      do) leaves the PREVIOUS run's completed transfer sitting at the
//      destination — enqueueing the same fixture name again then hits a
//      real conflict prompt for the same correct reason as #2, which
//      this harness (a headless, non-interactive main()) has no way to
//      dismiss, so it silently hangs to the timeout instead of failing
//      clearly. Fixed by deleting any stale destination copy up front,
//      making the harness idempotent across repeated runs without
//      requiring the servers to be restarted each time.
//
// `remote-to-remote-test` (the fake-backend test) already covers the
// orchestration completely and deterministically; this covers the one
// thing it structurally can't: real bytes, over a real network, between
// two genuinely different server processes.
//
// A second scenario, after the first transfer completes cleanly: pausing
// and resuming a RemoteToRemote transfer during BOTH phases, against real
// SftpBackend instances on real worker threads — closing the gap that
// this capability (enabled after `remote-to-remote-test`'s fake-backend
// scenario E proved the orchestration correct) had only ever been
// exercised against a fake backend, never real libssh2 I/O. Triggers the
// pause the instant real progress is seen (same deterministic technique
// `verify_sftp_pause_cancel.cpp` uses for a real, single-backend
// pause/cancel) rather than guessing a wall-clock delay — reliable
// regardless of how fast the loopback connection actually is, since the
// first progress tick lands after just one ~480000-byte chunk, leaving
// most of a 10MB file's transfer still ahead of it either way.
//
// Needs TWO local-test-servers running on different ports, e.g.:
//   SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp-a SFTP_TEST_PORT=2222 \
//       tools/local-test-servers/start-sftp-pubkey.sh
//   SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp-b SFTP_TEST_PORT=2224 \
//       tools/local-test-servers/start-sftp-pubkey.sh
//   cmake --build build --target verify-remote-to-remote-live
//   QT_QPA_PLATFORM=offscreen \
//       SFTP_TEST_SCRATCH_A=/tmp/zephyrftp-local-test-servers/sftp-a SFTP_TEST_PORT_A=2222 \
//       SFTP_TEST_SCRATCH_B=/tmp/zephyrftp-local-test-servers/sftp-b SFTP_TEST_PORT_B=2224 \
//       ./build/verify-remote-to-remote-live
//
// NOT one of the self-contained EXCLUDE_FROM_ALL targets (external
// precondition: two servers, not one), so not part of that suite or CI.
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QAbstractButton>
#include <QRandomGenerator>
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

// Same reasoning as verify_sftp_move.cpp's own copy of this: reschedules
// unconditionally rather than stopping after the first accept, since
// this harness opens two independent connections (to two DIFFERENT
// hosts here, each still needing its own confirmHostKey() round trip
// through the GUI thread) and a version that stopped after one dialog
// would deadlock waiting for the second.
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

bool filesMatchByteForByte(const QString &pathA, const QString &pathB)
{
    QFile a(pathA), b(pathB);
    if (!a.open(QIODevice::ReadOnly) || !b.open(QIODevice::ReadOnly))
        return false;
    if (a.size() != b.size())
        return false;
    const qint64 chunk = 4 * 1024 * 1024;
    while (!a.atEnd()) {
        if (a.read(chunk) != b.read(chunk))
            return false;
    }
    return true;
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    const QString scratchA = qEnvironmentVariable(
        "SFTP_TEST_SCRATCH_A", "/tmp/zephyrftp-local-test-servers/sftp-a");
    const QString scratchB = qEnvironmentVariable(
        "SFTP_TEST_SCRATCH_B", "/tmp/zephyrftp-local-test-servers/sftp-b");
    const int portA = qEnvironmentVariable("SFTP_TEST_PORT_A", "2222").toInt();
    const int portB = qEnvironmentVariable("SFTP_TEST_PORT_B", "2224").toInt();
    const QString serverRootA = scratchA + "/root";
    const QString serverRootB = scratchB + "/root";
    const QString fixtureName = QStringLiteral("r2r_fixture.bin");
    const QString fixtureName2 = QStringLiteral("r2r_fixture_pause.bin");   // scenario 2's own fixture — see idempotency note below

    // A real, sizable random fixture (QRandomGenerator, not memset) —
    // written directly into server A's served directory rather than
    // uploaded, since this harness's own local user account already has
    // filesystem access to both throwaway servers' scratch dirs (same
    // technique verify_sftp_pause_cancel.cpp uses for its source file,
    // just placed directly on the "server" side here instead). Named
    // uniquely so it does NOT collide with either server's own
    // start-sftp-pubkey.sh fixture set — see this file's header comment
    // on bug #2 above.
    const qint64 fixtureSize = qEnvironmentVariable("VERIFY_R2R_FILE_SIZE_MB", "10").toLongLong()
        * 1024 * 1024;
    // See bug #3 above — a previous run's completed transfer left this
    // exact file sitting at the destination; removing it up front is
    // what makes re-running this harness against the same two
    // already-running servers safe, not just a happens-to-work-once
    // demo. QFile::remove() on a nonexistent path is a harmless no-op.
    // Same idempotency treatment for fixtureName2 (scenario 2's own
    // fixture, uniquely named so it can't collide with fixtureName's own
    // conflict-avoidance either).
    QFile::remove(serverRootB + "/" + fixtureName);
    QFile::remove(serverRootB + "/" + fixtureName2);
    auto writeRandomFixture = [&](const QString &path) {
        QFile f(path);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        QByteArray chunk(4 * 1024 * 1024, Qt::Uninitialized);
        qint64 written = 0;
        while (written < fixtureSize) {
            for (int i = 0; i < chunk.size(); i += 4)
                *reinterpret_cast<quint32 *>(chunk.data() + i) = QRandomGenerator::global()->generate();
            const qint64 toWrite = qMin<qint64>(chunk.size(), fixtureSize - written);
            f.write(chunk.constData(), toWrite);
            written += toWrite;
        }
    };
    writeRandomFixture(serverRootA + "/" + fixtureName);
    writeRandomFixture(serverRootA + "/" + fixtureName2);
    fprintf(stderr, "[setup] fixtures: %s, %s (%lld MB each) on server A\n",
            qPrintable(serverRootA + "/" + fixtureName), qPrintable(serverRootA + "/" + fixtureName2),
            fixtureSize / 1024 / 1024);

    auto *hostKeyVerifier = new HostKeyVerifier(&app);

    SftpCredentials credsA;
    credsA.host = "127.0.0.1";
    credsA.port = portA;
    credsA.username = qEnvironmentVariable("USER", "test");
    credsA.authMethod = SftpAuthMethod::PublicKey;
    credsA.privateKeyPath = scratchA + "/client_key";

    SftpCredentials credsB = credsA;
    credsB.port = portB;
    credsB.privateKeyPath = scratchB + "/client_key";

    auto *sourceBackend = new SftpBackend(credsA, hostKeyVerifier);
    auto *destBackend = new SftpBackend(credsB, hostKeyVerifier);
    auto *sourceThread = new QThread(&app);
    auto *destThread = new QThread(&app);
    sourceBackend->moveToThread(sourceThread);
    destBackend->moveToThread(destThread);

    auto *sourcePane = new FilePaneWidget(new LocalBackend());
    auto *destPane = new FilePaneWidget(new LocalBackend());
    auto *manager = new TransferManager(&app);

    QString failureReason;
    bool sourceConnected = false;
    bool destConnected = false;
    bool enqueued = false;
    bool sawDownloadProgress = false;
    bool sawUploadProgress = false;
    QString capturedTempPath;

    enum class Stage {
        Connecting, WaitAdded, WaitDone,
        // Scenario 2: pause + resume during BOTH phases, against real
        // SftpBackend I/O — see this file's header comment.
        Second_WaitAdded, Second_WaitDownloadStarted, Second_WaitPausedPhase1, Second_WaitResumedPhase1,
        Second_WaitUploadStarted, Second_WaitPausedPhase2, Second_WaitResumedPhase2, Second_WaitDone,
        Done
    };
    Stage stage = Stage::Connecting;
    int currentItemId = -1;
    qint64 pausedBytesDone1 = -1;
    qint64 pausedBytesDone2 = -1;
    QString pausedTempPath;

    auto fail = [&](const QString &reason) {
        if (failureReason.isEmpty())
            failureReason = reason;
        qApp->quit();
    };

    QObject::connect(sourceBackend, &RemoteBackend::connectionFailed, &app,
                      [&](const QString &reason) { fail("source: " + reason); });
    QObject::connect(destBackend, &RemoteBackend::connectionFailed, &app,
                      [&](const QString &reason) { fail("dest: " + reason); });

    auto maybeEnqueue = [&]() {
        if (enqueued || !sourceConnected || !destConnected)
            return;
        enqueued = true;
        stage = Stage::WaitAdded;
        manager->enqueue(sourcePane, destPane, fixtureName);
    };

    QObject::connect(sourceBackend, &RemoteBackend::connected, &app, [&]() {
        sourceConnected = true;
        maybeEnqueue();
    });
    QObject::connect(destBackend, &RemoteBackend::connected, &app, [&]() {
        destConnected = true;
        maybeEnqueue();
    });

    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        currentItemId = item.id;
        if (stage == Stage::WaitAdded) {
            check("enqueue() with both panes remote produces RemoteToRemote",
                  item.direction == TransferDirection::RemoteToRemote);
            stage = Stage::WaitDone;
        } else if (stage == Stage::Second_WaitAdded) {
            check("scenario 2: enqueue() also produces RemoteToRemote",
                  item.direction == TransferDirection::RemoteToRemote);
            stage = Stage::Second_WaitDownloadStarted;
        }
    });

    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.id != currentItemId)
            return;
        if (!item.tempFilePath.isEmpty())
            capturedTempPath = item.tempFilePath;

        const bool realProgressTick = item.status == TransferStatus::InProgress && item.bytesDone > 0;
        if (realProgressTick) {
            if (item.phase == TransferPhase::Downloading)
                sawDownloadProgress = true;
            else if (item.phase == TransferPhase::Uploading)
                sawUploadProgress = true;
        } else if (item.status == TransferStatus::Failed) {
            fail(QStringLiteral("transfer failed: %1").arg(item.errorMessage));
            return;
        }

        switch (stage) {
        case Stage::WaitDone:
            if (item.status == TransferStatus::Done) {
                stage = Stage::Second_WaitAdded;
                manager->enqueue(sourcePane, destPane, fixtureName2);
            }
            break;
        case Stage::Second_WaitDownloadStarted:
            if (realProgressTick && item.phase == TransferPhase::Downloading) {
                stage = Stage::Second_WaitPausedPhase1;   // set BEFORE pauseItem() — same discipline remote_to_remote_test.cpp's fake-backend scenario E uses
                manager->pauseItem(currentItemId);
            }
            break;
        case Stage::Second_WaitPausedPhase1:
            if (item.status == TransferStatus::Paused) {
                check("scenario 2: paused while still in phase 1 (Downloading), against a REAL server",
                      item.phase == TransferPhase::Downloading);
                check("scenario 2: paused with a real nonzero bytesDone", item.bytesDone > 0);
                check("scenario 2: the local temp file survives the pause (not cleaned up)",
                      QFile::exists(capturedTempPath));
                pausedBytesDone1 = item.bytesDone;
                pausedTempPath = capturedTempPath;
                stage = Stage::Second_WaitResumedPhase1;   // set BEFORE resumeItem() — it synchronously emits itemUpdated(Queued) before its own async re-dispatch
                manager->resumeItem(currentItemId);
            }
            break;
        case Stage::Second_WaitResumedPhase1:
            if (realProgressTick && item.phase == TransferPhase::Downloading) {
                check("scenario 2: resume continues phase 1 from the paused offset against a REAL server, not from zero",
                      item.bytesDone >= pausedBytesDone1);
                check("scenario 2: resume reuses the SAME local temp file", capturedTempPath == pausedTempPath);
                stage = Stage::Second_WaitUploadStarted;
            }
            break;
        case Stage::Second_WaitUploadStarted:
            if (realProgressTick && item.phase == TransferPhase::Uploading) {
                stage = Stage::Second_WaitPausedPhase2;
                manager->pauseItem(currentItemId);
            }
            break;
        case Stage::Second_WaitPausedPhase2:
            if (item.status == TransferStatus::Paused) {
                check("scenario 2: paused while in phase 2 (Uploading), against a REAL server",
                      item.phase == TransferPhase::Uploading);
                check("scenario 2: paused with a real nonzero bytesDone (phase 2)", item.bytesDone > 0);
                pausedBytesDone2 = item.bytesDone;
                stage = Stage::Second_WaitResumedPhase2;
                manager->resumeItem(currentItemId);
            }
            break;
        case Stage::Second_WaitResumedPhase2:
            if (realProgressTick && item.phase == TransferPhase::Uploading) {
                check("scenario 2: resume continues phase 2 from the paused offset against a REAL server, not from zero",
                      item.bytesDone >= pausedBytesDone2);
                stage = Stage::Second_WaitDone;
            }
            break;
        case Stage::Second_WaitDone:
            if (item.status == TransferStatus::Done) {
                check("scenario 2: after pausing/resuming BOTH phases against real servers, "
                      "the item still completes normally end to end", item.status == TransferStatus::Done);
                stage = Stage::Done;
                qApp->quit();
            }
            break;
        default:
            break;
        }
    });

    sourcePane->setBackend(sourceBackend, sourceThread);
    destPane->setBackend(destBackend, destThread);
    autoAcceptHostKeyPrompt();

    // Longer than scenario 1 alone needs — now covers a second full
    // transfer plus two real pause/resume round trips against live
    // servers.
    QTimer::singleShot(90000, &app, [&]() {
        if (stage == Stage::Done)
            return;
        fail(QStringLiteral("timed out after 90s in stage %1").arg(static_cast<int>(stage)));
    });

    app.exec();

    if (!failureReason.isEmpty()) {
        fprintf(stderr, "failure reason: %s\n", qPrintable(failureReason));
        allPass = false;
    }
    check("reached Done", stage == Stage::Done);
    check("saw real download-phase progress (source -> local temp)", sawDownloadProgress);
    check("saw real upload-phase progress (local temp -> destination)", sawUploadProgress);
    check("the local staging temp file was cleaned up after completion",
          !capturedTempPath.isEmpty() && !QFile::exists(capturedTempPath));
    check("the fixture landed on server B's real disk", QFile::exists(serverRootB + "/" + fixtureName));
    check("server B's copy matches server A's original byte-for-byte",
          filesMatchByteForByte(serverRootA + "/" + fixtureName, serverRootB + "/" + fixtureName));
    check("scenario 2: its fixture also landed on server B's real disk",
          QFile::exists(serverRootB + "/" + fixtureName2));
    check("scenario 2: content survived two real pause/resume cycles byte-for-byte",
          filesMatchByteForByte(serverRootA + "/" + fixtureName2, serverRootB + "/" + fixtureName2));

    sourcePane->setBackend(new LocalBackend(), nullptr);
    destPane->setBackend(new LocalBackend(), nullptr);

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
