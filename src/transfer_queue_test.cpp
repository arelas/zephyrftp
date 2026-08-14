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
    // before any event-loop turn lets startNext() begin real I/O). ----
    bool phase4Pass = false;
    QTimer::singleShot(3800, &app, [&]() {
        auto *sortManager = new TransferManager(&app);
        auto *queueWidget = new TransferQueueWidget(sortManager);
        auto *table = queueWidget->findChild<QTableWidget *>();

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
        auto *table = visWidget->findChild<QTableWidget *>();
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
        // ascending should put "local copy" before "move".
        header->sectionClicked(1);   // ColDirection
        const bool directionSortAscendingCorrect =
            table->rowCount() == 2
            && table->item(0, 0)->text() == "vis_localcopy.bin"
            && table->item(1, 0)->text() == "vis_moveme.bin";
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
    // from every earlier phase. ----
    bool phase6Pass = false;
    QTimer::singleShot(5000, &app, [&]() {
        auto *bulkManager = new TransferManager(&app);
        auto *bulkWidget = new TransferQueueWidget(bulkManager);
        auto *table = bulkWidget->findChild<QTableWidget *>();
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
    });

    QTimer::singleShot(5600, &app, [&]() {
        const bool overallPass = phase1Pass && phase2SyncPass && phase2Pass && phase3Pass
            && phase4Pass && phase5Pass && phase6Pass;
        qDebug() << (overallPass ? "[test] ALL PHASES PASS" : "[test] AT LEAST ONE PHASE FAILED");
        app.exit(overallPass ? 0 : 1);
    });

    return app.exec();
}
