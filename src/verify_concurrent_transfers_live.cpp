// Exercises TransferManager's new per-backend-instance concurrent
// scheduling (see ARCHITECTURE.md's `TransferManager` entry) against TWO
// REAL, independent SFTP servers — closing the one gap
// transfer-concurrency-test structurally can't: that gap's own header
// comment says outright it "proves TransferManager's SCHEDULING allows
// concurrent InProgress items across distinct backend instances — it
// does NOT exercise real parallel network I/O (that would need two live
// SFTP/FTP servers, unavailable [there])." This is that missing piece —
// two real SftpBackend instances, each on its own worker QThread with
// its own live libssh2 session/TCP connection to a genuinely separate
// sshd process, uploading real files at the same time.
//
// Same two-independent-server setup verify_remote_to_remote_live.cpp
// already established (two instances of the same start-sftp-pubkey.sh
// fixture script on different ports) — reused here rather than
// reinvented, including its connect-both-then-proceed gating and
// autoAcceptHostKeyPrompt() (rescheduling unconditionally, since this
// harness also opens two independent connections needing their own
// host-key confirmation each).
//
// Three phases:
//   1. SERIAL BASELINE — upload fixtureA to server A alone, wait for
//      Done, record elapsed time; then upload fixtureB to server B
//      alone (only after A finishes), record elapsed time. This is the
//      real, measured per-file cost each transfer actually has on this
//      machine's real loopback link — not an assumption.
//   2. CONCURRENT BATCH — enqueue THREE items back to back: fixtureA2 ->
//      server A, fixtureB2 -> server B (a DIFFERENT backend instance —
//      should run genuinely at the same time as the first), and
//      fixtureA3 -> server A AGAIN (the SAME backend instance as the
//      first — must still serialize strictly behind it; two transfers
//      can never safely share one libssh2 session at once). Total wall-
//      clock time for the whole batch is measured and compared against
//      the serial-baseline-derived expectation
//      (T_A + T_B + T_A, since the batch is the same-sized work as two
//      A-sized uploads and one B-sized upload) — real concurrency should
//      make the batch finish well under that serial sum, not just
//      "eventually, all three succeeded."
//   3. Content verification — all three concurrently-uploaded files are
//      confirmed byte-for-byte identical to their local originals,
//      checking for any cross-wire corruption between the two backends'
//      worker threads that a purely timing-based check wouldn't catch.
//
// Needs TWO local-test-servers running on different ports, exactly like
// verify-remote-to-remote-live:
//   SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp-a SFTP_TEST_PORT=2222 \
//       tools/local-test-servers/start-sftp-pubkey.sh
//   SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp-b SFTP_TEST_PORT=2224 \
//       tools/local-test-servers/start-sftp-pubkey.sh
//   cmake --build build --target verify-concurrent-transfers-live
//   QT_QPA_PLATFORM=offscreen \
//       SFTP_TEST_SCRATCH_A=/tmp/zephyrftp-local-test-servers/sftp-a SFTP_TEST_PORT_A=2222 \
//       SFTP_TEST_SCRATCH_B=/tmp/zephyrftp-local-test-servers/sftp-b SFTP_TEST_PORT_B=2224 \
//       ./build/verify-concurrent-transfers-live
//
// NOT one of the self-contained EXCLUDE_FROM_ALL targets (external
// precondition: two servers, not one), so not part of that suite or CI —
// same treatment as verify-remote-to-remote-live.
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
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

// Identical technique to verify_remote_to_remote_live.cpp's own copy —
// reschedules unconditionally since two independent connections each
// need their own host-key confirmation.
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

void writeRandomFixture(const QString &path, qint64 sizeBytes)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    QByteArray chunk(4 * 1024 * 1024, Qt::Uninitialized);
    qint64 written = 0;
    while (written < sizeBytes) {
        for (int i = 0; i < chunk.size(); i += 4)
            *reinterpret_cast<quint32 *>(chunk.data() + i) = QRandomGenerator::global()->generate();
        const qint64 toWrite = qMin<qint64>(chunk.size(), sizeBytes - written);
        f.write(chunk.constData(), toWrite);
        written += toWrite;
    }
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

    // A sizable fixture — big enough on a real loopback link (~22-25MB/s,
    // per verify_sftp_throughput.cpp's own measurement) to take a few
    // real seconds, leaving a genuine window to observe two transfers
    // both mid-flight at once, not so big the whole harness is slow.
    const qint64 fixtureSize = qEnvironmentVariable("VERIFY_CONCURRENCY_FILE_SIZE_MB", "60").toLongLong()
        * 1024 * 1024;

    const QString localSrcDir = QDir::tempPath() + "/zephyrftp_concurrency_verify_src";
    QDir(localSrcDir).removeRecursively();
    QDir().mkpath(localSrcDir);

    const QString fixtureA = "concurrency_baseline_a.bin";
    const QString fixtureB = "concurrency_baseline_b.bin";
    const QString fixtureA2 = "concurrency_batch_a1.bin";
    const QString fixtureB2 = "concurrency_batch_b1.bin";
    const QString fixtureA3 = "concurrency_batch_a2.bin";

    fprintf(stderr, "[setup] generating 5 local fixtures (%lld MB each)...\n",
            fixtureSize / 1024 / 1024);
    for (const QString &name : {fixtureA, fixtureB, fixtureA2, fixtureB2, fixtureA3})
        writeRandomFixture(localSrcDir + "/" + name, fixtureSize);

    // Idempotency across repeated runs against the same already-running
    // servers — same reasoning verify_remote_to_remote_live.cpp's own
    // header comment documents for its bug #3.
    for (const QString &name : {fixtureA, fixtureA2, fixtureA3})
        QFile::remove(serverRootA + "/" + name);
    for (const QString &name : {fixtureB, fixtureB2})
        QFile::remove(serverRootB + "/" + name);

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

    auto *backendA = new SftpBackend(credsA, hostKeyVerifier);
    auto *backendB = new SftpBackend(credsB, hostKeyVerifier);
    auto *threadA = new QThread(&app);
    auto *threadB = new QThread(&app);
    backendA->moveToThread(threadA);
    backendB->moveToThread(threadB);

    auto *localPane = new FilePaneWidget(new LocalBackend());
    localPane->navigateTo(localSrcDir);
    auto *paneA = new FilePaneWidget(new LocalBackend());
    auto *paneB = new FilePaneWidget(new LocalBackend());
    auto *manager = new TransferManager(&app);

    QString failureReason;
    bool connectedA = false, connectedB = false;

    enum class Stage {
        Connecting,
        Baseline_WaitA, Baseline_WaitB,
        Concurrent_WaitAllAdded, Concurrent_Running,
        Done
    };
    Stage stage = Stage::Connecting;

    auto fail = [&](const QString &reason) {
        if (failureReason.isEmpty())
            failureReason = reason;
        qApp->quit();
    };

    QObject::connect(backendA, &RemoteBackend::connectionFailed, &app,
                      [&](const QString &reason) { fail("server A: " + reason); });
    QObject::connect(backendB, &RemoteBackend::connectionFailed, &app,
                      [&](const QString &reason) { fail("server B: " + reason); });

    QElapsedTimer baselineATimer, baselineBTimer, batchTimer;
    qint64 baselineAElapsedMs = 0, baselineBElapsedMs = 0, batchElapsedMs = 0;
    int baselineAItemId = -1, baselineBItemId = -1;

    auto maybeStartBaseline = [&]() {
        if (!connectedA || !connectedB || stage != Stage::Connecting)
            return;
        stage = Stage::Baseline_WaitA;
        baselineATimer.start();
        manager->enqueue(localPane, paneA, fixtureA);
    };

    QObject::connect(backendA, &RemoteBackend::connected, &app, [&]() {
        connectedA = true;
        maybeStartBaseline();
    });
    QObject::connect(backendB, &RemoteBackend::connected, &app, [&]() {
        connectedB = true;
        maybeStartBaseline();
    });

    // Concurrent-batch bookkeeping: three item ids resolved via
    // itemAdded (matched by filename, same technique
    // transfer_concurrency_test.cpp's fake-backend version already
    // uses), then their live status tracked via itemUpdated.
    int a1Id = -1, b1Id = -1, a2Id = -1;
    TransferStatus a1Status = TransferStatus::Queued;
    TransferStatus b1Status = TransferStatus::Queued;
    TransferStatus a2Status = TransferStatus::Queued;
    qint64 a1BytesDone = 0, b1BytesDone = 0;
    bool sawA1AndB1BothInProgressAtOnce = false;
    bool sawA2StayedQueuedWhileA1Ran = false;
    bool a2StartedBeforeA1Finished = false;   // a real violation if it ever becomes true
    int itemsDoneInBatch = 0;

    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        if (stage == Stage::Baseline_WaitA && item.fileName == fixtureA) {
            baselineAItemId = item.id;
            check("baseline: enqueue() with a local source + remote dest produces LocalToRemote",
                  item.direction == TransferDirection::LocalToRemote);
        } else if (stage == Stage::Baseline_WaitB && item.fileName == fixtureB) {
            baselineBItemId = item.id;
        } else if (stage == Stage::Concurrent_WaitAllAdded || stage == Stage::Concurrent_Running) {
            if (item.fileName == fixtureA2) a1Id = item.id;
            else if (item.fileName == fixtureB2) b1Id = item.id;
            else if (item.fileName == fixtureA3) a2Id = item.id;
        }
    });

    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (stage == Stage::Baseline_WaitA && item.id == baselineAItemId) {
            if (item.status == TransferStatus::Done) {
                baselineAElapsedMs = baselineATimer.elapsed();
                fprintf(stderr, "[baseline] server A alone: %lld ms\n",
                        static_cast<long long>(baselineAElapsedMs));
                stage = Stage::Baseline_WaitB;
                baselineBTimer.start();
                manager->enqueue(localPane, paneB, fixtureB);
            } else if (item.status == TransferStatus::Failed) {
                fail("baseline A failed: " + item.errorMessage);
            }
            return;
        }
        if (stage == Stage::Baseline_WaitB && item.id == baselineBItemId) {
            if (item.status == TransferStatus::Done) {
                baselineBElapsedMs = baselineBTimer.elapsed();
                fprintf(stderr, "[baseline] server B alone: %lld ms\n",
                        static_cast<long long>(baselineBElapsedMs));
                // Start the concurrent batch: all three enqueue() calls
                // back to back, synchronously, nothing else running —
                // mirrors how a real drag-drop on both panes at once
                // would arrive at TransferManager.
                stage = Stage::Concurrent_WaitAllAdded;
                batchTimer.start();
                manager->enqueue(localPane, paneA, fixtureA2);
                manager->enqueue(localPane, paneB, fixtureB2);
                manager->enqueue(localPane, paneA, fixtureA3);
                stage = Stage::Concurrent_Running;
            } else if (item.status == TransferStatus::Failed) {
                fail("baseline B failed: " + item.errorMessage);
            }
            return;
        }

        if (stage != Stage::Concurrent_Running)
            return;

        if (item.id == a1Id) {
            a1Status = item.status;
            a1BytesDone = item.bytesDone;
            if (item.status == TransferStatus::Failed) fail("concurrent A1 failed: " + item.errorMessage);
            if (item.status == TransferStatus::Done) ++itemsDoneInBatch;
        } else if (item.id == b1Id) {
            b1Status = item.status;
            b1BytesDone = item.bytesDone;
            if (item.status == TransferStatus::Failed) fail("concurrent B1 failed: " + item.errorMessage);
            if (item.status == TransferStatus::Done) ++itemsDoneInBatch;
        } else if (item.id == a2Id) {
            a2Status = item.status;
            if (item.status == TransferStatus::InProgress && a1Status != TransferStatus::Done)
                a2StartedBeforeA1Finished = true;   // real violation — see the check() below
            if (item.status == TransferStatus::Failed) fail("concurrent A2 failed: " + item.errorMessage);
            if (item.status == TransferStatus::Done) ++itemsDoneInBatch;
        } else {
            return;
        }

        // Checked on every relevant update, not just once — the overlap
        // window needs to be caught whenever it happens to land, and
        // itemUpdated fires often enough (progress ticks) that polling
        // this way reliably catches it.
        if (!sawA1AndB1BothInProgressAtOnce
            && a1Status == TransferStatus::InProgress && a1BytesDone > 0
            && b1Status == TransferStatus::InProgress && b1BytesDone > 0) {
            sawA1AndB1BothInProgressAtOnce = true;
        }
        if (a1Status == TransferStatus::InProgress && a2Status == TransferStatus::Queued)
            sawA2StayedQueuedWhileA1Ran = true;

        if (itemsDoneInBatch == 3) {
            batchElapsedMs = batchTimer.elapsed();
            fprintf(stderr, "[concurrent] batch of 3 (2x server-A + 1x server-B): %lld ms\n",
                    static_cast<long long>(batchElapsedMs));
            stage = Stage::Done;
            qApp->quit();
        }
    });

    paneA->setBackend(backendA, threadA);
    paneB->setBackend(backendB, threadB);
    autoAcceptHostKeyPrompt();

    QTimer::singleShot(120000, &app, [&]() {
        if (stage == Stage::Done)
            return;
        fail(QStringLiteral("timed out after 120s in stage %1").arg(static_cast<int>(stage)));
    });

    app.exec();

    if (!failureReason.isEmpty()) {
        fprintf(stderr, "failure reason: %s\n", qPrintable(failureReason));
        allPass = false;
    }
    check("reached Done", stage == Stage::Done);
    check("saw item A1 (server A) and item B1 (server B, a DIFFERENT backend instance) "
          "both genuinely InProgress with real progress at the same time",
          sawA1AndB1BothInProgressAtOnce);
    check("saw item A2 (also server A) stay Queued while item A1 (SAME backend instance) was InProgress",
          sawA2StayedQueuedWhileA1Ran);
    check("item A2 never became InProgress before item A1 (same backend) actually finished — "
          "real serialization on one real libssh2 session held, not just on fake backends",
          !a2StartedBeforeA1Finished);

    if (stage == Stage::Done && baselineAElapsedMs > 0 && baselineBElapsedMs > 0) {
        const qint64 serialEstimateMs = baselineAElapsedMs + baselineBElapsedMs + baselineAElapsedMs;
        const double ratio = serialEstimateMs > 0
            ? static_cast<double>(batchElapsedMs) / static_cast<double>(serialEstimateMs) : 1.0;
        fprintf(stderr, "\nserial-estimate for the same batch (T_A + T_B + T_A): %lld ms\n"
                        "actual concurrent batch time:                          %lld ms\n"
                        "ratio (concurrent / serial-estimate):                  %.2f "
                        "(1.0 = no speedup, <1.0 = real speedup, ~0.67 expected: "
                        "2 items serialize on backend A ~= 2x T_A, backend B's ~T_B runs "
                        "fully overlapped)\n",
                static_cast<long long>(serialEstimateMs), static_cast<long long>(batchElapsedMs), ratio);
        // Generous threshold (0.9, not the ~0.67 theoretically expected) —
        // real loopback jitter, connection-setup overhead already paid
        // during the baseline phase, and general scheduling noise all
        // eat into the margin; this only needs to prove REAL, not
        // theoretically-perfect, concurrency.
        check("concurrent batch measurably faster than the serial-baseline estimate "
              "(ratio < 0.9 — proves real overlapping network I/O, not just correct "
              "orchestration)", ratio < 0.9);
    }

    check("baseline A fixture landed on server A's real disk",
          QFile::exists(serverRootA + "/" + fixtureA));
    check("baseline A content matches the local original byte-for-byte",
          filesMatchByteForByte(localSrcDir + "/" + fixtureA, serverRootA + "/" + fixtureA));
    check("baseline B fixture landed on server B's real disk",
          QFile::exists(serverRootB + "/" + fixtureB));
    check("baseline B content matches the local original byte-for-byte",
          filesMatchByteForByte(localSrcDir + "/" + fixtureB, serverRootB + "/" + fixtureB));
    check("concurrent A1 content matches byte-for-byte (no cross-thread corruption)",
          filesMatchByteForByte(localSrcDir + "/" + fixtureA2, serverRootA + "/" + fixtureA2));
    check("concurrent B1 content matches byte-for-byte (no cross-thread corruption)",
          filesMatchByteForByte(localSrcDir + "/" + fixtureB2, serverRootB + "/" + fixtureB2));
    check("concurrent A2 content matches byte-for-byte (no cross-thread corruption)",
          filesMatchByteForByte(localSrcDir + "/" + fixtureA3, serverRootA + "/" + fixtureA3));

    paneA->setBackend(new LocalBackend(), nullptr);
    paneB->setBackend(new LocalBackend(), nullptr);

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
