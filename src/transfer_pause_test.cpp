// Tests TransferManager's pause/resume ORCHESTRATION — status transitions,
// bytesDone preservation across a pause, that resumeItem() actually
// causes the next run to start from the paused offset rather than zero,
// and — added after live-server verification (verify-remote-to-remote-live)
// found a real bug — that resuming does NOT re-run the destination
// conflict check startNext() normally does for a fresh dispatch. That
// bug was invisible to this fake-backend test until checkExistsCallCount
// was added specifically to catch it: a real backend's checkExists()
// would truthfully report "yes, something's there" on resume (this
// item's own partial content from before the pause), triggering a real,
// spurious Overwrite/Skip conflict prompt for what was never actually a
// conflict — but this fake's own checkExists() stub always reports
// false regardless, so the ONLY way to catch a stray second call here is
// to count them directly, not to observe a dialog that (with the fix
// working) should never even be triggered.
//
// Does NOT test SftpBackend's real byte-offset resume logic (the seek64
// calls, the local-file clamping/trimming) — that needs a live SFTP
// server and is flagged as such in ARCHITECTURE.md's Known gaps,
// consistent with how SftpBackend's cancel-mid-transfer is already
// flagged unverified there. What CAN be verified without a server is
// whether TransferManager itself correctly orchestrates pause and
// resume — that's what this tests, using a fake backend built for
// exactly this purpose (a legitimate test-double technique: TransferManager
// only ever talks to the RemoteBackend interface, so a fake that honors
// that interface's contract exercises the same code paths a real one would).
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QEventLoop>
#include <functional>
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"
#include "backends/RemoteBackend.h"
#include "transfer/TransferManager.h"

namespace {

// Simulates an interruptible async transfer via a QTimer ticking in fixed
// chunks — genuinely asynchronous (not a single synchronous loop), so
// there's a real window between ticks for the test to call
// pauseItem()/resumeItem(), same as there would be with a real backend
// on a real network.
class FakePausableBackend : public RemoteBackend {
    Q_OBJECT
public:
    explicit FakePausableBackend(QObject *parent = nullptr) : RemoteBackend(parent) {
        connect(&m_timer, &QTimer::timeout, this, &FakePausableBackend::tick);
    }

    QString currentPath() const override { return QStringLiteral("/fake"); }
    bool isLocalFilesystem() const override { return false; }   // pretend remote, so pause is meaningful
    QString connectionIdentity() const override { return QStringLiteral("fake"); }
    void requestCancel() override { m_cancelRequested = true; }
    void requestPause() override { m_pauseRequested = true; }

    // Test hook — see main()'s own check() using this: a resumed item
    // must NOT trigger a second checkExists() call. Real backends would
    // legitimately report "yes, something's there" the second time
    // (this item's own partial content from before the pause), which
    // would show a real, spurious Overwrite/Skip conflict prompt for
    // what isn't actually a conflict at all — a real bug this fake's
    // own always-false checkExists() stub could never have caught on
    // its own, only found via live-server verification
    // (verify-remote-to-remote-live). This count is what actually proves
    // the fix (TransferItem::skipConflictCheckOnDispatch) works, more
    // directly than trying to detect a dialog that (with the fix
    // working) should never even be triggered.
    int checkExistsCallCount = 0;

    // Test hook for the cancel/pause-race regression scenario below —
    // directly emits transferPaused() regardless of m_cancelRequested,
    // bypassing tick()'s own ordering (which always resolves a pending
    // cancel first and could never naturally produce this race). A real
    // backend's own internal cancel/pause resolution could plausibly land
    // either way depending on exact timing; this drives TransferManager's
    // ORCHESTRATION side of that race directly and deterministically,
    // independent of how any specific backend happens to arbitrate it.
    void forceEmitPaused() { emit transferPaused(m_name, m_done); }

public slots:
    void connectToHost() override { emit connected(); }
    void listDirectory(const QString &path) override { emit directoryListed(path, {}); }

    void downloadFile(const QString &remotePath, const QString &localPath, qint64 resumeOffset = 0) override {
        Q_UNUSED(localPath);
        start(remotePath, resumeOffset);
    }
    void uploadFile(const QString &localPath, const QString &remotePath, qint64 resumeOffset = 0) override {
        Q_UNUSED(remotePath);
        start(localPath, resumeOffset);
    }

    // Not exercised by this test — it's about pause/resume orchestration,
    // not file management — but RemoteBackend is a pure interface, so a
    // concrete fake needs some implementation to be instantiable at all.
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
    // now always issues a checkExists() call before dispatching a
    // transfer, and waits for existsChecked() before proceeding. A no-op
    // stub here would leave that wait unanswered forever, hanging every
    // test that exercises a real transfer through this fake.
    void checkExists(const QString &path, int requestId) override
    {
        ++checkExistsCallCount;
        emit existsChecked(path, false, false, requestId);
    }

private slots:
    void tick() {
        if (m_cancelRequested) {
            m_timer.stop();
            emit transferFailed(m_name, QStringLiteral("Cancelled"));
            return;
        }
        if (m_pauseRequested) {
            m_timer.stop();
            emit transferPaused(m_name, m_done);
            return;
        }
        m_done = qMin(m_done + 100, m_total);
        emit transferProgress(m_name, m_done, m_total);
        if (m_done >= m_total) {
            m_timer.stop();
            emit transferFinished(m_name);
        }
    }

private:
    void start(const QString &name, qint64 resumeOffset) {
        m_cancelRequested = false;
        m_pauseRequested = false;
        m_name = name;
        m_done = resumeOffset;
        m_total = 1000;
        m_timer.start(30);   // ~30ms per 100-byte chunk — fast enough for a test, slow enough to interrupt
    }

    QTimer m_timer;
    QString m_name;
    qint64 m_done = 0;
    qint64 m_total = 0;
    bool m_cancelRequested = false;
    bool m_pauseRequested = false;
};

}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Fixture for the cancel-unblocks-the-queue regression scenario
    // further down — a real file, so that item's real LocalBackend
    // transfer can genuinely reach Done (not just start), proving the
    // queue is actually unblocked, not merely no-longer-stuck.
    const QString unblockSrcDir = "/tmp/transfer_pause_test/unblock_src";
    const QString unblockDstDir = "/tmp/transfer_pause_test/unblock_dst";
    QDir("/tmp/transfer_pause_test").removeRecursively();
    QDir().mkpath(unblockSrcDir);
    QDir().mkpath(unblockDstDir);
    {
        QFile f(unblockSrcDir + "/real_file.bin");
        f.open(QIODevice::WriteOnly);
        f.write(QByteArray(500, 'y'));
    }

    auto *fakeBackend = new FakePausableBackend();
    auto *leftPane = new FilePaneWidget(new LocalBackend());
    auto *rightPane = new FilePaneWidget(fakeBackend);
    auto *manager = new TransferManager(&app);

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    int itemId = -1;
    qint64 bytesDoneAtPause = -1;
    qint64 firstBytesDoneAfterResume = -1;
    bool sawResumeInProgress = false;
    qint64 latestInProgressBytesDone = 0;

    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        itemId = item.id;
    });

    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.id != itemId)
            return;
        qDebug() << "[test] itemUpdated: status =" << static_cast<int>(item.status)
                 << "bytesDone =" << item.bytesDone;

        if (item.status == TransferStatus::InProgress) {
            latestInProgressBytesDone = item.bytesDone;
        }
        if (item.status == TransferStatus::Paused && bytesDoneAtPause < 0) {
            bytesDoneAtPause = item.bytesDone;
        }
        if (item.status == TransferStatus::InProgress && bytesDoneAtPause >= 0 && !sawResumeInProgress) {
            // First InProgress update after a resume — capture it once.
            firstBytesDoneAfterResume = item.bytesDone;
            sawResumeInProgress = true;
        }
    });

    // Same reasoning as navigation-test.cpp's own waitUntil — a real hang
    // found and fixed while building the macOS CI job: Step 2 below used
    // to pause after a FIXED 250ms delay, assuming the fake backend's
    // first 30ms tick would have already landed by then. True on the
    // Linux CI containers, but the first real run on a macOS GitHub-hosted
    // runner (slower/colder to spin up threads and dispatch the initial
    // async enqueue()/checkExists() round trip) blew through that budget
    // with zero ticks landed, so "paused with nonzero bytesDone" failed
    // for a reason that had nothing to do with the actual pause/resume
    // behavior under test.
    auto waitUntil = [&](std::function<bool()> condition) {
        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeoutTimer.start(5000);
        while (!condition() && timeoutTimer.isActive())
            loop.processEvents(QEventLoop::AllEvents, 20);
    };

    // Step 1: start the (fake) transfer, then pause it partway through —
    // waits for real, observed progress (at least one genuine tick)
    // rather than trusting a fixed wall-clock delay to have been enough,
    // then pauses right away (still well before the fake backend's
    // ~300ms total run time, so there's a meaningful, nonzero
    // bytesDone, not so late it finishes first).
    QTimer::singleShot(100, &app, [&]() {
        manager->enqueue(leftPane, rightPane, "fakefile.bin");
        waitUntil([&]() { return itemId > 0; });
        check("item exists before pausing", itemId > 0);
        waitUntil([&]() { return latestInProgressBytesDone > 0; });
        manager->pauseItem(itemId);
    });

    // Step 3: confirm it actually paused with real partial progress.
    QTimer::singleShot(500, &app, [&]() {
        check("paused with nonzero bytesDone", bytesDoneAtPause > 0 && bytesDoneAtPause < 1000);

        bool foundPaused = false;
        for (const TransferItem &it : manager->items()) {
            if (it.id == itemId && it.status == TransferStatus::Paused)
                foundPaused = true;
        }
        check("item status is genuinely Paused (not just a local variable)", foundPaused);

        // Resume it.
        manager->resumeItem(itemId);
    });

    // Step 4: confirm resume started from the paused offset, not zero —
    // the whole point of pause/resume rather than cancel/retry.
    QTimer::singleShot(700, &app, [&]() {
        check("resumed and made progress", sawResumeInProgress);
        check("resume continued from the paused offset rather than restarting from zero",
              firstBytesDoneAfterResume >= bytesDoneAtPause);
        qDebug() << "[test] bytesDoneAtPause =" << bytesDoneAtPause
                 << "firstBytesDoneAfterResume =" << firstBytesDoneAfterResume;
    });

    // Step 5: let it run to completion.
    QTimer::singleShot(1500, &app, [&]() {
        bool foundDone = false;
        qint64 finalBytesDone = -1;
        for (const TransferItem &it : manager->items()) {
            if (it.id == itemId) {
                foundDone = (it.status == TransferStatus::Done);
                finalBytesDone = it.bytesDone;
            }
        }
        check("transfer completed to Done after resuming", foundDone);
        check("final bytesDone reached the full total", finalBytesDone == 1000);
        check("resuming did NOT re-check the destination for a conflict (checkExists called exactly once, "
              "not once per pause/resume cycle)", fakeBackend->checkExistsCallCount == 1);
    });

    // ---------- Regression: cancelItem() on the active item must not
    // permanently stall the queue if its backend has gone null. A real
    // bug: m_currentBackend is a QPointer that goes null if the active
    // item's backend is destroyed/swapped mid-transfer (e.g. Disconnect
    // on a pane with a transfer running against it) — cancelItem() used
    // to just no-op in that case (nothing to call requestCancel() on),
    // leaving the item AND m_activeIndex stuck forever, permanently
    // blocking every later Queued item. A second, real LocalBackend
    // transfer (item2) proves the queue is genuinely unblocked
    // afterward, not just that item1 changed status. ----------
    auto *unblockFake = new FakePausableBackend();
    auto *unblockSrcPane = new FilePaneWidget(new LocalBackend());
    auto *unblockDestPane = new FilePaneWidget(unblockFake);
    auto *unblockLocalDestPane = new FilePaneWidget(new LocalBackend());
    auto *unblockManager = new TransferManager(&app);
    int unblockItem1Id = -1, unblockItem2Id = -1;
    QObject::connect(unblockManager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        if (item.fileName == "fakefile.bin") unblockItem1Id = item.id;
        if (item.fileName == "real_file.bin") unblockItem2Id = item.id;
    });
    // Navigated immediately (not inside a later timer) so both panes'
    // currentDirectory() is already correct by the time enqueue() runs
    // below — navigateTo() dispatches asynchronously, so calling enqueue()
    // right after it (rather than giving it real time first) would build
    // item2's source/dest paths from whatever directory the pane was
    // still sitting in beforehand, not unblockSrcDir/unblockDstDir. Same
    // pattern transfer_queue_test.cpp's own phase 1 already uses.
    unblockSrcPane->navigateTo(unblockSrcDir);
    unblockLocalDestPane->navigateTo(unblockDstDir);

    QTimer::singleShot(1700, &app, [&]() {
        unblockManager->enqueue(unblockSrcPane, unblockDestPane, "fakefile.bin");   // dispatches against the fake
        unblockManager->enqueue(unblockSrcPane, unblockLocalDestPane, "real_file.bin");   // stays Queued behind item1
    });

    // Swap the destination pane's backend — the same real trigger
    // (Disconnect mid-transfer) that nulls TransferManager's
    // m_currentBackend QPointer via deleteLater() + the event loop
    // actually running it.
    QTimer::singleShot(1900, &app, [&]() {
        unblockDestPane->setBackend(new LocalBackend());
    });

    QTimer::singleShot(2100, &app, [&]() {
        unblockManager->cancelItem(unblockItem1Id);
    });

    QTimer::singleShot(2500, &app, [&]() {
        bool item1Cancelled = false, item2Done = false;
        for (const TransferItem &it : unblockManager->items()) {
            if (it.id == unblockItem1Id && it.status == TransferStatus::Cancelled)
                item1Cancelled = true;
            if (it.id == unblockItem2Id && it.status == TransferStatus::Done)
                item2Done = true;
        }
        check("cancelItem() on an active item whose backend went null still resolves it to Cancelled",
              item1Cancelled);
        check("the queue was genuinely unblocked afterward — item2 actually ran to Done, "
              "not left stuck behind item1 forever", item2Done);
    });

    // ---------- Regression: a cancel/pause race must not mislabel a
    // LATER, unrelated item's genuine failure. A real bug: onBackendPaused()
    // never reset m_activeItemCancelled (unlike onBackendFailed(), which
    // does), so cancelItem() setting that flag followed by the backend
    // resolving to Paused instead of Failed (a real, possible race — see
    // forceEmitPaused()'s own comment) left the flag stale. The next
    // item's genuine, unrelated failure would then be misreported as
    // Cancelled with its real error message blanked out. ----------
    auto *raceFake = new FakePausableBackend();
    auto *raceSrcPane = new FilePaneWidget(new LocalBackend());
    auto *raceDestPane = new FilePaneWidget(raceFake);
    // Real LocalBackend, real (guaranteed) failure — a nonexistent source
    // file — so item B's eventual status/errorMessage are genuine, not
    // simulated.
    auto *raceRealFailDestPane = new FilePaneWidget(new LocalBackend());
    auto *raceManager = new TransferManager(&app);
    int raceItemAId = -1, raceItemBId = -1;
    QObject::connect(raceManager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        if (item.fileName == "fakefile.bin") raceItemAId = item.id;
        if (item.fileName == "doesnotexist_race.bin") raceItemBId = item.id;
    });
    raceSrcPane->navigateTo(unblockSrcDir);
    raceRealFailDestPane->navigateTo(unblockDstDir);

    QTimer::singleShot(2700, &app, [&]() {
        raceManager->enqueue(raceSrcPane, raceDestPane, "fakefile.bin");   // dispatches against raceFake
        raceManager->enqueue(raceSrcPane, raceRealFailDestPane, "doesnotexist_race.bin");   // stays Queued
    });

    QTimer::singleShot(2900, &app, [&]() {
        raceManager->cancelItem(raceItemAId);   // sets m_activeItemCancelled, calls raceFake->requestCancel()
        raceFake->forceEmitPaused();   // ...but the backend resolves to Paused, not Failed — the real race
    });

    QTimer::singleShot(3300, &app, [&]() {
        TransferStatus itemBStatus = TransferStatus::Queued;
        QString itemBError;
        for (const TransferItem &it : raceManager->items()) {
            if (it.id == raceItemBId) {
                itemBStatus = it.status;
                itemBError = it.errorMessage;
            }
        }
        qDebug() << "[test] item B (unrelated, genuinely failing) final status ="
                 << static_cast<int>(itemBStatus) << "error =" << itemBError;
        check("a stale cancel flag from an earlier item's pause did NOT mislabel a later, "
              "unrelated item's genuine failure as Cancelled",
              itemBStatus == TransferStatus::Failed);
        check("...and did not blank its real error message either", !itemBError.isEmpty());

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}

#include "transfer_pause_test.moc"
