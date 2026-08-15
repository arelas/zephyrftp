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
#include <QPointer>
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
        while (!condition() && timeoutTimer.isActive()) {
            loop.processEvents(QEventLoop::AllEvents, 20);
            // A plain processEvents() call from this deeply call-stack-
            // nested context (many lambdas deep, never returning to the
            // top-level app.exec() loop) doesn't reliably deliver a
            // pending deleteLater() — the same real gotcha
            // transfer_pause_test.cpp's own waitUntil() already
            // documents and works around. Needed here for scenario 5's
            // pool-teardown check (a QPointer staying non-null despite
            // deleteLater() having been called, purely because this
            // helper never forced the deferred-delete event through).
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    };

    QDir("/tmp/transfer_concurrency_test").removeRecursively();
    QDir().mkpath("/tmp/transfer_concurrency_test/a");
    QDir().mkpath("/tmp/transfer_concurrency_test/b");
    QDir().mkpath("/tmp/transfer_concurrency_test/c");
    QDir().mkpath("/tmp/transfer_concurrency_test/d");
    QDir().mkpath("/tmp/transfer_concurrency_test/pool");

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

    // Scenario 4 setup — a real code-review-found gap: isBackendClaimed()
    // only serializes per-backend-INSTANCE, not per remote server. Two
    // independent backend instances connected to the SAME remote server
    // (routine — both panes are often connected to the same host) could
    // both pass checkExists()==false for the identical destination path
    // and silently clobber each other, since neither instance's own busy
    // check has any way to know about the other. sameServerFake1/2 below
    // are two SEPARATE FakeAsyncBackend instances that happen to report
    // the identical connectionIdentity() ("fake") AND currentPath()
    // ("/fake") — exactly what two real panes connected to the same real
    // server would look like from TransferManager's perspective.
    auto *sameServerFake1 = new FakeAsyncBackend();
    auto *sameServerFake2 = new FakeAsyncBackend();
    auto *sameServerPane1 = new FilePaneWidget(sameServerFake1);
    auto *sameServerPane2 = new FilePaneWidget(sameServerFake2);
    auto *localG = new FilePaneWidget(new LocalBackend());
    auto *localH = new FilePaneWidget(new LocalBackend());
    QDir().mkpath("/tmp/transfer_concurrency_test/g");
    QDir().mkpath("/tmp/transfer_concurrency_test/h");
    localG->navigateTo("/tmp/transfer_concurrency_test/g");
    localH->navigateTo("/tmp/transfer_concurrency_test/h");
    auto *manager4 = new TransferManager(&app);

    int sharedItem1Id = -1, sharedItem2Id = -1;
    TransferStatus sharedItem1Status = TransferStatus::Queued;
    TransferStatus sharedItem2Status = TransferStatus::Queued;
    qint64 sharedItem1BytesDone = 0;

    // Both items below deliberately share the SAME fileName ("shared.bin")
    // — that's the whole point, it's what makes them resolve to the
    // identical destPath — so they can't be told apart by name the way
    // every earlier scenario's tracking does. Distinguished by ORDER
    // instead: enqueue() appends and emits itemAdded synchronously, so
    // the first call below is always seen here before the second.
    QObject::connect(manager4, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        if (sharedItem1Id < 0)
            sharedItem1Id = item.id;
        else if (sharedItem2Id < 0)
            sharedItem2Id = item.id;
    });
    QObject::connect(manager4, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.id == sharedItem1Id) { sharedItem1Status = item.status; sharedItem1BytesDone = item.bytesDone; }
        if (item.id == sharedItem2Id) sharedItem2Status = item.status;
    });

    // Scenario 5 setup — simultaneous-connections pooling (see
    // FilePaneWidget::configureTransferPool()/pickIdleTransferBackend()):
    // ONE pane, configured with a pool of up to 3 connections, should
    // dispatch that many items genuinely in parallel instead of
    // serializing every one through its single primary connection —
    // proving the SAME cross-backend concurrency scenario 1 already
    // proved for two independently-constructed panes, but now driven by
    // ONE pane's own lazily-grown pool instead. Also confirms the
    // reservation-key guard scenario 4 above proved for two independent
    // panes still holds when the "two different backend instances" are
    // instead two pool members of the SAME pane, and that Disconnect
    // (setBackend() swap) tears every pool member down cleanly.
    auto *poolPrimary = new FakeAsyncBackend();
    auto *poolPane = new FilePaneWidget(poolPrimary);
    QList<QPointer<RemoteBackend>> poolMembersBuilt;   // every backend the factory has ever produced, alive or not
    poolPane->configureTransferPool(3, [&poolMembersBuilt]() -> QPair<RemoteBackend *, QThread *> {
        auto *extra = new FakeAsyncBackend();
        poolMembersBuilt.append(extra);
        return {extra, nullptr};   // no real QThread — same no-thread shape every fake backend in this file already uses
    });
    // One shared local source pane — every item below has a distinct
    // filename, so there's no need for a separate pane per item the way
    // scenario 4 above needs one per side of its identical-filename
    // collision.
    auto *localPool = new FilePaneWidget(new LocalBackend());
    localPool->navigateTo("/tmp/transfer_concurrency_test/pool");
    auto *manager5 = new TransferManager(&app);

    int poolItem1Id = -1, poolItem2Id = -1, poolItem3Id = -1, poolItem4Id = -1;
    TransferStatus poolItem1Status = TransferStatus::Queued, poolItem2Status = TransferStatus::Queued,
                   poolItem3Status = TransferStatus::Queued, poolItem4Status = TransferStatus::Queued;
    // "pooldup.bin" is enqueued TWICE, deliberately (see runScenario5()
    // below) — same-filename trick Scenario 4 already uses above, since
    // two items sharing a name can't be told apart BY name. Distinguished
    // by arrival ORDER instead: enqueue() appends and emits itemAdded
    // synchronously, so the first call is always seen here before the
    // second.
    int poolDup1Id = -1, poolDup2Id = -1;
    TransferStatus poolDup1Status = TransferStatus::Queued, poolDup2Status = TransferStatus::Queued;

    QObject::connect(manager5, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        if (item.fileName == "pool1.bin") poolItem1Id = item.id;
        else if (item.fileName == "pool2.bin") poolItem2Id = item.id;
        else if (item.fileName == "pool3.bin") poolItem3Id = item.id;
        else if (item.fileName == "pool4.bin") poolItem4Id = item.id;
        else if (item.fileName == "pooldup.bin") {
            if (poolDup1Id < 0)
                poolDup1Id = item.id;
            else if (poolDup2Id < 0)
                poolDup2Id = item.id;
        }
    });
    QObject::connect(manager5, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.id == poolItem1Id) poolItem1Status = item.status;
        else if (item.id == poolItem2Id) poolItem2Status = item.status;
        else if (item.id == poolItem3Id) poolItem3Status = item.status;
        else if (item.id == poolItem4Id) poolItem4Status = item.status;
        else if (item.id == poolDup1Id) poolDup1Status = item.status;
        else if (item.id == poolDup2Id) poolDup2Status = item.status;
    });

    // ---------- Scenario 5: simultaneous-connections pooling — real
    // N-way parallelism through ONE pane's lazily-grown pool, pool-member
    // reuse (not unbounded growth), the reservation-key guard still
    // holding across pool members, and clean teardown on disconnect.
    // Chained last since it's the newest addition, not because it's
    // lower-priority than the others. ----------
    auto runScenario5 = [&]() {
        // ---- Phase A: 3 distinct-destination items against a pool
        // capped at 3 should all reach InProgress at the same time —
        // the same cross-backend concurrency scenario 1 proved for two
        // independently-constructed panes, now proved for ONE pane's
        // own pool instead. ----
        manager5->enqueue(localPool, poolPane, "pool1.bin");
        manager5->enqueue(localPool, poolPane, "pool2.bin");
        manager5->enqueue(localPool, poolPane, "pool3.bin");

        waitUntil([&]() {
            return poolItem1Status == TransferStatus::InProgress
                && poolItem2Status == TransferStatus::InProgress
                && poolItem3Status == TransferStatus::InProgress;
        });
        check("scenario 5: 3 items against a pool capped at 3 all reached InProgress at the same time",
              poolItem1Status == TransferStatus::InProgress
              && poolItem2Status == TransferStatus::InProgress
              && poolItem3Status == TransferStatus::InProgress);
        // The primary (poolPrimary) covers the first; growing to 3 total
        // needs exactly 2 more built via the factory — not 3 (the
        // primary isn't a factory product) and not fewer (all 3 needed
        // to be busy at once for the check just above to hold).
        check("scenario 5: reaching 3 simultaneous connections grew the pool by exactly 2 (the primary "
              "already counts as the first)",
              poolMembersBuilt.size() == 2);

        waitUntil([&]() {
            return poolItem1Status == TransferStatus::Done && poolItem2Status == TransferStatus::Done
                && poolItem3Status == TransferStatus::Done;
        }, 3000);
        check("scenario 5: all 3 pooled items completed normally",
              poolItem1Status == TransferStatus::Done && poolItem2Status == TransferStatus::Done
              && poolItem3Status == TransferStatus::Done);

        // ---- Phase B: with the pool now idle again, one more item
        // should reuse an existing member rather than growing further. ----
        manager5->enqueue(localPool, poolPane, "pool4.bin");
        waitUntil([&]() { return poolItem4Status == TransferStatus::InProgress; });
        check("scenario 5: a 4th item, enqueued once the pool was idle again, started immediately "
              "(reusing an existing member)", poolItem4Status == TransferStatus::InProgress);
        check("scenario 5: reusing an idle pool member didn't grow the pool any further",
              poolMembersBuilt.size() == 2);
        waitUntil([&]() { return poolItem4Status == TransferStatus::Done; }, 3000);

        // ---- Phase C: the SAME destination path, enqueued twice, must
        // still serialize — proving destinationReservationKey() (already
        // keyed on connectionIdentity(), not a specific backend pointer;
        // see scenario 4 above) holds just as well when the "two
        // different backend instances" are two pool members of the SAME
        // pane, not two independently-connected panes. ----
        manager5->enqueue(localPool, poolPane, "pooldup.bin");
        manager5->enqueue(localPool, poolPane, "pooldup.bin");

        waitUntil([&]() { return poolDup1Status == TransferStatus::InProgress; });
        check("scenario 5: the first of two identical-destination items reached InProgress",
              poolDup1Status == TransferStatus::InProgress);
        check("scenario 5: the second — same destination, freely available OTHER pool members "
              "notwithstanding — stayed Queued instead of racing it",
              poolDup2Status == TransferStatus::Queued);

        waitUntil([&]() { return poolDup1Status == TransferStatus::Done; }, 3000);
        waitUntil([&]() {
            return poolDup2Status == TransferStatus::InProgress || poolDup2Status == TransferStatus::Done;
        }, 3000);
        check("scenario 5: the second identical-destination item started only once the first released "
              "the shared destination — not permanently stuck",
              poolDup2Status == TransferStatus::InProgress || poolDup2Status == TransferStatus::Done);
        waitUntil([&]() { return poolDup2Status == TransferStatus::Done; }, 3000);
        check("scenario 5: the second identical-destination item also completed normally",
              poolDup2Status == TransferStatus::Done);
        check("scenario 5: still exactly 2 factory-built pool members after the whole scenario — "
              "never grew past the configured cap of 3 total", poolMembersBuilt.size() == 2);

        // ---- Phase D: Disconnect (a primary-backend swap, same as any
        // real Connect/Disconnect) must tear down every pool member
        // cleanly — no crash, no hang, no dangling thread. Each fake
        // pool backend was built with thread=nullptr (parented to
        // poolPane per setBackend()'s own doc comment), so a clean
        // Qt-level deleteLater() teardown is what "cleanly" means to
        // verify here; a real SftpBackend/FtpBackend's own thread
        // teardown is already covered generically by every OTHER
        // scenario in this file exercising ordinary primary-backend
        // replacement, not something pooling changes the shape of.
        QList<QPointer<RemoteBackend>> membersBeforeDisconnect = poolMembersBuilt;
        poolPane->setBackend(new LocalBackend(), nullptr);
        // deleteLater() events need an event-loop turn to actually run.
        waitUntil([&]() {
            for (const QPointer<RemoteBackend> &member : membersBeforeDisconnect) {
                if (member)
                    return false;
            }
            return true;
        }, 3000);
        bool allPoolMembersGone = true;
        for (const QPointer<RemoteBackend> &member : membersBeforeDisconnect) {
            if (member)
                allPoolMembersGone = false;
        }
        check("scenario 5: Disconnect (primary backend swap) tore down every pool member "
              "built during this scenario", allPoolMembersGone);

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    };

    // ---------- Scenario 4: two DIFFERENT backend instances of the SAME
    // simulated server, both targeting the identical destination path,
    // must not run concurrently ----------
    auto runScenario4 = [&]() {
        // Deliberately the SAME filename via joinPath(currentDirectory(),
        // fileName) against two panes whose currentPath() both return the
        // fixed "/fake" — both items resolve to the identical destPath,
        // matching two real panes connected to the same real server and
        // uploading to the same remote directory.
        manager4->enqueue(localG, sameServerPane1, "shared.bin");
        manager4->enqueue(localH, sameServerPane2, "shared.bin");

        waitUntil([&]() {
            return sharedItem1Status == TransferStatus::InProgress && sharedItem1BytesDone > 0;
        });
        check("scenario 4: the first item claiming the shared destination reached InProgress",
              sharedItem1Status == TransferStatus::InProgress);
        // Deterministic, not a timing guess: startNext() claims item 1
        // (reserving the shared destination key) and evaluates item 2 in
        // the very SAME synchronous scan — pre-fix, item 2's own
        // checkExists() would already be in flight (or further) by now,
        // since isBackendClaimed() alone can't see these two DIFFERENT
        // backend instances as conflicting at all.
        check("scenario 4: the second item — same destination path, a DIFFERENT backend "
              "instance of the same simulated server — stayed Queued instead of racing it",
              sharedItem2Status == TransferStatus::Queued);

        waitUntil([&]() { return sharedItem1Status == TransferStatus::Done; }, 3000);
        check("scenario 4: the first item completed normally", sharedItem1Status == TransferStatus::Done);
        waitUntil([&]() {
            return sharedItem2Status == TransferStatus::InProgress || sharedItem2Status == TransferStatus::Done;
        }, 3000);
        check("scenario 4: the second item started only once the first released the shared "
              "destination — not permanently stuck",
              sharedItem2Status == TransferStatus::InProgress || sharedItem2Status == TransferStatus::Done);
        waitUntil([&]() { return sharedItem2Status == TransferStatus::Done; }, 3000);
        check("scenario 4: the second item also completed normally", sharedItem2Status == TransferStatus::Done);

        runScenario5();
    };

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

        runScenario4();
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
