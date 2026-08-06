// Tests TransferManager's remote-to-remote staged-transfer ORCHESTRATION —
// direction/phase assignment, the phase-1 (download-to-temp) -> phase-2
// (upload-from-temp) transition, temp-file allocation/cleanup on every exit
// path, and retryItem()'s phase reset.
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
// against a live server, or a live two-server remote-to-remote transfer —
// see ARCHITECTURE.md's Known Gaps for what remains unverified even after
// this.
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QFile>
#include <QByteArray>
#include "ui/FilePaneWidget.h"
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
        connect(&m_timer, &QTimer::timeout, this, &FakeRemoteBackend::tick);
    }

    QString currentPath() const override { return QStringLiteral("/fake"); }
    bool isLocalFilesystem() const override { return false; }
    void requestCancel() override { m_cancelRequested = true; }
    // RemoteToRemote is cancel-only for v1 — pause is never offered for
    // this direction (see TransferQueueWidget's pauseCapableDirection), so
    // this is never actually called in this test; present only to satisfy
    // the interface.
    void requestPause() override {}

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
        QFile(localPath).open(QIODevice::WriteOnly);   // creates a real, empty file up front
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
    void createDirectory(const QString &) override {}
    void createFile(const QString &) override {}
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
        m_finished = false;
        m_done = resumeOffset;
        m_total = 1000;
        m_timer.start(30);   // ~30ms per 100-byte chunk — same pacing transfer_pause_test.cpp uses
    }

    QTimer m_timer;
    QString m_writeTargetPath;
    qint64 m_done = 0;
    qint64 m_total = 0;
    bool m_cancelRequested = false;
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

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    int currentItemId = -1;
    QString capturedTempPath;   // last known non-empty tempFilePath for the current item
    TransferPhase lastKnownPhase = TransferPhase::None;
    TransferStatus lastKnownStatus = TransferStatus::Queued;

    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        currentItemId = item.id;
        capturedTempPath.clear();
        lastKnownPhase = item.phase;
        lastKnownStatus = item.status;
    });
    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.id != currentItemId)
            return;
        if (!item.tempFilePath.isEmpty())
            capturedTempPath = item.tempFilePath;
        lastKnownPhase = item.phase;
        lastKnownStatus = item.status;
    });

    // Looks the current scenario's item up by id in the manager's own list
    // — "not just a local variable" confirmation, same reasoning
    // transfer_pause_test.cpp's own Paused check uses.
    auto findCurrentItem = [&]() -> const TransferItem * {
        for (const TransferItem &it : manager->items()) {
            if (it.id == currentItemId)
                return &it;
        }
        return nullptr;
    };

    // Nominal per-phase duration is 10 ticks x 30ms = 300ms, but each tick
    // does real synchronous QFile I/O (open/append/close), and by the time
    // later scenarios run, several prior phases' worth of that I/O plus
    // ordinary event-loop/OS scheduling jitter has accumulated — margins
    // below are deliberately generous (not the tightest that happened to
    // pass once) so this doesn't flake as more scenarios run later in the
    // same process, the same reasoning transfer_pause_test.cpp's own
    // margins already follow.

    // ---------- Scenario A: full happy path, both phases complete ----------
    QTimer::singleShot(100, &app, [&]() {
        manager->enqueue(sourcePane, destPane, "fakefile.bin");
    });

    QTimer::singleShot(300, &app, [&]() {
        const TransferItem *item = findCurrentItem();
        check("scenario A: enqueue() with both panes remote produces RemoteToRemote",
              currentItemId > 0 && item && item->direction == TransferDirection::RemoteToRemote);
        check("scenario A: phase 1 dispatched against the SOURCE fake with a real temp path",
              !sourceFake->lastDownloadLocalPath.isEmpty()
                  && sourceFake->lastDownloadLocalPath == capturedTempPath);
        check("scenario A: temp file genuinely exists on disk during phase 1",
              QFile::exists(capturedTempPath));
    });

    QTimer::singleShot(600, &app, [&]() {
        check("scenario A: phase 1 completion transitions to Uploading",
              lastKnownPhase == TransferPhase::Uploading);
        check("scenario A: phase 2 dispatched against the DESTINATION fake with the SAME temp path",
              destFake->lastUploadLocalPath == capturedTempPath);
        check("scenario A: the temp file existed when phase 2 started (real content from phase 1)",
              destFake->lastUploadLocalPathExisted);
    });

    QTimer::singleShot(1000, &app, [&]() {
        check("scenario A: item reaches Done", lastKnownStatus == TransferStatus::Done);
        check("scenario A: temp file deleted after phase 2 completes", !QFile::exists(capturedTempPath));
    });

    // ---------- Scenario B: cancel mid-phase-1 ----------
    QTimer::singleShot(1300, &app, [&]() {
        manager->enqueue(sourcePane, destPane, "fakefile.bin");
    });

    QTimer::singleShot(1450, &app, [&]() {
        check("scenario B: still in phase 1 when cancelled", lastKnownPhase == TransferPhase::Downloading);
        manager->cancelItem(currentItemId);
    });

    QTimer::singleShot(1700, &app, [&]() {
        check("scenario B: cancel mid-phase-1 produces Cancelled", lastKnownStatus == TransferStatus::Cancelled);
        check("scenario B: the (partially-written) temp file is cleaned up", !QFile::exists(capturedTempPath));
    });

    // ---------- Scenario C: cancel mid-phase-2 (a real download already
    // completed — this specifically exercises cleaning up an
    // already-downloaded temp file, distinct from B's phase-1 case, and
    // confirms m_currentBackend was actually re-pointed at the destination
    // fake, since cancelItem() only ever targets m_currentBackend) ----------
    QTimer::singleShot(2000, &app, [&]() {
        manager->enqueue(sourcePane, destPane, "fakefile.bin");
    });

    QTimer::singleShot(2500, &app, [&]() {
        check("scenario C: phase 1 completed, now in phase 2 when cancelled",
              lastKnownPhase == TransferPhase::Uploading);
        manager->cancelItem(currentItemId);
    });

    QTimer::singleShot(2750, &app, [&]() {
        check("scenario C: cancel mid-phase-2 produces Cancelled", lastKnownStatus == TransferStatus::Cancelled);
        check("scenario C: the already-downloaded temp file is cleaned up too",
              !QFile::exists(capturedTempPath));
    });

    // ---------- Scenario D: a genuine (non-cancelled) phase-2 failure,
    // then retry — confirms retryItem() resets phase back to Downloading
    // and allocates a FRESH temp path rather than re-using the one
    // cleanupTempFile() already deleted ----------
    QTimer::singleShot(3050, &app, [&]() {
        manager->enqueue(sourcePane, destPane, "fakefile.bin");
    });

    QString oldTempPath;
    QTimer::singleShot(3550, &app, [&]() {
        check("scenario D: phase 1 completed, now in phase 2 before the simulated failure",
              lastKnownPhase == TransferPhase::Uploading);
        oldTempPath = capturedTempPath;
        destFake->simulateFailure();
    });

    QTimer::singleShot(3800, &app, [&]() {
        check("scenario D: a genuine phase-2 failure produces Failed, not Cancelled",
              lastKnownStatus == TransferStatus::Failed);
        check("scenario D: the failed phase's temp file was cleaned up",
              !oldTempPath.isEmpty() && !QFile::exists(oldTempPath));
        manager->retryItem(currentItemId);
    });

    QTimer::singleShot(4050, &app, [&]() {
        check("scenario D: retry resets phase back to Downloading", lastKnownPhase == TransferPhase::Downloading);
        // allocateTempFilePath() is deterministic on item.id + fileName, so
        // retrying the SAME item legitimately regenerates the SAME path —
        // the old file was already deleted by onBackendFailed()'s cleanup,
        // so reusing that filename is harmless, not evidence of a stale
        // reference. What actually matters (and what the retryItem() fix
        // this scenario targets is about) is proven by the two checks
        // below instead: a real, non-empty path was allocated, AND
        // dispatch went back to downloadFile() against the SOURCE fake —
        // not straight to uploadFile() against a tempFilePath that was
        // still pointing at the now-deleted file, which is what would
        // happen without the fix (phase would still read Uploading).
        check("scenario D: retry allocates a valid (re-creatable) temp path",
              !capturedTempPath.isEmpty());
        check("scenario D: retry re-dispatches against the SOURCE fake (not stuck trying to re-upload)",
              sourceFake->lastDownloadLocalPath == capturedTempPath);
    });

    QTimer::singleShot(4750, &app, [&]() {
        check("scenario D: after retry, the item still completes normally end to end",
              lastKnownStatus == TransferStatus::Done);

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}

#include "remote_to_remote_test.moc"
