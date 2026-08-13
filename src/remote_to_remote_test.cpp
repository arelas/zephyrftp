// Tests TransferManager's remote-to-remote staged-transfer ORCHESTRATION —
// direction/phase assignment, the phase-1 (download-to-temp) -> phase-2
// (upload-from-temp) transition, temp-file allocation/cleanup on every exit
// path, retryItem()'s phase reset, and — added after a code review found
// this exact path untested (see FakeRemoteBackend::requestPause()'s own
// comment) — pausing and resuming during BOTH phases.
//
// Uses TWO independent fake backends representing two different "remote"
// servers — a legitimate test-double technique (TransferManager only ever
// talks to the RemoteBackend interface, so a fake honoring that interface's
// contract exercises the same orchestration code a real one would), same
// shape as transfer_pause_test.cpp's FakePausableBackend but writing real
// bytes to the local path it's given, so the temp file this test cares
// about genuinely exists on disk mid-transfer and genuinely disappears
// afterward — not just an in-memory status check.
//
// Does NOT test SftpBackend/FtpBackend's real downloadFile()/uploadFile()
// against a live server — see ARCHITECTURE.md's Known Gaps for what remains
// unverified even after this. A live two-server remote-to-remote transfer
// IS now covered separately, by verify-remote-to-remote-live.
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QByteArray>
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"
#include "backends/RemoteBackend.h"
#include "transfer/TransferManager.h"

namespace {

// Simulates an interruptible async transfer via a QTimer ticking in fixed
// chunks, same reasoning as FakePausableBackend: genuinely asynchronous, so
// there's a real window for the test to call cancelItem()/simulateFailure()
// mid-transfer. Unlike FakePausableBackend, downloadFile() actually writes
// real bytes to the localPath it's given — this test's whole point is
// confirming a real temp file gets created and cleaned up, which an
// in-memory-only fake couldn't demonstrate.
class FakeRemoteBackend : public RemoteBackend {
    Q_OBJECT
public:
    explicit FakeRemoteBackend(QObject *parent = nullptr) : RemoteBackend(parent) {
        static int nextInstanceId = 1;
        m_identity = QStringLiteral("fake://%1").arg(nextInstanceId++);
        connect(&m_timer, &QTimer::timeout, this, &FakeRemoteBackend::tick);
    }

    QString currentPath() const override { return QStringLiteral("/fake"); }
    bool isLocalFilesystem() const override { return false; }
    // Each instance gets its own identity string (see the constructor) —
    // this test constructs two independent fakes representing two
    // DIFFERENT servers, and moveEligible() requires equal, non-empty
    // identities, so two default-constructed fakes returning the same
    // fixed string would (wrongly) look move-eligible to any code that
    // checked. Not exercised by THIS test (it's about the remote-to-
    // remote staging path, not Move), but wrong-by-default here would be
    // a trap for a future test that reused this fake.
    QString connectionIdentity() const override { return m_identity; }
    void requestCancel() override { m_cancelRequested = true; }
    // Genuinely simulates pausing now — RemoteToRemote used to be
    // cancel-only in the UI on the theory that resuming a staged
    // two-phase transfer would need extra work to preserve phase/
    // tempFilePath/the resume offset across the pause. A code review
    // found that reasoning was never actually verified: TransferManager's
    // pauseItem()/resumeItem() are already direction-agnostic and don't
    // reset any of those three, so nothing extra was ever needed — this
    // was just never tested. Scenario E below is that test.
    void requestPause() override { m_pauseRequested = true; }

    // Test hooks — the specific local path this fake was last asked to
    // read from/write to, so the test can confirm phase 1 and phase 2
    // really do share the same temp file, and that it exists when phase 2
    // expects it to.
    QString lastDownloadLocalPath;
    QString lastUploadLocalPath;
    bool lastUploadLocalPathExisted = false;

    // Stops the current transfer immediately with a genuine, non-cancelled
    // failure — used to exercise retryItem()'s phase-reset path, which
    // cancellation alone wouldn't (a cancelled item isn't retried the same
    // way a failed one's "try again" implies).
    void simulateFailure() {
        // m_timer.stop() alone isn't enough — it prevents FUTURE timeouts
        // but doesn't retract a timeout event already sitting in the
        // queue at the exact moment stop() runs (a real, if narrow, Qt
        // timer race, more likely to actually surface under system load).
        // Without m_finished guarding tick() too, that already-queued
        // event could still fire tick() once more right after this
        // "failure", emitting a stray transferProgress/transferFinished
        // that corrupts whatever the item's state moves to next (e.g. a
        // retry already back in Downloading). Confirmed as the real cause
        // of an intermittent failure in scenario D before this guard was
        // added — not a hypothetical concern.
        m_finished = true;
        m_timer.stop();
        emit transferFailed(QStringLiteral("fakefile.bin"), QStringLiteral("simulated failure"));
    }

public slots:
    void connectToHost() override { emit connected(); }
    void listDirectory(const QString &path) override { emit directoryListed(path, {}); }

    void downloadFile(const QString &remotePath, const QString &localPath, qint64 resumeOffset = 0) override {
        Q_UNUSED(remotePath);
        lastDownloadLocalPath = localPath;
        m_writeTargetPath = localPath;
        // Only truncate on a FRESH start — a resumed download (scenario
        // E) must leave the bytes already on disk from before the pause
        // alone, matching real backends' own resume contract (trust
        // what's actually there), since tick()'s own QIODevice::Append
        // opens below just keep appending onto it either way.
        if (resumeOffset <= 0) {
            QFile f(localPath);
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);   // creates a real, empty file up front
        }
        beginTransfer(resumeOffset);
    }
    void uploadFile(const QString &localPath, const QString &remotePath, qint64 resumeOffset = 0) override {
        Q_UNUSED(remotePath);
        lastUploadLocalPath = localPath;
        lastUploadLocalPathExisted = QFile::exists(localPath);
        m_writeTargetPath.clear();   // nothing real to write to — "remotePath" isn't a local path
        beginTransfer(resumeOffset);
    }

    // Not exercised by this test — it's about remote-to-remote transfer
    // orchestration, not file management — but RemoteBackend is a pure
    // interface, so a concrete fake needs some implementation to be
    // instantiable at all.
    void deleteEntry(const QString &, bool) override {}
    void renameEntry(const QString &, const QString &) override {}
    void moveEntry(const QString &, const QString &, int requestId) override {
        emit entryMoveFailed(QStringLiteral("Not implemented"), requestId);
    }
    void createDirectory(const QString &) override {}
    void createFile(const QString &) override {}
    void setPermissions(const QString &, int) override {}
    void listDirectoryForEnumeration(const QString &, int) override {}

    // Must actually respond (not just satisfy the interface) — startNext()
    // always issues a checkExists() call against the destination before
    // dispatching, and waits for existsChecked() before proceeding. A
    // no-op stub here would leave that wait unanswered forever.
    void checkExists(const QString &path, int requestId) override {
        emit existsChecked(path, false, false, requestId);
    }

private slots:
    void tick() {
        // Guards against an already-queued timeout event still being
        // delivered after this transfer was already ended some other way
        // (simulateFailure(), or a cancel/finish that already fired on a
        // previous tick) — see simulateFailure()'s own comment for why
        // m_timer.stop() alone isn't sufficient on its own.
        if (m_finished)
            return;
        if (m_cancelRequested) {
            m_finished = true;
            m_timer.stop();
            emit transferFailed(QStringLiteral("fakefile.bin"), QStringLiteral("Cancelled"));
            return;
        }
        if (m_pauseRequested) {
            m_finished = true;   // same "genuinely ended" guard cancel uses — see simulateFailure()'s comment
            m_timer.stop();
            emit transferPaused(QStringLiteral("fakefile.bin"), m_done);
            return;
        }
        m_done = qMin(m_done + 100, m_total);
        if (!m_writeTargetPath.isEmpty()) {
            QFile f(m_writeTargetPath);
            if (f.open(QIODevice::Append))
                f.write(QByteArray(100, 'x'));
        }
        emit transferProgress(QStringLiteral("fakefile.bin"), m_done, m_total);
        if (m_done >= m_total) {
            m_finished = true;
            m_timer.stop();
            emit transferFinished(QStringLiteral("fakefile.bin"));
        }
    }

private:
    void beginTransfer(qint64 resumeOffset) {
        m_cancelRequested = false;
        m_pauseRequested = false;
        m_finished = false;
        m_done = resumeOffset;
        m_total = 1000;
        m_timer.start(30);   // ~30ms per 100-byte chunk — same pacing transfer_pause_test.cpp uses
    }

    QTimer m_timer;
    QString m_identity;
    QString m_writeTargetPath;
    qint64 m_done = 0;
    qint64 m_total = 0;
    bool m_cancelRequested = false;
    bool m_pauseRequested = false;
    // Set once this transfer has genuinely ended (cancelled, finished, or
    // simulateFailure()'d) — see tick()'s and simulateFailure()'s own
    // comments for the specific race this closes.
    bool m_finished = false;
};

}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // sourcePane's backend plays "server A" (phase 1 downloads FROM this),
    // destPane's plays "server B" (phase 2 uploads TO this) — two
    // independent fakes, not one, since a real remote-to-remote transfer
    // genuinely involves two different servers.
    auto *sourceFake = new FakeRemoteBackend();
    auto *destFake = new FakeRemoteBackend();
    auto *sourcePane = new FilePaneWidget(sourceFake);
    auto *destPane = new FilePaneWidget(destFake);
    auto *manager = new TransferManager(&app);

    // Scenario F's own blocker item needs a local source pane distinct
    // from sourcePane/destPane — its whole point is to claim sourceFake
    // as an UNRELATED item's executor, which needs a real (if empty)
    // local directory to navigate to, even though the fake executor never
    // actually reads the local path it's given.
    QDir("/tmp/remote_to_remote_test_f").removeRecursively();
    QDir().mkpath("/tmp/remote_to_remote_test_f");
    auto *blockerLocalPane = new FilePaneWidget(new LocalBackend());
    blockerLocalPane->navigateTo("/tmp/remote_to_remote_test_f");

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    // Event-driven, not time-driven — reacts to the ACTUAL itemUpdated
    // signal each step is waiting for, rather than guessing a wall-clock
    // delay by which it should have happened. This matters for real
    // robustness, not just tidiness: an earlier, fixed-delay version of
    // this test (generous margins, double nominal, passed 20+ repeated
    // local runs) still flaked on a real CI runner doing other heavy work
    // concurrently (a linuxdeploy AppImage build sharing the same job),
    // and reproducing that locally under deliberate CPU contention (14
    // busy-loop processes on a 16-core machine) made EVERY run fail —
    // there is no fixed delay that's safe against arbitrary system load.
    // Reacting to the real signal has no such ceiling.
    enum class Stage {
        A_WaitDownloadStarted, A_WaitUploadStarted, A_WaitDone,
        B_WaitDownloadStarted, B_WaitCancelled,
        C_WaitUploadStarted, C_WaitCancelled,
        D_WaitUploadStarted, D_WaitFailed, D_WaitRetryDownloadStarted, D_WaitDone,
        E_WaitDownloadStarted, E_WaitPausedPhase1, E_WaitResumedPhase1,
        E_WaitUploadStarted, E_WaitPausedPhase2, E_WaitResumedPhase2, E_WaitDone,
        F_WaitUploadStarted, F_WaitPaused, F_WaitBlockerStarted, F_WaitResumedDespiteBlocker,
        AllDone
    };
    Stage stage = Stage::A_WaitDownloadStarted;

    int currentItemId = -1;
    QString capturedTempPath;   // last known non-empty tempFilePath for the current item
    QString oldTempPathD;       // scenario D's temp path, captured just before the simulated failure
    qint64 pausedBytesDoneE1 = -1;   // scenario E's paused offset, phase 1
    qint64 pausedBytesDoneE2 = -1;   // scenario E's paused offset, phase 2
    QString pausedTempPathE;        // scenario E's temp path, captured at the phase-1 pause

    // Scenario F's own tracking — needs to watch TWO items at once (the
    // paused RemoteToRemote item AND the unrelated blocker on the same
    // source backend), unlike every earlier scenario's single
    // currentItemId. Handled by a separate connection below, not by
    // reusing the main one above, so scenarios A-E stay untouched.
    int f1ItemId = -1, f2ItemId = -1;
    TransferStatus f1Status = TransferStatus::Queued;
    TransferStatus f2Status = TransferStatus::Queued;
    qint64 f1BytesDoneAtPause = -1;

    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        currentItemId = item.id;
        capturedTempPath.clear();
        if (stage == Stage::A_WaitDownloadStarted) {
            check("scenario A: enqueue() with both panes remote produces RemoteToRemote",
                  item.direction == TransferDirection::RemoteToRemote);
        }
    });

    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.id != currentItemId)
            return;
        if (!item.tempFilePath.isEmpty())
            capturedTempPath = item.tempFilePath;

        // Only a REAL tick (bytesDone > 0) proves downloadFile()/
        // uploadFile() has actually started — dispatchActiveItem()'s own
        // status=InProgress update fires (with bytesDone still 0) before
        // the queued call to the backend has even been processed, so
        // acting on that earlier update specifically (e.g. cancelling)
        // could race with beginTransfer() resetting the fake's own
        // m_cancelRequested flag right after. Waiting for a genuine tick
        // sidesteps that race entirely.
        const bool realProgressTick = item.status == TransferStatus::InProgress && item.bytesDone > 0;

        switch (stage) {
        case Stage::A_WaitDownloadStarted:
            if (realProgressTick && item.phase == TransferPhase::Downloading) {
                check("scenario A: phase 1 dispatched against the SOURCE fake with a real temp path",
                      !sourceFake->lastDownloadLocalPath.isEmpty()
                          && sourceFake->lastDownloadLocalPath == capturedTempPath);
                check("scenario A: temp file genuinely exists on disk during phase 1",
                      QFile::exists(capturedTempPath));
                stage = Stage::A_WaitUploadStarted;
            }
            break;
        case Stage::A_WaitUploadStarted:
            if (realProgressTick && item.phase == TransferPhase::Uploading) {
                check("scenario A: phase 1 completion transitions to Uploading", item.phase == TransferPhase::Uploading);
                check("scenario A: phase 2 dispatched against the DESTINATION fake with the SAME temp path",
                      destFake->lastUploadLocalPath == capturedTempPath);
                check("scenario A: the temp file existed when phase 2 started (real content from phase 1)",
                      destFake->lastUploadLocalPathExisted);
                stage = Stage::A_WaitDone;
            }
            break;
        case Stage::A_WaitDone:
            if (item.status == TransferStatus::Done) {
                check("scenario A: item reaches Done", item.status == TransferStatus::Done);
                check("scenario A: temp file deleted after phase 2 completes", !QFile::exists(capturedTempPath));
                stage = Stage::B_WaitDownloadStarted;
                manager->enqueue(sourcePane, destPane, "fakefile.bin");
            }
            break;

        // ---------- Scenario B: cancel mid-phase-1 ----------
        case Stage::B_WaitDownloadStarted:
            if (realProgressTick && item.phase == TransferPhase::Downloading) {
                stage = Stage::B_WaitCancelled;   // set BEFORE cancelItem() — see reentrancy note below
                manager->cancelItem(currentItemId);
            }
            break;
        case Stage::B_WaitCancelled:
            if (item.status == TransferStatus::Cancelled) {
                check("scenario B: still in phase 1 when cancelled", item.phase == TransferPhase::Downloading);
                check("scenario B: cancel mid-phase-1 produces Cancelled", item.status == TransferStatus::Cancelled);
                check("scenario B: the (partially-written) temp file is cleaned up", !QFile::exists(capturedTempPath));
                stage = Stage::C_WaitUploadStarted;
                manager->enqueue(sourcePane, destPane, "fakefile.bin");
            }
            break;

        // ---------- Scenario C: cancel mid-phase-2 (a real download
        // already completed — this specifically exercises cleaning up an
        // already-downloaded temp file, distinct from B's phase-1 case,
        // and confirms m_currentBackend was actually re-pointed at the
        // destination fake, since cancelItem() only ever targets
        // m_currentBackend — if it weren't re-pointed, this cancel would
        // silently hit the wrong (already-finished) backend and this
        // stage would simply never see Cancelled, tripping the safety
        // timeout below instead of a clean, immediate failure) ----------
        case Stage::C_WaitUploadStarted:
            if (realProgressTick && item.phase == TransferPhase::Uploading) {
                stage = Stage::C_WaitCancelled;
                manager->cancelItem(currentItemId);
            }
            break;
        case Stage::C_WaitCancelled:
            if (item.status == TransferStatus::Cancelled) {
                check("scenario C: phase 1 completed, now in phase 2 when cancelled",
                      item.phase == TransferPhase::Uploading);
                check("scenario C: cancel mid-phase-2 produces Cancelled", item.status == TransferStatus::Cancelled);
                check("scenario C: the already-downloaded temp file is cleaned up too",
                      !QFile::exists(capturedTempPath));
                stage = Stage::D_WaitUploadStarted;
                manager->enqueue(sourcePane, destPane, "fakefile.bin");
            }
            break;

        // ---------- Scenario D: a genuine (non-cancelled) phase-2
        // failure, then retry — confirms retryItem() resets phase back to
        // Downloading and allocates a valid temp path rather than trying
        // to reuse the one cleanupTempFile() already deleted ----------
        case Stage::D_WaitUploadStarted:
            if (realProgressTick && item.phase == TransferPhase::Uploading) {
                check("scenario D: phase 1 completed, now in phase 2 before the simulated failure",
                      item.phase == TransferPhase::Uploading);
                oldTempPathD = capturedTempPath;
                // simulateFailure() emits transferFailed() SYNCHRONOUSLY
                // (a direct, same-thread connection to onBackendFailed()),
                // which re-enters this very lambda before this call
                // returns — stage MUST already read D_WaitFailed by then,
                // or that reentrant call would (harmlessly, but
                // pointlessly) re-match THIS case instead, and the real
                // Failed transition would have nothing left to trigger
                // D_WaitFailed's handling, hanging until the safety
                // timeout. Same reasoning applies to retryItem() below,
                // which also synchronously emits itemUpdated (Queued)
                // before its own async re-dispatch.
                stage = Stage::D_WaitFailed;
                destFake->simulateFailure();
            }
            break;
        case Stage::D_WaitFailed:
            if (item.status == TransferStatus::Failed) {
                check("scenario D: a genuine phase-2 failure produces Failed, not Cancelled",
                      item.status == TransferStatus::Failed);
                check("scenario D: the failed phase's temp file was cleaned up",
                      !oldTempPathD.isEmpty() && !QFile::exists(oldTempPathD));
                stage = Stage::D_WaitRetryDownloadStarted;
                manager->retryItem(currentItemId);
            }
            break;
        case Stage::D_WaitRetryDownloadStarted:
            if (realProgressTick && item.phase == TransferPhase::Downloading) {
                // allocateTempFilePath() is deterministic on item.id +
                // fileName, so retrying the SAME item legitimately
                // regenerates the SAME path — the old file was already
                // deleted by onBackendFailed()'s cleanup, so reusing that
                // filename is harmless, not evidence of a stale
                // reference. What actually matters (and what the
                // retryItem() fix this scenario targets is about) is
                // proven by the two checks below instead: a real,
                // non-empty path was allocated, AND dispatch went back to
                // downloadFile() against the SOURCE fake — not straight
                // to uploadFile() against a tempFilePath that was still
                // pointing at the now-deleted file, which is what would
                // happen without the fix (phase would still read
                // Uploading).
                check("scenario D: retry resets phase back to Downloading", item.phase == TransferPhase::Downloading);
                check("scenario D: retry allocates a valid (re-creatable) temp path",
                      !capturedTempPath.isEmpty());
                check("scenario D: retry re-dispatches against the SOURCE fake (not stuck trying to re-upload)",
                      sourceFake->lastDownloadLocalPath == capturedTempPath);
                stage = Stage::D_WaitDone;
            }
            break;
        case Stage::D_WaitDone:
            if (item.status == TransferStatus::Done) {
                check("scenario D: after retry, the item still completes normally end to end",
                      item.status == TransferStatus::Done);
                stage = Stage::E_WaitDownloadStarted;
                manager->enqueue(sourcePane, destPane, "fakefile.bin");
            }
            break;

        // ---------- Scenario E: pause + resume during BOTH phases — the
        // specific path a code review found untested, contradicting a
        // stale comment that claimed RemoteToRemote pause/resume needed
        // more work first (see FakeRemoteBackend::requestPause()'s own
        // comment). Confirms phase/tempFilePath/bytesDone all correctly
        // survive a pause/resume cycle with no RemoteToRemote-specific
        // handling, in both the download and the upload half. ----------
        case Stage::E_WaitDownloadStarted:
            if (realProgressTick && item.phase == TransferPhase::Downloading) {
                pausedTempPathE = capturedTempPath;
                stage = Stage::E_WaitPausedPhase1;   // set BEFORE pauseItem() — same discipline as B/C's cancelItem() calls
                manager->pauseItem(currentItemId);
            }
            break;
        case Stage::E_WaitPausedPhase1:
            if (item.status == TransferStatus::Paused) {
                check("scenario E: paused while still in phase 1 (Downloading)",
                      item.phase == TransferPhase::Downloading);
                check("scenario E: paused with a real nonzero bytesDone", item.bytesDone > 0);
                check("scenario E: the temp file survives the pause (not cleaned up)",
                      QFile::exists(capturedTempPath));
                pausedBytesDoneE1 = item.bytesDone;
                stage = Stage::E_WaitResumedPhase1;   // set BEFORE resumeItem() — it synchronously emits itemUpdated(Queued) before its own async re-dispatch, same reentrancy shape as scenario D's retryItem()
                manager->resumeItem(currentItemId);
            }
            break;
        case Stage::E_WaitResumedPhase1:
            if (realProgressTick && item.phase == TransferPhase::Downloading) {
                check("scenario E: resume continues phase 1 from the paused offset, not from zero",
                      item.bytesDone >= pausedBytesDoneE1);
                check("scenario E: resume reuses the SAME temp file (not a freshly allocated one)",
                      capturedTempPath == pausedTempPathE);
                stage = Stage::E_WaitUploadStarted;
            }
            break;
        case Stage::E_WaitUploadStarted:
            if (realProgressTick && item.phase == TransferPhase::Uploading) {
                check("scenario E: phase 1 -> phase 2 transition still works after a phase-1 pause/resume",
                      item.phase == TransferPhase::Uploading);
                stage = Stage::E_WaitPausedPhase2;
                manager->pauseItem(currentItemId);
            }
            break;
        case Stage::E_WaitPausedPhase2:
            if (item.status == TransferStatus::Paused) {
                check("scenario E: paused while in phase 2 (Uploading)", item.phase == TransferPhase::Uploading);
                check("scenario E: paused with a real nonzero bytesDone (phase 2)", item.bytesDone > 0);
                pausedBytesDoneE2 = item.bytesDone;
                stage = Stage::E_WaitResumedPhase2;
                manager->resumeItem(currentItemId);
            }
            break;
        case Stage::E_WaitResumedPhase2:
            if (realProgressTick && item.phase == TransferPhase::Uploading) {
                check("scenario E: resume continues phase 2 from the paused offset, not from zero",
                      item.bytesDone >= pausedBytesDoneE2);
                stage = Stage::E_WaitDone;
            }
            break;
        case Stage::E_WaitDone:
            if (item.status == TransferStatus::Done) {
                check("scenario E: after pausing/resuming BOTH phases, the item still completes normally end to end",
                      item.status == TransferStatus::Done);
                stage = Stage::F_WaitUploadStarted;
                manager->enqueue(sourcePane, destPane, "fakefile.bin");
            }
            break;

        // ---------- Scenario F: a real bug found by a later code review —
        // resumeItem() never resets phase for RemoteToRemote (unlike
        // retryItem(), which does), so a resume from mid-phase-2
        // (Uploading) reached requiredBackendsForDispatch() with
        // phase == Uploading, which unconditionally demanded BOTH source
        // and destination backends regardless of phase — wrongly
        // re-claiming the source backend an Uploading-phase resume no
        // longer needs at all (dispatchActiveItem()'s own Uploading
        // branch never touches it). Reproduced here directly: pause mid-
        // phase-2, let a completely UNRELATED item claim sourceFake, then
        // confirm resuming the paused item is NOT blocked by that
        // unrelated item still running — the rest of this scenario is
        // handled by the dedicated connection below, since it needs to
        // track TWO items at once (this one and the blocker), unlike
        // every case above. ----------
        case Stage::F_WaitUploadStarted:
            if (realProgressTick && item.phase == TransferPhase::Uploading) {
                stage = Stage::F_WaitPaused;
                manager->pauseItem(currentItemId);
            }
            break;
        case Stage::F_WaitPaused:
            if (item.status == TransferStatus::Paused) {
                check("scenario F: paused while in phase 2 (Uploading)", item.phase == TransferPhase::Uploading);
                f1ItemId = currentItemId;
                f1BytesDoneAtPause = item.bytesDone;
                stage = Stage::F_WaitBlockerStarted;
                // An UNRELATED item, deliberately targeting sourceFake as
                // ITS OWN executor (LocalToRemote: destPane == sourcePane,
                // so dstBackend == sourceFake) — sourceFake is otherwise
                // completely idle right now (phase 1 for the paused item
                // above finished long ago), so this claims it freely.
                manager->enqueue(blockerLocalPane, sourcePane, "blocker.bin");
            }
            break;
        case Stage::F_WaitBlockerStarted:
        case Stage::F_WaitResumedDespiteBlocker:
            break;   // handled entirely by the dedicated connection below, not this one
        case Stage::AllDone:
            break;
        }
    });

    // Scenario F's dedicated tracking — needs BOTH f1 (the paused item)
    // and f2 (the unrelated blocker) at once, once f2 exists; reusing the
    // main handler's single currentItemId gate above would silently drop
    // every further update for f1 the instant f2 is enqueued (itemAdded
    // unconditionally reassigns currentItemId to whichever item was added
    // most recently). A separate connection, filtering by f1ItemId/
    // f2ItemId explicitly, sidesteps that entirely without touching
    // scenarios A-E's own working logic above.
    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        if (item.fileName == "blocker.bin")
            f2ItemId = item.id;
    });
    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.id == f2ItemId) {
            f2Status = item.status;
            if (stage == Stage::F_WaitBlockerStarted
                && f2Status == TransferStatus::InProgress && item.bytesDone > 0) {
                check("scenario F: the unrelated blocker item genuinely claimed sourceFake",
                      sourceFake->lastUploadLocalPath.endsWith("blocker.bin"));
                stage = Stage::F_WaitResumedDespiteBlocker;
                manager->resumeItem(f1ItemId);
            }
        }
        if (item.id == f1ItemId) {
            f1Status = item.status;
            if (stage == Stage::F_WaitResumedDespiteBlocker
                && f1Status == TransferStatus::InProgress && item.bytesDone > 0) {
                // The real assertion: the blocker must still be genuinely
                // running (not yet Done) at the exact moment the resumed
                // item reaches InProgress — reaching InProgress at all is
                // trivially true either way (the buggy pre-fix behavior
                // isn't "never resumes", it's "resumes only once the
                // unrelated blocker happens to finish and free sourceFake
                // first"), so f2 still being mid-flight right here is what
                // actually distinguishes "resume proceeded independently"
                // from "resume silently waited its turn behind an item it
                // never needed to".
                check("scenario F: resuming a phase-2-paused item is NOT blocked waiting for an "
                      "unrelated item still actively running on the source backend it no longer needs",
                      f2Status == TransferStatus::InProgress);
                check("scenario F: resume continues phase 2 from the paused offset, not from zero",
                      item.bytesDone >= f1BytesDoneAtPause);
                stage = Stage::AllDone;
                qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
                app.exit(allPass ? 0 : 1);
            }
        }
    });

    manager->enqueue(sourcePane, destPane, "fakefile.bin");

    // Safety net, not the primary mechanism: if a real bug ever breaks an
    // expected state transition (e.g. m_currentBackend NOT actually
    // re-pointed, so a cancel silently hits the wrong backend), the state
    // machine above would otherwise wait forever for a signal that will
    // never come. A generous absolute deadline turns that into a clear,
    // fast, informative failure instead of an indefinite hang tying up a
    // CI job.
    QTimer::singleShot(20000, &app, [&]() {
        if (stage == Stage::AllDone)
            return;
        check(QStringLiteral("test timed out waiting for a state transition (stuck at stage %1)")
                  .arg(static_cast<int>(stage)),
              false);
        qDebug() << "[test] AT LEAST ONE FAILURE";
        app.exit(1);
    });

    return app.exec();
}

#include "remote_to_remote_test.moc"
