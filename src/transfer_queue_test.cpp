// Headless functional test of the transfer queue. Not a unit test of one
// class in isolation — this exercises the real path: two FilePaneWidgets
// each backed by a real LocalBackend, a real TransferManager, real files
// on disk.
//
// Three phases:
//   1. A real transfer completes: file moves, hash matches, Queued ->
//      InProgress -> Done with plausible progress numbers.
//   2. Cancelling a QUEUED (not yet started) item. Exploits a real,
//      deterministic property of TransferManager rather than racing a
//      timer: enqueue() marks the new item InProgress synchronously if
//      nothing else is running, or leaves it Queued (also synchronously)
//      if something already is — so two enqueue() calls in a row, with
//      nothing else running between them, reliably produce one InProgress
//      item and one Queued item with no timing window to miss.
//   3. Retrying a FAILED item actually re-runs the transfer (proven by
//      pointing at a nonexistent source file, so both the original attempt
//      and the retry fail deterministically — this tests that retryItem()
//      genuinely re-triggers execution, not just resets a status field).
//
// UNVERIFIED — flagged, not silently skipped: this only exercises
// LocalBackend's requestCancel(), which is a documented no-op (QFile::copy
// can't be interrupted mid-call). SftpBackend's actual mid-transfer
// interruption (the cancel flag checked inside the libssh2 read/write
// loops) has no automated coverage — it needs a real SFTP server slow
// enough to catch mid-transfer, which isn't available here.
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QEventLoop>
#include <QElapsedTimer>
#include "ui/FilePaneWidget.h"
#include "ui/TransferQueueWidget.h"
#include "ui/TransferQueueTable.h"
#include "ui/IconTheme.h"
#include "backends/LocalBackend.h"
#include "transfer/TransferManager.h"

namespace {
QString fileHash(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Md5).toHex();
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const QString srcDir = "/tmp/transfer_test/src_dir";
    const QString dstDir = "/tmp/transfer_test/dst_dir";
    const QString fileName = "testfile.bin";

    // Start every run from a clean slate. A previous run's testfile.bin
    // (or testfile2.bin/doesnotexist.bin's requeue side effects) left
    // sitting at the destination would otherwise make TransferManager
    // correctly detect a real destination conflict and pop a real,
    // unanswered QMessageBox — the exact same class of bug
    // folder_transfer_test.cpp had (see its own matching comment) — and
    // this test used to depend entirely on whoever ran it remembering an
    // undocumented-in-code `rm -rf` first. Generating testfile.bin's
    // content here too (rather than requiring it be created externally
    // beforehand, as CONTRIBUTING.md used to instruct) means this test
    // no longer depends on anything set up outside this binary.
    QDir("/tmp/transfer_test").removeRecursively();
    QDir().mkpath(srcDir);
    QDir().mkpath(dstDir);
    {
        QFile f(srcDir + "/" + fileName);
        f.open(QIODevice::WriteOnly);
        QByteArray randomData(500000, Qt::Uninitialized);
        QRandomGenerator::global()->fillRange(
            reinterpret_cast<quint32 *>(randomData.data()), randomData.size() / sizeof(quint32));
        f.write(randomData);
    }
    const QString srcHashBefore = fileHash(srcDir + "/" + fileName);

    // Second small file for the cancel-while-queued phase.
    {
        QFile f(srcDir + "/testfile2.bin");
        f.open(QIODevice::WriteOnly);
        f.write(QByteArray(1000, 'x'));
    }

    auto *leftPane = new FilePaneWidget(new LocalBackend());
    auto *rightPane = new FilePaneWidget(new LocalBackend());
    auto *manager = new TransferManager(&app);

    // ---- Phase 1 tracking ----
    bool sawQueued = false, sawInProgress = false, sawDone = false;
    bool sawPlausibleProgress = false;
    bool phase1Pass = false;

    // ---- Phase 2 tracking ----
    int cancelTargetId = -1;
    bool phase2SyncPass = false;
    bool phase2Pass = false;

    // ---- Phase 3 tracking ----
    int retryTargetId = -1;
    int retryFailedCount = 0;
    bool sawRetryRequeued = false;
    bool phase3Pass = false;

    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        qDebug() << "[test] itemAdded: status =" << static_cast<int>(item.status)
                 << "src =" << item.sourcePath << "dst =" << item.destPath;
        if (item.status == TransferStatus::Queued)
            sawQueued = true;
        if (item.fileName == "testfile2.bin")
            cancelTargetId = item.id;
        if (item.fileName == "doesnotexist.bin")
            retryTargetId = item.id;
    });

    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        qDebug() << "[test] itemUpdated: id =" << item.id << "status =" << static_cast<int>(item.status)
                 << "bytesDone =" << item.bytesDone << "bytesTotal =" << item.bytesTotal;
        if (item.status == TransferStatus::InProgress) {
            sawInProgress = true;
            if (item.bytesTotal == 500000 && item.bytesDone <= item.bytesTotal)
                sawPlausibleProgress = true;
        }
        if (item.status == TransferStatus::Done)
            sawDone = true;

        if (item.id == retryTargetId) {
            if (item.status == TransferStatus::Failed)
                retryFailedCount++;
            if (item.status == TransferStatus::Queued && retryFailedCount == 1)
                sawRetryRequeued = true;   // the re-queue specifically happened AFTER the first failure
        }
    });

    leftPane->navigateTo(srcDir);
    rightPane->navigateTo(dstDir);

    // ---- Phase 1: real transfer ----
    QTimer::singleShot(300, &app, [&]() {
        manager->enqueue(leftPane, rightPane, fileName);
    });

    QTimer::singleShot(1200, &app, [&]() {
        const QString dstPath = dstDir + "/" + fileName;
        const bool fileExists = QFile::exists(dstPath);
        const QString dstHash = fileExists ? fileHash(dstPath) : QString();
        const bool hashMatches = fileExists && (dstHash == srcHashBefore);

        qDebug() << "[phase1] dest file exists:" << fileExists;
        qDebug() << "[phase1] hash matches source:" << hashMatches;
        qDebug() << "[phase1] saw Queued->InProgress->Done:" << sawQueued << sawInProgress << sawDone;
        qDebug() << "[phase1] saw plausible progress numbers:" << sawPlausibleProgress;

        const bool pass = fileExists && hashMatches && sawQueued && sawInProgress && sawDone && sawPlausibleProgress;
        phase1Pass = pass;
        qDebug() << (pass ? "[phase1] PASS" : "[phase1] FAIL");
    });

    // ---- Phase 2: cancel a queued (not-yet-started) item ----
    QTimer::singleShot(1400, &app, [&]() {
        QFile::remove(dstDir + "/testfile2.bin");   // clean slate in case a prior run left it
        // testfile.bin already exists at this destination from phase 1 —
        // remove it first so re-queueing the same transfer doesn't hit
        // the (correct, intentional) new destination-conflict prompt
        // this test isn't set up to handle. Conflict resolution has its
        // own dedicated coverage; this phase is specifically about
        // cancelling a queued item, not about overwrite behavior.
        QFile::remove(dstDir + "/testfile.bin");

        // Back-to-back, synchronous, no event-loop turn between them: the
        // first becomes InProgress immediately (nothing else running),
        // the second is left Queued immediately (guard in startNext()).
        manager->enqueue(leftPane, rightPane, "testfile.bin");
        manager->enqueue(leftPane, rightPane, "testfile2.bin");

        bool cancelledSynchronously = false;
        manager->cancelItem(cancelTargetId);
        for (const TransferItem &it : manager->items()) {
            if (it.id == cancelTargetId && it.status == TransferStatus::Cancelled)
                cancelledSynchronously = true;
        }
        qDebug() << "[phase2] queued item cancelled synchronously, before any event loop turn:"
                 << cancelledSynchronously;
        phase2SyncPass = cancelledSynchronously;
        qDebug() << (cancelledSynchronously ? "[phase2-sync] PASS" : "[phase2-sync] FAIL");
    });

    QTimer::singleShot(2200, &app, [&]() {
        const bool neverCopied = !QFile::exists(dstDir + "/testfile2.bin");
        bool finalStatusStillCancelled = false;
        for (const TransferItem &it : manager->items()) {
            if (it.id == cancelTargetId && it.status == TransferStatus::Cancelled)
                finalStatusStillCancelled = true;
        }
        qDebug() << "[phase2] testfile2.bin never actually copied:" << neverCopied;
        qDebug() << "[phase2] status still Cancelled (wasn't silently resumed):" << finalStatusStillCancelled;
        const bool pass = neverCopied && finalStatusStillCancelled;
        phase2Pass = pass;
        qDebug() << (pass ? "[phase2] PASS" : "[phase2] FAIL");
    });

    // ---- Phase 3: retry a failed item ----
    QTimer::singleShot(2400, &app, [&]() {
        manager->enqueue(leftPane, rightPane, "doesnotexist.bin");   // guaranteed to fail — source doesn't exist
    });

    QTimer::singleShot(3000, &app, [&]() {
        qDebug() << "[phase3] failed once before retry:" << (retryFailedCount == 1);
        manager->retryItem(retryTargetId);
    });

    QTimer::singleShot(3600, &app, [&]() {
        qDebug() << "[phase3] was actually re-queued after the first failure:" << sawRetryRequeued;
        qDebug() << "[phase3] failed again after retry (proves retry re-ran, not just reset a flag):"
                 << (retryFailedCount == 2);
        const bool pass = sawRetryRequeued && (retryFailedCount == 2);
        phase3Pass = pass;
        qDebug() << (pass ? "[phase3] PASS" : "[phase3] FAIL");

        qDebug() << (phase3Pass ? "[phase3] PASS" : "[phase3] FAIL");
    });

    // ---- Phase 4: TransferQueueWidget keeps a new item in its correct
    // sorted position, not always appended at the bottom. Regression test
    // for a real bug: onItemAdded() always called insertRow(rowCount()),
    // ignoring whatever column sort was currently active — sorting the
    // queue by Name, then enqueueing something, silently broke the sort
    // the person had just set. A fresh TransferManager/TransferQueueWidget
    // pair, isolated from phases 1-3's items — this only checks how new
    // rows land relative to an active sort, not transfer execution, so
    // the enqueued items never need to actually run (checked synchronously,
    // before any event-loop turn lets startNext() begin real I/O). All
    // three items stay Queued throughout, so they all live in the ACTIVE
    // tab specifically — see TransferQueueTable.h's QueueCategory. ----
    bool phase4Pass = false;
    QTimer::singleShot(3800, &app, [&]() {
        auto *sortManager = new TransferManager(&app);
        auto *queueWidget = new TransferQueueWidget(sortManager);
        auto *activeContainer = queueWidget->findChild<TransferQueueTable *>(QStringLiteral("activeQueueTable"));
        auto *table = activeContainer->findChild<QTableWidget *>();

        sortManager->enqueue(leftPane, rightPane, "b_item.bin");
        sortManager->enqueue(leftPane, rightPane, "a_item.bin");

        // Same call Qt's own header-click handling invokes — sorts
        // ascending by Name (column 0).
        table->horizontalHeader()->sectionClicked(0);

        const bool sortedAfterHeaderClick =
            table->item(0, 0)->text() == "a_item.bin" && table->item(1, 0)->text() == "b_item.bin";
        qDebug() << "[phase4] sorted ascending by Name after header click:" << sortedAfterHeaderClick;

        // The regression check: enqueue a third item that belongs
        // ALPHABETICALLY BETWEEN the first two, while the sort is still
        // active. Under the old bug this always landed at row 2 (the
        // bottom) regardless of the active sort, producing
        // a_item/b_item/ab_item instead of the correctly sorted
        // a_item/ab_item/b_item.
        sortManager->enqueue(leftPane, rightPane, "ab_item.bin");

        // TransferQueueWidget::onItemAdded() now DEFERS its rebuild-while-
        // sorted path via QTimer::singleShot(0, ...) instead of rebuilding
        // synchronously — a real fix for a severe O(N²) bug (enqueueing N
        // items while sorted used to rebuild the whole table N times,
        // hanging the GUI thread and consuming runaway memory on a large
        // batch transfer; see that method's own comment). One
        // processEvents() call lets that queued zero-delay callback
        // actually run before this checks the table's contents.
        qApp->processEvents();

        const bool stillSortedAfterNewItem = table->rowCount() == 3
            && table->item(0, 0)->text() == "a_item.bin"
            && table->item(1, 0)->text() == "ab_item.bin"
            && table->item(2, 0)->text() == "b_item.bin";
        qDebug() << "[phase4] new item landed in its correct sorted position, not appended at the bottom:"
                  << stillSortedAfterNewItem;

        phase4Pass = sortedAfterHeaderClick && stillSortedAfterNewItem;
        qDebug() << (phase4Pass ? "[phase4] PASS" : "[phase4] FAIL");
    });

    // ---- Phase 5: TransferQueueWidget's per-row visual indicators must
    // be internally consistent and reflect the real sort key shown on
    // screen — four real bugs found by code review, fixed together since
    // they're all in the same small handful of functions. Fresh
    // TransferManager/TransferQueueWidget pair, isolated from phases 1-4. ----
    bool phase5Pass = false;
    QTimer::singleShot(4200, &app, [&]() {
        auto *visManager = new TransferManager(&app);
        auto *visWidget = new TransferQueueWidget(visManager);
        // Both items captured below are InProgress (hence in the ACTIVE
        // tab specifically — see TransferQueueTable.h's QueueCategory)
        // at the moment captureIfInProgress() needs to find them; used
        // for that AND the sort-indicator check below (any freshly-
        // constructed tab is equally representative for that one).
        auto *activeContainer = visWidget->findChild<TransferQueueTable *>(QStringLiteral("activeQueueTable"));
        auto *table = activeContainer->findChild<QTableWidget *>();
        auto *header = table->horizontalHeader();

        // Regression: the sort-indicator arrow used to show (ascending,
        // File column) from construction, even though nothing has been
        // sorted yet (m_sortColumn stays -1 until a header is actually
        // clicked) — falsely implying the queue was already alphabetized.
        const bool noIndicatorBeforeClick = !header->isSortIndicatorShown();
        qDebug() << "[phase5] no sort indicator shown before any header click:" << noIndicatorBeforeClick;

        // Regression: resortAndRebuild()'s Direction-column sort compared
        // raw TransferDirection enum ordinals instead of the visible
        // directionText() — the same comparator basis ColStatus already
        // uses for its own column. A live SFTP/FTP server would be
        // needed to drive every direction through the real end-to-end UI
        // sort (not available here — see this project's other documented
        // live-server gaps), so this confirms the underlying comparator
        // basis directly instead: text order and ordinal order genuinely
        // DISAGREE for LocalToLocal ("local copy") vs RemoteToLocal
        // ("remote -> local") — ordinal order puts RemoteToLocal first
        // (declared before LocalToLocal), alphabetical order puts
        // LocalToLocal first ('l' < 'r') — proving the fix's choice of
        // comparator actually changes real-world behavior, not a no-op.
        const bool textOrderDiffersFromOrdinalOrder =
            TransferQueueWidget::directionText(TransferDirection::LocalToLocal)
                .localeAwareCompare(TransferQueueWidget::directionText(TransferDirection::RemoteToLocal)) < 0;
        qDebug() << "[phase5] directionText()'s ordering (what the fix now sorts Direction by) "
                    "genuinely differs from raw enum-ordinal ordering for a real pair:"
                  << textOrderDiffersFromOrdinalOrder;

        // Capture each item's rendered state the FIRST time it's actually
        // InProgress — connected AFTER visWidget's own constructor (which
        // wires up manager->itemUpdated internally), so for the same
        // emission this always runs SECOND and sees whatever the widget
        // just rendered, same connection-order trick used elsewhere in
        // this project's tests. Necessary because a tiny local copy or
        // move can go Queued -> InProgress -> Done within a single
        // event-loop turn — too fast to reliably catch by polling the
        // table after a fixed delay, unlike phase 1's real 500KB file.
        bool sawLocalCopyInProgress = false, sawMoveInProgress = false;
        QImage localCopyIconAtInProgress, moveIconAtInProgress;
        QString moveChunkStyleAtInProgress;
        // Connected to BOTH itemAdded and itemUpdated: a Move item is
        // created ALREADY InProgress (dispatchMoveEntry() sets that
        // before its one and only itemAdded, unlike enqueue()'s items,
        // which start Queued and only reach InProgress via a LATER
        // itemUpdated) — missing itemAdded here would silently never see
        // Move's InProgress state at all.
        const auto captureIfInProgress = [&](const TransferItem &item) {
            if (item.status != TransferStatus::InProgress)
                return;
            int row = -1;
            for (int r = 0; r < table->rowCount(); ++r) {
                if (table->item(r, 0) && table->item(r, 0)->data(Qt::UserRole).toInt() == item.id) {
                    row = r;
                    break;
                }
            }
            if (row < 0)
                return;
            auto *dirLabel = qobject_cast<QLabel *>(table->cellWidget(row, 1));
            if (item.direction == TransferDirection::LocalToLocal && !sawLocalCopyInProgress) {
                sawLocalCopyInProgress = true;
                if (dirLabel) localCopyIconAtInProgress = dirLabel->pixmap().toImage();
            } else if (item.direction == TransferDirection::Move && !sawMoveInProgress) {
                sawMoveInProgress = true;
                if (dirLabel) moveIconAtInProgress = dirLabel->pixmap().toImage();
                auto *progressContainer = table->cellWidget(row, 3);
                auto *progressBar = progressContainer ? progressContainer->findChild<QProgressBar *>() : nullptr;
                if (progressBar) moveChunkStyleAtInProgress = progressBar->styleSheet();
            }
        };
        QObject::connect(visManager, &TransferManager::itemAdded, &app, captureIfInProgress);
        QObject::connect(visManager, &TransferManager::itemUpdated, &app, captureIfInProgress);

        // Move is the second direction achievable without a live server
        // (moveEligible() only requires matching connectionIdentity(),
        // which LocalBackend gives any two local panes) — exercises both
        // the direction-icon-color bug and the progress-bar-chunk-color-
        // vs-icon-color consistency bug. moveEntry() routes through an
        // async checkExists() round trip first (unlike plain enqueue()),
        // so drain briefly for both to fully settle — a fixed, generous
        // margin over what local, in-process queued round trips actually
        // need, same reasoning already established elsewhere in this
        // project for local-only async waits.
        visManager->enqueue(leftPane, rightPane, "vis_localcopy.bin");
        visManager->moveEntry(leftPane, rightPane, "vis_moveme.bin");
        QEventLoop drain;
        QTimer::singleShot(400, &drain, &QEventLoop::quit);
        drain.exec();

        qDebug() << "[phase5] saw LocalToLocal actually InProgress at least once:" << sawLocalCopyInProgress;
        const QImage expectedGreenIcon =
            IconTheme::tintedIcon(":/icons/arrows-left-right.svg", IconTheme::Green, 20)
                .pixmap(20, 20).toImage();
        const bool localToLocalIsGreen =
            sawLocalCopyInProgress && localCopyIconAtInProgress == expectedGreenIcon;
        qDebug() << "[phase5] ...and its direction icon was Green then, not the Gray Queued "
                    "default:" << localToLocalIsGreen;

        qDebug() << "[phase5] saw Move actually InProgress at least once:" << sawMoveInProgress;
        const QImage expectedBlueIcon =
            IconTheme::tintedIcon(":/icons/arrow-right.svg", IconTheme::Blue, 20)
                .pixmap(20, 20).toImage();
        const bool moveIconIsBlue = sawMoveInProgress && moveIconAtInProgress == expectedBlueIcon;
        qDebug() << "[phase5] ...and its direction icon was Blue then:" << moveIconIsBlue;
        // NOT asserted as part of phase5Pass, deliberately: tracing this
        // while writing this test found that onItemUpdated()'s chunk-
        // color logic never actually runs for a Move item's InProgress
        // state in current production code at all — dispatchMoveEntry()
        // emits exactly one itemAdded (already InProgress, rendered by
        // appendRow(), which never touches chunk color) and later exactly
        // one itemUpdated when onEntryMoved()/onEntryMoveFailed() flips
        // status straight to Done/Failed; nothing ever calls
        // onItemUpdated() with the item still InProgress. The fix is
        // still correct and worth keeping (a real inconsistency, and
        // cheap insurance if Move ever gains progress reporting or
        // pause/resume), but — unlike every other check in this phase —
        // there's no genuine, non-contrived way to observe it happen
        // through the real signal flow today, so this is logged for
        // visibility only, not required to pass.
        const bool moveChunkIsBlue =
            moveChunkStyleAtInProgress.contains(IconTheme::Blue.name(), Qt::CaseInsensitive);
        qDebug() << "[phase5] (informational, not required) in-flight Move's progress-bar chunk "
                    "style captured:" << moveChunkStyleAtInProgress << "matches Blue:" << moveChunkIsBlue;

        // Direction-column sort, end to end, with two real, different
        // directions now genuinely in the table (LocalToLocal, Move) —
        // both items have long since finished by now (regardless of
        // status, `direction` never changes), so this just confirms the
        // real header-click path reaches the fixed comparator correctly:
        // ascending should put "local copy" before "move". Both items
        // fail (neither vis_localcopy.bin nor vis_moveme.bin was ever
        // created as a real file — deliberately, this phase only cares
        // about direction, not outcome), so by now both have migrated
        // to the FAILED tab specifically (see TransferQueueTable.h's
        // QueueCategory) — not the ACTIVE tab `table` still points at.
        auto *failedContainer = visWidget->findChild<TransferQueueTable *>(QStringLiteral("failedQueueTable"));
        auto *failedTable = failedContainer->findChild<QTableWidget *>();
        auto *failedHeader = failedTable->horizontalHeader();
        failedHeader->sectionClicked(1);   // ColDirection
        const bool directionSortAscendingCorrect =
            failedTable->rowCount() == 2
            && failedTable->item(0, 0)->text() == "vis_localcopy.bin"
            && failedTable->item(1, 0)->text() == "vis_moveme.bin";
        qDebug() << "[phase5] Direction column sorts ascending by directionText() end to end:"
                  << directionSortAscendingCorrect;

        phase5Pass = noIndicatorBeforeClick && textOrderDiffersFromOrdinalOrder
            && localToLocalIsGreen && moveIconIsBlue && directionSortAscendingCorrect;
        qDebug() << (phase5Pass ? "[phase5] PASS" : "[phase5] FAIL");
    });

    // ---- Phase 6: a real, severe crash regression — reported live, on
    // Windows, transferring a large number of files: the app hung
    // ("stopped interacting with Windows") and consumed memory at a
    // runaway rate. Root cause: with a column sort active, onItemAdded()
    // called resortAndRebuild() (a full table wipe + rebuild of every
    // row's items AND cell widgets) SYNCHRONOUSLY, once per item — N
    // items enqueued in one tight loop (any folder transfer, or a multi-
    // select drag-drop) meant N full rebuilds, each O(current size), for
    // O(N²) total widget churn. A few thousand files was enough to hang
    // the GUI thread and exhaust memory. The fix coalesces every
    // onItemAdded() within one synchronous burst into a SINGLE deferred
    // rebuild (QTimer::singleShot(0, ...)) — this proves that directly:
    // enqueue a large batch while sorted, pump the event loop exactly
    // ONCE, and confirm both (a) it finishes fast (the old O(N²) code
    // would take vastly longer, if it didn't hang outright, for this
    // many items) and (b) the end state is still fully correct — every
    // item present, correctly sorted, nothing dropped by the coalescing
    // itself. A fresh TransferManager/TransferQueueWidget pair, isolated
    // from every earlier phase.
    //
    // Scheduled with a LARGE gap after phase5 (4200ms + its own ~400ms
    // nested drain), not just enough for the nominal case — a real bug
    // found on real CI (a slower/more loaded macOS runner, not local):
    // phase5's own wait is a NESTED QEventLoop::exec(), and a nested
    // loop pumps the WHOLE app's pending timer queue, not just events
    // scoped to itself — so if phase5's nested drain is still running
    // when this timer becomes due, phase6 (and this test's OWN
    // aggregation, chained to phase6's completion below) can fire
    // REENTRANTLY from inside phase5's still-in-progress lambda,
    // printing a result before phase5 ever finishes setting phase5Pass.
    // A wide margin here is cheap insurance against that same class of
    // collision recurring under different CI timing, matching the
    // generous-margin philosophy this whole file already uses for
    // phases 1-5. ----
    bool phase6Pass = false;
    QTimer::singleShot(7000, &app, [&]() {
        auto *bulkManager = new TransferManager(&app);
        auto *bulkWidget = new TransferQueueWidget(bulkManager);
        // None of the 1500 items below have a real backing file, so
        // none of them can actually resolve (even a failure needs at
        // least a threadpool round trip now — see LocalBackend's own
        // QtConcurrent-backed copy) within the single processEvents()
        // call this phase makes — all 1500 stay Queued, hence in the
        // ACTIVE tab specifically (see TransferQueueTable.h's
        // QueueCategory) for this whole phase.
        auto *activeContainer = bulkWidget->findChild<TransferQueueTable *>(QStringLiteral("activeQueueTable"));
        auto *table = activeContainer->findChild<QTableWidget *>();
        table->horizontalHeader()->sectionClicked(0);   // activate ascending Name sort

        constexpr int kFileCount = 1500;
        QStringList expectedNames;
        expectedNames.reserve(kFileCount);
        for (int i = 0; i < kFileCount; ++i)
            expectedNames << QStringLiteral("bulk_%1.bin").arg(i, 4, 10, QLatin1Char('0'));

        QElapsedTimer timer;
        timer.start();
        // The actual regression trigger: a tight loop of enqueue() calls,
        // all synchronous, all while m_sortColumn != -1 — exactly the
        // pattern MainWindow::enqueueEntries()/enqueueFolder() produce
        // for a real multi-file or folder transfer.
        for (const QString &name : std::as_const(expectedNames))
            bulkManager->enqueue(leftPane, rightPane, name);
        qApp->processEvents();   // lets the ONE coalesced deferred rebuild actually run
        const qint64 elapsedMs = timer.elapsed();

        // Generous — the fixed (O(N)) path finishes in well under 100ms
        // locally; this just needs to be far below what the old O(N²)
        // path would take for 1500 items (proportional to 1500² rebuilt-
        // row operations) to catch a real regression back to that
        // pattern, not to pin down an exact performance number.
        const bool fastEnough = elapsedMs < 5000;
        qDebug() << "[phase6] 1500-item batch enqueue while sorted took" << elapsedMs
                  << "ms, expected well under 5000ms:" << fastEnough;

        QStringList actualNames;
        for (int r = 0; r < table->rowCount(); ++r) {
            if (auto *it = table->item(r, 0))
                actualNames << it->text();
        }
        const bool correctCountAndOrder = actualNames == expectedNames;
        qDebug() << "[phase6] all" << kFileCount
                  << "items present in correct sorted order after the single coalesced rebuild:"
                  << correctCountAndOrder;

        phase6Pass = fastEnough && correctCountAndOrder;
        qDebug() << (phase6Pass ? "[phase6] PASS" : "[phase6] FAIL");

        // ---- Phase 7: a second real crash regression, found from a
        // follow-up live report AFTER phase6's fix already shipped — the
        // app still hung (though it eventually recovered) on a large
        // transfer with NO sort ever touched, meaning phase6's fix (which
        // only coalesces onItemAdded()'s rebuild-while-SORTED path) never
        // even applies. THREE separate O(N)-or-worse-per-event
        // bottlenecks, all hit for an unsorted bulk transfer, found by
        // isolating where the time actually went (a diagnostic split of
        // "file creation" vs. "the enqueue() loop itself" vs. "waiting
        // for completions" — the SECOND of those turned out to dominate
        // by nearly two orders of magnitude, not the first two hypotheses
        // this phase originally shipped to prove):
        // (1) TransferQueueWidget::rowForId(), called from onItemUpdated()
        //     (fires on every progress tick), used to linearly scan every
        //     row — fixed with an id->row hash map.
        // (2) TransferManager::startNext()'s own scan used to always
        //     start from index 0 even though m_items only ever grows —
        //     fixed with a lazily-advancing "nothing below this index is
        //     still Queued" floor. This helps once items actually start
        //     RESOLVING, but on its own did almost nothing for THIS
        //     phase's actual pattern (measured: enqueuing 15,000 items
        //     still took 10.4 SECONDS with only fixes 1+2 applied) —
        //     because ALL of them stay genuinely Queued-and-busy for the
        //     entire synchronous enqueue loop (nothing can complete
        //     without an event-loop turn, which the loop never yields),
        //     so the floor has nothing resolved to skip past.
        // (3) THE actual dominant cost: enqueue() called startNext()'s
        //     full scan UNCONDITIONALLY for every new item, even when
        //     every backend it could possibly use was already known to
        //     be claimed — turning "enqueue 15,000 items against one busy
        //     backend" into 15,000 full rescans of an ever-growing
        //     Queued-but-busy prefix, an O(N²) storm independent of (1)/
        //     (2). Fixed with startNextIfLikelyToDispatch() — a cheap,
        //     m_active-sized (not queue-sized) pre-check that skips the
        //     scan entirely when nothing could possibly dispatch, safe
        //     because a backend actually freeing up already re-triggers
        //     a full scan on its own. This one change alone took the
        //     enqueue loop for 15,000 items from ~10,400ms to ~20ms in
        //     manual testing.
        //
        // Proof here is real end-to-end execution, not just queueing
        // shape like phase6: a real batch of small local files, actually
        // transferred start to finish, unsorted (the exact condition
        // phase6 doesn't cover), on a FRESH manager/pair so this doesn't
        // interact with phases 1-3's own shared manager. Chained directly
        // off phase6's own completion (see this test's own hard-won
        // lesson above on why an independent timer here would risk yet
        // another CI-only race) — polls for completion via repeated
        // processEvents() calls rather than a nested QEventLoop::exec(),
        // for the same reason. Pass/fail is based on ACTUALLY FINISHING
        // within a very generous timeout, not a tight performance number
        // — completing at all, without hanging, is the real thing this
        // guards against; the old code did eventually finish too (per
        // the live report), just unacceptably slowly. ----
        bool phase7Pass = false;
        {
            const QString bulkSrcDir = QStringLiteral("/tmp/transfer_test/bulk_src");
            const QString bulkDstDir = QStringLiteral("/tmp/transfer_test/bulk_dst");
            QDir(bulkSrcDir).removeRecursively();
            QDir(bulkDstDir).removeRecursively();
            QDir().mkpath(bulkSrcDir);
            QDir().mkpath(bulkDstDir);

            constexpr int kRealFileCount = 3000;
            for (int i = 0; i < kRealFileCount; ++i) {
                QFile f(bulkSrcDir + QStringLiteral("/real_%1.bin").arg(i, 4, 10, QLatin1Char('0')));
                f.open(QIODevice::WriteOnly);
                f.write(QByteArray(200, 'x'));
            }

            auto *realManager = new TransferManager(&app);
            auto *realLeftPane = new FilePaneWidget(new LocalBackend());
            auto *realRightPane = new FilePaneWidget(new LocalBackend());
            realLeftPane->navigateTo(bulkSrcDir);
            realRightPane->navigateTo(bulkDstDir);

            // Settle both panes' navigation before enqueueing anything —
            // navigateTo() dispatches via Qt::QueuedConnection (even a
            // LocalBackend's own auto-connect-on-construction navigation
            // is queued, not synchronous), so calling enqueue() in the
            // same synchronous block without first letting that resolve
            // is a real, previously-documented race in this exact
            // codebase (see sync-browsing-test's own history) — enqueue()
            // would silently build paths against a still-empty
            // currentDirectory(). Polled, not a fixed delay, and bounded
            // by a generous timeout consistent with this file's own
            // established margins.
            {
                QElapsedTimer settle;
                settle.start();
                while ((realLeftPane->currentDirectory() != bulkSrcDir
                        || realRightPane->currentDirectory() != bulkDstDir)
                       && settle.elapsed() < 5000) {
                    qApp->processEvents(QEventLoop::AllEvents, 10);
                }
            }

            int doneCount = 0;
            QObject::connect(realManager, &TransferManager::itemUpdated, &app, [&](const TransferItem &it) {
                if (it.status == TransferStatus::Done)
                    ++doneCount;
            });

            QElapsedTimer timer;
            timer.start();
            // Unsorted (default TransferQueueWidget state, never touched
            // here) — the actual condition this phase exists to cover.
            for (int i = 0; i < kRealFileCount; ++i)
                realManager->enqueue(realLeftPane, realRightPane,
                                      QStringLiteral("real_%1.bin").arg(i, 4, 10, QLatin1Char('0')));

            QElapsedTimer waitTimer;
            waitTimer.start();
            while (doneCount < kRealFileCount && waitTimer.elapsed() < 60000)
                qApp->processEvents(QEventLoop::AllEvents, 20);

            const qint64 elapsedMs = timer.elapsed();
            const bool allCompleted = doneCount == kRealFileCount;
            qDebug() << "[phase7]" << kRealFileCount << "real, unsorted local-to-local transfers all "
                        "reached Done:" << allCompleted << "(" << doneCount << "/" << kRealFileCount
                     << ") in" << elapsedMs << "ms — informational only, not a pass/fail gate (see this "
                        "phase's own doc comment on why completion, not timing, is what's asserted)";

            phase7Pass = allCompleted;
            qDebug() << (phase7Pass ? "[phase7] PASS" : "[phase7] FAIL");
        }

        // Chained off phase6's OWN completion, not a separate fixed-delay
        // timer — a real bug found on real CI (not locally): a slower/
        // more loaded runner made phase6's actual work (scheduled to
        // START at 5000ms) run long enough to still be in flight when an
        // earlier draft's independent `QTimer::singleShot(5600, ...)`
        // aggregation fired on its own fixed schedule, reading phase6Pass
        // before this lambda ever set it — printing a false "AT LEAST ONE
        // PHASE FAILED" moments before phase6's own PASS line even
        // appeared in the log. Every phase before this one is safe from
        // that same race only because each already runs to completion,
        // synchronously, inside its own timer callback before the next
        // one is scheduled — this now does the same.
        const bool overallPass = phase1Pass && phase2SyncPass && phase2Pass && phase3Pass
            && phase4Pass && phase5Pass && phase6Pass && phase7Pass;
        qDebug() << (overallPass ? "[test] ALL PHASES PASS" : "[test] AT LEAST ONE PHASE FAILED");
        app.exit(overallPass ? 0 : 1);
    });

    return app.exec();
}
