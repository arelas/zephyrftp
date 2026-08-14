// Real end-to-end test of destination-conflict resolution — Overwrite/
// Skip for files, Write Into/Skip for folders, and critically the
// "apply to all remaining conflicts" checkbox that's supposed to make
// the SECOND conflict in a batch resolve automatically without a second
// prompt.
//
// This actually drives the real QMessageBox — not a mock. A QTimer
// scheduled to fire during the dialog's still-blocking exec() call
// (exec() pumps the event loop internally, which is what makes this
// possible at all) finds it via QApplication::activeModalWidget(),
// toggles its checkbox, and clicks the matching button by text. Same
// technique already proven for capturing an open context menu in this
// project's development, applied here to actually verify behavior
// rather than just appearance.
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QCheckBox>
#include <QAbstractButton>
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"
#include "transfer/TransferManager.h"

namespace {
bool writeFile(const QString &path, const QString &content)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content.toUtf8());
    return true;
}

QString readFile(const QString &path)
{
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    return QString::fromUtf8(f.readAll());
}

// Finds the currently-open QMessageBox (if any), sets its checkbox to
// checkApplyToAll, and clicks whichever button's text contains
// buttonText (case-insensitive) — avoids needing to know the exact
// QMessageBox::StandardButton role, since these are custom buttons
// added via addButton(). Returns false (and does nothing) if no
// QMessageBox is currently open — callers use this to confirm a dialog
// that shouldn't appear (a remembered decision in effect) really didn't.
bool clickConflictDialog(bool checkApplyToAll, const QString &buttonText)
{
    auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (!box)
        return false;

    if (QCheckBox *checkbox = box->checkBox())
        checkbox->setChecked(checkApplyToAll);

    for (QAbstractButton *button : box->buttons()) {
        if (button->text().contains(buttonText, Qt::CaseInsensitive)) {
            button->click();
            return true;
        }
    }
    return false;
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Without this, closing the conflict QMessageBox (the only actually-
    // visible window in this test — the panes are constructed but never
    // shown) triggers Qt's default "quit when the last window closes"
    // behavior, ending the test's event loop before any of the later
    // phases get a chance to run.
    app.setQuitOnLastWindowClosed(false);

    const QString base = "/tmp/conflict_test";
    // Wipe any leftover state from a previous run before regenerating —
    // same convention transfer_queue_test.cpp/folder_transfer_test.cpp
    // already use for their own /tmp dirs. Without this, a stale
    // mergedir/f.txt left behind by an earlier run makes Phase D's own
    // per-file checkExists() genuinely find a real conflict (the file
    // already exists) that this test's own Phase D flow never accounts
    // for, opening an extra, un-clicked dialog that Phase E's later
    // click can then steal instead of reentrancy.txt's own dialog —
    // a real, reproducible local-only failure this exact bug caused.
    QDir(base).removeRecursively();
    QDir().mkpath(base + "/src");
    QDir().mkpath(base + "/dst");

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    auto *srcPane = new FilePaneWidget(new LocalBackend());
    auto *dstPane = new FilePaneWidget(new LocalBackend());
    auto *manager = new TransferManager(&app);

    int itemsUpdatedToDone = 0;
    int itemsUpdatedToSkipped = 0;
    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.status == TransferStatus::Done) itemsUpdatedToDone++;
        if (item.status == TransferStatus::Skipped) itemsUpdatedToSkipped++;
    });

    // ===================================================================
    // Phase A: file conflict, Overwrite + "apply to all"
    // ===================================================================
    writeFile(base + "/src/a.txt", "NEW A");
    writeFile(base + "/src/b.txt", "NEW B");
    writeFile(base + "/dst/a.txt", "OLD A");
    writeFile(base + "/dst/b.txt", "OLD B");

    QTimer::singleShot(200, &app, [&]() {
        srcPane->navigateTo(base + "/src");
        dstPane->navigateTo(base + "/dst");
    });

    QTimer::singleShot(400, &app, [&]() {
        manager->enqueue(srcPane, dstPane, "a.txt");
        manager->enqueue(srcPane, dstPane, "b.txt");
    });

    // The dialog for a.txt should be open by now — check the box, click Overwrite.
    QTimer::singleShot(600, &app, [&]() {
        const bool clicked = clickConflictDialog(/*checkApplyToAll=*/true, "Overwrite");
        check("Phase A: conflict dialog for a.txt actually appeared", clicked);
    });

    QTimer::singleShot(1200, &app, [&]() {
        check("Phase A: a.txt was overwritten with the new content", readFile(base + "/dst/a.txt") == "NEW A");
        check("Phase A: b.txt was ALSO overwritten — remembered decision, no second prompt needed",
              readFile(base + "/dst/b.txt") == "NEW B");
        check("Phase A: exactly 2 items reached Done", itemsUpdatedToDone == 2);
    });

    // ===================================================================
    // Phase B: file conflict, Skip + "apply to all" (queue has drained
    // between phases, so the remembered decision from Phase A is gone —
    // that reset behavior is itself being exercised here, not assumed)
    // ===================================================================
    QTimer::singleShot(1600, &app, [&]() {
        writeFile(base + "/src/c.txt", "NEW C");
        writeFile(base + "/src/d.txt", "NEW D");
        writeFile(base + "/dst/c.txt", "OLD C");
        writeFile(base + "/dst/d.txt", "OLD D");

        manager->enqueue(srcPane, dstPane, "c.txt");
        manager->enqueue(srcPane, dstPane, "d.txt");
    });

    QTimer::singleShot(1800, &app, [&]() {
        const bool clicked = clickConflictDialog(/*checkApplyToAll=*/true, "Skip");
        check("Phase B: a fresh conflict dialog appeared for c.txt — proves the Phase A "
              "remembered decision reset when the queue drained, not leaked across batches", clicked);
    });

    QTimer::singleShot(2400, &app, [&]() {
        check("Phase B: c.txt was left untouched (skip, not overwrite)",
              readFile(base + "/dst/c.txt") == "OLD C");
        check("Phase B: d.txt was ALSO skipped automatically — remembered decision",
              readFile(base + "/dst/d.txt") == "OLD D");
        check("Phase B: exactly 2 items reached Skipped", itemsUpdatedToSkipped == 2);
    });

    // ===================================================================
    // Phase C: directory conflict, Skip — nothing should be enumerated
    // or transferred at all
    // ===================================================================
    bool sawFolderSkipped = false;
    bool sawFolderStartedDuringPhaseC = false;
    QObject::connect(manager, &TransferManager::folderTransferSkipped, &app, [&](const QString &name) {
        if (name == "confdir") sawFolderSkipped = true;
    });
    QObject::connect(manager, &TransferManager::folderTransferStarted, &app, [&](const QString &name) {
        if (name == "confdir") sawFolderStartedDuringPhaseC = true;
    });

    QTimer::singleShot(2800, &app, [&]() {
        QDir().mkpath(base + "/src/confdir");
        writeFile(base + "/src/confdir/e.txt", "should never arrive");
        QDir().mkpath(base + "/dst/confdir");   // pre-existing conflict

        manager->enqueueFolder(srcPane, dstPane, "confdir");
    });

    QTimer::singleShot(3300, &app, [&]() {
        const bool clicked = clickConflictDialog(/*checkApplyToAll=*/false, "Skip");
        check("Phase C: folder conflict dialog appeared for confdir", clicked);
    });

    QTimer::singleShot(3700, &app, [&]() {
        check("Phase C: folderTransferSkipped fired", sawFolderSkipped);
        check("Phase C: enumeration never started (folderTransferStarted never fired) — "
              "skip happens before paying for a walk of the source tree",
              !sawFolderStartedDuringPhaseC);
        check("Phase C: e.txt never arrived in the destination folder",
              !QFile::exists(base + "/dst/confdir/e.txt"));
    });

    // ===================================================================
    // Phase D: directory conflict, Write Into — should merge, not replace.
    // Given its own independent top-level timers (not nested inside
    // Phase C's completion callback) with generous margins — an earlier
    // version nested this and used tighter timing, which passed most of
    // the time but occasionally missed the dialog: adding debug logging
    // to investigate the failure made it start passing reliably again,
    // which is itself the signature of a timing race, not a real logic
    // bug (confirmed by rerunning with the same margins the debug prints
    // had incidentally introduced). Fixed properly here with real margin
    // instead of relying on incidental logging overhead.
    // ===================================================================
    QTimer::singleShot(4000, &app, [&]() {
        QDir().mkpath(base + "/src/mergedir");
        writeFile(base + "/src/mergedir/f.txt", "new file via write-into");
        QDir().mkpath(base + "/dst/mergedir");
        writeFile(base + "/dst/mergedir/preexisting.txt", "already here before the transfer");

        manager->enqueueFolder(srcPane, dstPane, "mergedir");
    });

    QTimer::singleShot(4500, &app, [&]() {
        const bool clicked = clickConflictDialog(/*checkApplyToAll=*/false, "Write Into");
        check("Phase D: folder conflict dialog appeared for mergedir", clicked);
    });

    QTimer::singleShot(5300, &app, [&]() {
        check("Phase D: the new file was transferred into the existing folder",
              readFile(base + "/dst/mergedir/f.txt") == "new file via write-into");
        check("Phase D: the pre-existing file in that folder was left alone (merge, not replace)",
              readFile(base + "/dst/mergedir/preexisting.txt") == "already here before the transfer");
    });

    // ===================================================================
    // Phase E: regression test for a real dangling-reference bug.
    // onDestinationExistsChecked() used to hold a `TransferItem &` taken
    // from m_items across askConflict()'s QMessageBox::exec() call —
    // exec() pumps the event loop, and with real concurrent transfers
    // (TransferManager can now have several checkExists() calls in
    // flight at once), some OTHER path can legitimately append a new
    // item to m_items while this dialog is still open: verified directly
    // in TransferManager.cpp — the Move-into-a-non-empty-folder branch
    // of this SAME onDestinationExistsChecked() function calls
    // m_items.append() with zero dialog of its own whenever its
    // resolution is already "always overwrite", reachable via a second,
    // concurrently in-flight checkExists() response while a first item's
    // dialog blocks. QList<TransferItem> reallocating its backing store
    // on append would dangle any reference taken before it. Reproduced
    // here more directly (not via a second Move dialog) by firing many
    // real TransferManager::enqueue() calls — each one a genuine
    // m_items.append(), the same operation the Move branch performs —
    // while the target item's own conflict dialog is still open, then
    // confirming that item's own conflict resolution still lands on the
    // item it was actually about.
    //
    // Driven by plain independent QTimer::singleShot() offsets, same
    // shape as phases A-D above — deliberately NOT a nested
    // processEvents()-based poll waiting for the dialog to appear. That
    // was tried first and is fundamentally unsound, not just fragile: the
    // dialog opens SYNCHRONOUSLY, on the same call stack, as a direct
    // result of the very processEvents() call that would be doing the
    // "waiting" — askConflict()'s QMessageBox::exec() is itself a nested
    // QEventLoop entered from inside that delivery, so the OUTER
    // processEvents() call cannot return (and the poll's own condition
    // can never be re-checked) until the dialog is dismissed by
    // something else. Confirmed directly: an earlier version reliably
    // stalled until an unrelated 20-second safety-net timer's own
    // app.exit() forcibly unwound the stuck nested loop, every single
    // time, not just occasionally. Trusting a fixed delay for the dialog
    // to open (like every phase above already does successfully) sidesteps
    // this entirely — the top-level app.exec() loop delivers the
    // checkExists() response and opens the dialog on its own, between
    // scheduled timers, well before the next one fires.
    // ===================================================================
    QTimer::singleShot(5600, &app, [&]() {
        writeFile(base + "/src/reentrancy.txt", "should survive the flood");
        writeFile(base + "/dst/reentrancy.txt", "OLD reentrancy");   // real conflict — opens a dialog
        manager->enqueue(srcPane, dstPane, "reentrancy.txt");
    });

    QTimer::singleShot(6100, &app, [&]() {
        const bool dialogOpen = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()) != nullptr;
        check("Phase E: reentrancy.txt's conflict dialog is open before the flood", dialogOpen);

        // Reuses dstPane itself (NOT a fresh backend) — its backend is
        // already claimed busy by reentrancy.txt's own still-pending
        // ActiveTransfer entry for as long as this dialog is up, so
        // every one of these enqueue() calls appends to m_items and then
        // finds that backend busy and stays harmlessly Queued: no
        // dispatch, no filesystem I/O, no dialog of its own, no
        // cascading async round trips — just the bare m_items.append()
        // call itself, genuinely happening WHILE askConflict() for
        // reentrancy.txt is still on the call stack. This loop is purely
        // synchronous (no event-loop pumping happens inside it, so
        // nothing else can run or change state until it returns) — 80 is
        // well past QList's growth increments from this test's ~9 prior
        // items, so this is guaranteed to force at least one real
        // reallocation, not just a theoretical possibility, while staying
        // cheap: enqueue() unconditionally calls startNext() internally,
        // which rescans the WHOLE queue every time, making this loop
        // O(n²) in the queue's own size.
        for (int i = 0; i < 80; ++i)
            manager->enqueue(srcPane, dstPane, QString("decoy%1.txt").arg(i));
    });

    QTimer::singleShot(6600, &app, [&]() {
        const bool clicked = clickConflictDialog(/*checkApplyToAll=*/false, "Skip");
        check("Phase E: a dialog was still there to click after the flood", clicked);
        // The rest (confirming reentrancy.txt's own resolution) is NOT
        // waited on here — clicking only SCHEDULES the dialog's own
        // QEventLoop to quit; askConflict()'s exec() call, and everything
        // after it in onDestinationExistsChecked(), can only actually
        // resume once THIS callback returns and control unwinds back to
        // it. Handled entirely by the itemUpdated connection below
        // instead, which reacts to the REAL signal once it actually
        // fires — no further timer or poll needed.
    });

    bool sawReentrancyResolvedCorrectly = false;
    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.status == TransferStatus::Skipped && item.fileName == "reentrancy.txt"
            && item.destPath == base + "/dst/reentrancy.txt") {
            sawReentrancyResolvedCorrectly = true;
            check("Phase E: reentrancy.txt's own dialog response resolved reentrancy.txt itself — "
                  "correct id/fileName/destPath, not a value corrupted by the append flood",
                  sawReentrancyResolvedCorrectly);
            check("Phase E: destination reentrancy.txt was left untouched (skip really applied, to the right item)",
                  readFile(base + "/dst/reentrancy.txt") == "OLD reentrancy");

            qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
            app.exit(allPass ? 0 : 1);
        }
    });

    // Safety net, not the primary mechanism — same reasoning
    // remote_to_remote_test.cpp's own equivalent deadline uses: if a real
    // bug ever breaks the expected sequence, this turns an indefinite
    // hang into a clear, fast, informative failure instead of tying up a
    // CI job until its outer timeout.
    QTimer::singleShot(20000, &app, [&]() {
        if (sawReentrancyResolvedCorrectly)
            return;
        check("test timed out waiting for reentrancy.txt's own resolution", false);
        qDebug() << "[test] AT LEAST ONE FAILURE";
        app.exit(1);
    });

    return app.exec();
}
