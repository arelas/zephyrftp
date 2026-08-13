// Proves TransferManager's per-backend-instance concurrency (added
// alongside this test): two items whose executors are DIFFERENT backend
// instances now run genuinely at the same time — both InProgress, both
// making progress, in the same short window — which was structurally
// impossible under the old design (a single m_activeIndex/m_currentBackend
// serving exactly one active item globally, regardless of which backend(s)
// were actually involved). Two items that share the SAME backend instance
// still serialize strictly, which is the real safety invariant this
// change must not weaken (SftpBackend/FtpBackend each hold one
// non-thread-safe session/control connection). Also confirms cancelItem()
// on one of two concurrently-active items resolves only that one, leaving
// the other to keep running unaffected.
//
// This proves TransferManager's SCHEDULING now allows concurrent
// InProgress items across distinct backend instances — it does NOT
// exercise real parallel network I/O (that would need two live SFTP/FTP
// servers, unavailable here, same live-server boundary
// transfer_pause_test.cpp's own header comment already flags for
// SftpBackend's real byte-offset resume logic).
//
// Uses a small FakeAsyncBackend — same QTimer-tick technique
// transfer_pause_test.cpp's FakePausableBackend already established
// (genuinely asynchronous, not a single synchronous call, so there's a
// real window to observe two items progressing at once) — rather than
// queue_persistence_test.cpp's/remote_to_remote_test.cpp's
// FakeRemoteBackend, which resolves downloadFile()/uploadFile()
// synchronously inside the call itself and so could never demonstrate
// real overlap.
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <functional>
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"
#include "backends/RemoteBackend.h"
#include "transfer/TransferManager.h"

namespace {

class FakeAsyncBackend : public RemoteBackend {
    Q_OBJECT
public:
    explicit FakeAsyncBackend(QObject *parent = nullptr) : RemoteBackend(parent) {
        connect(&m_timer, &QTimer::timeout, this, &FakeAsyncBackend::tick);
    }

    QString currentPath() const override { return QStringLiteral("/fake"); }
    bool isLocalFilesystem() const override { return false; }
    QString connectionIdentity() const override { return QStringLiteral("fake"); }
    void requestCancel() override { m_cancelRequested = true; }
    void requestPause() override { }   // not exercised by this test

    // Test hook for scenario 3's reconnect-during-conflict-check
    // regression below — when true, checkExists() stashes the request
    // instead of answering immediately, giving the test a real window to
    // swap a pane's backend out from under an already-claimed item before
    // its conflict check ever resolves, the same async gap a real
    // Disconnect+Connect during a real network round trip could land in.
    bool holdCheckExists = false;
    void releaseHeldCheckExists() {
        if (m_pendingCheckExistsRequestId >= 0)
            emit existsChecked(m_pendingCheckExistsPath, false, false, m_pendingCheckExistsRequestId);
        m_pendingCheckExistsRequestId = -1;
    }
    bool hasPendingCheckExists() const { return m_pendingCheckExistsRequestId >= 0; }

public slots:
    void connectToHost() override { emit connected(); }
    void listDirectory(const QString &path) override { emit directoryListed(path, {}); }
    void downloadFile(const QString &remotePath, const QString &localPath, qint64 resumeOffset = 0) override {
        Q_UNUSED(localPath);
        start(remotePath, resumeOffset);
    }
    void uploadFile(const QString &localPath, const QString &remotePath, qint64 resumeOffset = 0) override {
        Q_UNUSED(localPath);
        start(remotePath, resumeOffset);
    }
    // Not exercised here — this test is about scheduling concurrency, not
    // file management — but RemoteBackend is a pure interface, so a
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
    // Must actually respond (unless deliberately held — see
    // holdCheckExists above) — startNext() always issues a checkExists()
    // call before dispatching a transfer and waits for existsChecked()
    // before proceeding; a no-op stub would hang every scenario below.
    void checkExists(const QString &path, int requestId) override {
        if (holdCheckExists) {
            m_pendingCheckExistsPath = path;
            m_pendingCheckExistsRequestId = requestId;
            return;
        }
        emit existsChecked(path, false, false, requestId);
    }

private slots:
    void tick() {
        if (m_cancelRequested) {
            m_timer.stop();
            emit transferFailed(m_name, QStringLiteral("Cancelled"));
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
        m_name = name;
        m_done = resumeOffset;
        m_total = 1000;
        m_timer.start(30);   // ~30ms per 100-byte chunk, same pacing as FakePausableBackend
    }

    QTimer m_timer;
    QString m_name;
    qint64 m_done = 0;
    qint64 m_total = 0;
    bool m_cancelRequested = false;
    QString m_pendingCheckExistsPath;
    int m_pendingCheckExistsRequestId = -1;
};

}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    // Same polling-with-timeout technique transfer_pause_test.cpp/
    // navigation-test.cpp already established — avoids trusting a fixed
    // wall-clock delay to have been enough on a slower CI runner.
    auto waitUntil = [&](std::function<bool()> condition, int timeoutMs = 5000) {
        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeoutTimer.start(timeoutMs);
        while (!condition() && timeoutTimer.isActive())
            loop.processEvents(QEventLoop::AllEvents, 20);
    };

    QDir("/tmp/transfer_concurrency_test").removeRecursively();
    QDir().mkpath("/tmp/transfer_concurrency_test/a");
    QDir().mkpath("/tmp/transfer_concurrency_test/b");
    QDir().mkpath("/tmp/transfer_concurrency_test/c");
    QDir().mkpath("/tmp/transfer_concurrency_test/d");

    // ---------- Scenario 1: cross-backend concurrency + same-backend
    // serialization ----------
    auto *fakeLeft = new FakeAsyncBackend();
    auto *fakeRight = new FakeAsyncBackend();
    auto *localA = new FilePaneWidget(new LocalBackend());
    auto *localB = new FilePaneWidget(new LocalBackend());
    auto *fakeLeftPane = new FilePaneWidget(fakeLeft);
    auto *fakeRightPane = new FilePaneWidget(fakeRight);
    localA->navigateTo("/tmp/transfer_concurrency_test/a");
    localB->navigateTo("/tmp/transfer_concurrency_test/b");
    auto *manager = new TransferManager(&app);

    int item1Id = -1, item2Id = -1, item3Id = -1;
    qint64 item1BytesDone = 0, item2BytesDone = 0;
    TransferStatus item1Status = TransferStatus::Queued;
    TransferStatus item2Status = TransferStatus::Queued;
    TransferStatus item3Status = TransferStatus::Queued;

    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        if (item.fileName == "file1.bin") item1Id = item.id;
        if (item.fileName == "file2.bin") item2Id = item.id;
        if (item.fileName == "file3.bin") item3Id = item.id;
    });
    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.id == item1Id) { item1Status = item.status; item1BytesDone = item.bytesDone; }
        if (item.id == item2Id) { item2Status = item.status; item2BytesDone = item.bytesDone; }
        if (item.id == item3Id) { item3Status = item.status; }
    });

    // Scenario 2 setup (constructed up front; run chained directly after
    // scenario 1 finishes below — NOT via its own independent fixed-delay
    // QTimer::singleShot(). An earlier version of this test used
    // independent absolute-time timers for each scenario/the final report,
    // and it was a real bug: under a genuine regression (confirmed by
    // deliberately testing this file against the OLD pre-concurrency
    // TransferManager), scenario 1's waitUntil() calls legitimately run
    // out their full timeout instead of returning quickly, which can
    // outlast a later independent timer's fixed delay — that later timer
    // then fires REENTRANTLY from inside scenario 1's still-running
    // waitUntil() (which pumps ALL events, including unrelated timers),
    // printing a premature result and calling app.exit() while checks
    // were still in flight, observed directly as "[test] ALL PASS"
    // printed before several genuine [FAIL] lines, with exit code 0.
    // Chaining eliminates the whole class of bug: each stage only ever
    // starts once the previous one has actually finished, on any timing.
    auto *fakeCancelTarget = new FakeAsyncBackend();
    auto *fakeOther = new FakeAsyncBackend();
    auto *localC = new FilePaneWidget(new LocalBackend());
    auto *localD = new FilePaneWidget(new LocalBackend());
    auto *cancelTargetPane = new FilePaneWidget(fakeCancelTarget);
    auto *otherPane = new FilePaneWidget(fakeOther);
    localC->navigateTo("/tmp/transfer_concurrency_test/c");
    localD->navigateTo("/tmp/transfer_concurrency_test/d");
    auto *manager2 = new TransferManager(&app);

    int cancelItemId = -1, otherItemId = -1;
    TransferStatus cancelItemStatus = TransferStatus::Queued;
    TransferStatus otherItemStatus = TransferStatus::Queued;
    qint64 otherItemBytesDone = 0;

    QObject::connect(manager2, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        if (item.fileName == "cancelme.bin") cancelItemId = item.id;
        if (item.fileName == "keepgoing.bin") otherItemId = item.id;
    });
    QObject::connect(manager2, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.id == cancelItemId) cancelItemStatus = item.status;
        if (item.id == otherItemId) { otherItemStatus = item.status; otherItemBytesDone = item.bytesDone; }
    });

    // Scenario 3 setup — a real code-review-found bug: ActiveTransfer::
    // claimedBackends (captured at claim time in startNext(), from
    // item.sourcePane->backend()/item.destPane->backend()) could desync
    // from currentExecutor (captured separately, later, in
    // dispatchActiveItem() via a FRESH re-fetch of the same calls) if a
    // pane's backend is swapped (Disconnect/Connect) during the async gap
    // between them. swappableFake1's holdCheckExists lets this test land
    // deliberately inside that exact gap, the same one a real network
    // round trip's own latency could land in.
    auto *swappableFake1 = new FakeAsyncBackend();
    auto *swappableFake2 = new FakeAsyncBackend();
    auto *swappablePane = new FilePaneWidget(swappableFake1);
    auto *localE = new FilePaneWidget(new LocalBackend());
    auto *localF = new FilePaneWidget(new LocalBackend());
    QDir().mkpath("/tmp/transfer_concurrency_test/e");
    QDir().mkpath("/tmp/transfer_concurrency_test/f");
    localE->navigateTo("/tmp/transfer_concurrency_test/e");
    localF->navigateTo("/tmp/transfer_concurrency_test/f");
    auto *manager3 = new TransferManager(&app);

    int targetItemId = -1, otherSwapItemId = -1;
    TransferStatus targetItemStatus = TransferStatus::Queued;
    TransferStatus otherSwapItemStatus = TransferStatus::Queued;

    QObject::connect(manager3, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        if (item.fileName == "target.bin") targetItemId = item.id;
        if (item.fileName == "other.bin") otherSwapItemId = item.id;
    });
    QObject::connect(manager3, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.id == targetItemId) targetItemStatus = item.status;
        if (item.id == otherSwapItemId) otherSwapItemStatus = item.status;
    });

    // ---------- Scenario 3: a pane's backend swapped mid-conflict-check
    // must not leave claimedBackends pointing at the old backend ----------
    auto runScenario3 = [&]() {
        swappableFake1->holdCheckExists = true;
        manager3->enqueue(localE, swappablePane, "target.bin");   // claims swappableFake1, checkExists() held

        waitUntil([&]() { return swappableFake1->hasPendingCheckExists(); });

        // Simulates Disconnect+Connect landing exactly inside the async
        // conflict-check gap — nothing in FilePaneWidget::setBackend()'s
        // real callers guards against this either.
        swappablePane->setBackend(swappableFake2, nullptr);
        swappableFake1->releaseHeldCheckExists();   // resolves against the OLD backend's own held request

        waitUntil([&]() {
            return targetItemStatus == TransferStatus::InProgress;
        });
        check("scenario 3: the item genuinely dispatched against the NEW backend after the swap",
              targetItemStatus == TransferStatus::InProgress);

        // A second, unrelated item targeting the pane's CURRENT (new)
        // backend, enqueued while the first is still actively running on
        // it. Pre-fix, claimedBackends still holds the OLD backend, so
        // isBackendClaimed(swappableFake2) wrongly reports it free and
        // this second item dispatches concurrently too — a genuine
        // double-dispatch on one backend instance, the exact invariant
        // this whole rewrite exists to enforce. Post-fix, it correctly
        // stays Queued until the first item is done with swappableFake2.
        manager3->enqueue(localF, swappablePane, "other.bin");

        waitUntil([&]() {
            return otherSwapItemStatus == TransferStatus::InProgress || targetItemStatus == TransferStatus::Done;
        }, 2000);
        check("scenario 3: the second item was never InProgress AT THE SAME TIME as the first "
              "on the SAME (new) backend instance — no double-dispatch",
              !(otherSwapItemStatus == TransferStatus::InProgress && targetItemStatus == TransferStatus::InProgress));

        waitUntil([&]() { return targetItemStatus == TransferStatus::Done; }, 3000);
        check("scenario 3: the first item still completes normally", targetItemStatus == TransferStatus::Done);
        waitUntil([&]() {
            return otherSwapItemStatus == TransferStatus::InProgress || otherSwapItemStatus == TransferStatus::Done;
        }, 3000);
        check("scenario 3: the second item started only after the first was done with the new backend "
              "— not permanently stuck",
              otherSwapItemStatus == TransferStatus::InProgress || otherSwapItemStatus == TransferStatus::Done);

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    };

    // ---------- Scenario 2: cancelling one of two concurrently-active
    // items affects only that item, then chains directly into scenario 3 ----------
    auto runScenario2 = [&]() {
        manager2->enqueue(localC, cancelTargetPane, "cancelme.bin");
        manager2->enqueue(localD, otherPane, "keepgoing.bin");

        waitUntil([&]() {
            return cancelItemStatus == TransferStatus::InProgress
                && otherItemStatus == TransferStatus::InProgress && otherItemBytesDone > 0;
        });
        manager2->cancelItem(cancelItemId);

        waitUntil([&]() { return cancelItemStatus == TransferStatus::Cancelled; });
        check("cancelling one concurrently-active item resolves it to Cancelled",
              cancelItemStatus == TransferStatus::Cancelled);

        waitUntil([&]() { return otherItemStatus == TransferStatus::Done; }, 3000);
        check("the OTHER concurrently-active item (a different backend) was unaffected "
              "and ran to Done on its own", otherItemStatus == TransferStatus::Done);

        runScenario3();
    };

    // ---------- Scenario 1: cross-backend concurrency + same-backend
    // serialization, then chains directly into scenario 2 ----------
    QTimer::singleShot(100, &app, [&]() {
        manager->enqueue(localA, fakeLeftPane, "file1.bin");
        manager->enqueue(localB, fakeRightPane, "file2.bin");
        manager->enqueue(localA, fakeLeftPane, "file3.bin");   // same executor (fakeLeft) as item1

        // item1 (fakeLeft) and item2 (fakeRight) should BOTH be InProgress
        // with real, nonzero progress at the same moment — impossible
        // under the old single-active-item design, where item2 (a
        // completely different backend) would have stayed Queued until
        // item1 finished, ~300ms later.
        waitUntil([&]() {
            return item1Status == TransferStatus::InProgress && item1BytesDone > 0
                && item2Status == TransferStatus::InProgress && item2BytesDone > 0;
        });
        check("cross-backend: item1 (fakeLeft) reached InProgress with real progress",
              item1Status == TransferStatus::InProgress && item1BytesDone > 0);
        check("cross-backend: item2 (fakeRight) reached InProgress with real progress AT THE SAME TIME",
              item2Status == TransferStatus::InProgress && item2BytesDone > 0);

        // item3 shares fakeLeft with item1, which is still running right
        // now — must still be strictly Queued, not claimed/InProgress.
        check("same-backend: item3 (also fakeLeft) stayed Queued while item1 was still InProgress on fakeLeft",
              item3Status == TransferStatus::Queued);

        // Let item1 run to completion, then confirm item3 (same backend)
        // picks up afterward — proving it was genuinely serialized behind
        // item1, not permanently stuck.
        waitUntil([&]() { return item1Status == TransferStatus::Done; }, 3000);
        check("same-backend: item1 (fakeLeft) reached Done", item1Status == TransferStatus::Done);
        waitUntil([&]() {
            return item3Status == TransferStatus::InProgress || item3Status == TransferStatus::Done;
        }, 3000);
        check("same-backend: item3 started only after item1 (its own backend) had finished",
              item3Status == TransferStatus::InProgress || item3Status == TransferStatus::Done);

        runScenario2();
    });

    return app.exec();
}

#include "transfer_concurrency_test.moc"
