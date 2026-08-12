// Headless functional test of FilePaneWidget's navigation history —
// back/forward/up — against a real LocalBackend and real temp
// directories, not mocked state. Everything here goes through the same
// async QMetaObject::invokeMethod(..., Qt::QueuedConnection) path real
// navigation uses, so each step is sequenced with QTimer::singleShot,
// same pattern as transfer_queue_test.cpp.
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QEventLoop>
#include <QMenu>
#include <QAction>
#include <QInputDialog>
#include <QMessageBox>
#include <QMetaObject>
#include <QTest>
#include <QTreeView>
#include <QItemSelectionModel>
#include <QLabel>
#include <functional>
#include <memory>
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // None of this file's panes are ever shown (see below) — without
    // this, the fileOpRace phase's QMenu/QInputDialog (the only actually-
    // visible windows anywhere in this test) closing after being answered
    // triggers Qt's default "quit when the last window closes" behavior,
    // silently ending the test's event loop before later phases (or even
    // this phase's own final checks) get a chance to run — confirmed
    // empirically: roughly 3 in 10 runs exited early without it. Same
    // fix conflict_resolution_test.cpp already applies for the same
    // reason with its own QMessageBox.
    app.setQuitOnLastWindowClosed(false);

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    // --- Direct checks of parentOfPath() itself, synchronous, no event
    // loop needed --- covers a real bug found after shipping: "C:/Users"
    // was computing its parent as the bare string "C:" (no trailing
    // slash), which Windows interprets as "the process's current
    // directory on drive C" — a legacy per-drive-working-directory quirk
    // — NOT the drive's actual root. Pressing Up from a top-level local
    // folder landed wherever the app happened to launch from instead of
    // "C:\". This sandbox is Linux, so this can only be verified by
    // calling the pure string-manipulation function directly — a real
    // "C:/Users" path could never resolve through the full
    // navigateTo()->LocalBackend->onDirectoryListed() stack here.
    check("Windows: parent of 'C:/Users/David' is 'C:/Users'",
          FilePaneWidget::parentOfPath("C:/Users/David") == "C:/Users");
    check("Windows: THE BUG FIX — parent of 'C:/Users' is 'C:/' (not the bare, ambiguous 'C:')",
          FilePaneWidget::parentOfPath("C:/Users") == "C:/");
    check("Windows: parent of the drive root 'C:/' is itself (safe no-op)",
          FilePaneWidget::parentOfPath("C:/") == "C:/");
    check("POSIX: parent of '/home/user' is '/home' (unaffected by the Windows fix)",
          FilePaneWidget::parentOfPath("/home/user") == "/home");
    check("POSIX: parent of '/home' is '/'",
          FilePaneWidget::parentOfPath("/home") == "/");
    check("POSIX: parent of the root '/' is itself (safe no-op)",
          FilePaneWidget::parentOfPath("/") == "/");

    const QString base = "/tmp/nav_test";
    QDir().mkpath(base + "/a/b/c");
    QDir().mkpath(base + "/other");

    auto *pane = new FilePaneWidget(new LocalBackend());

    // Step 0 (t=200ms): let the initial home-directory listing complete
    // first (constructor triggers an async connectToHost() ->
    // listDirectory("")), then start the real sequence from a known
    // directory rather than depending on whatever the home dir is.
    QTimer::singleShot(200, &app, [&]() {
        pane->navigateTo(base + "/a");
    });

    QTimer::singleShot(400, &app, [&]() {
        pane->navigateTo(base + "/a/b");
    });

    QTimer::singleShot(600, &app, [&]() {
        pane->navigateTo(base + "/a/b/c");
    });

    // At this point history should be [home, a, a/b, a/b/c], sitting at
    // a/b/c, forward disabled (nothing ahead), back enabled.
    QTimer::singleShot(800, &app, [&]() {
        check("at a/b/c after three forward navigations",
              pane->currentDirectory() == base + "/a/b/c");
        check("can go back from a/b/c", pane->canGoBack());
        check("cannot go forward from a/b/c (nothing ahead yet)", !pane->canGoForward());
        pane->goBack();
    });

    QTimer::singleShot(1000, &app, [&]() {
        check("goBack() from a/b/c lands on a/b", pane->currentDirectory() == base + "/a/b");
        pane->goBack();
    });

    QTimer::singleShot(1200, &app, [&]() {
        check("goBack() again lands on a", pane->currentDirectory() == base + "/a");
        check("can still go forward after two backs", pane->canGoForward());
        pane->goForward();
    });

    QTimer::singleShot(1400, &app, [&]() {
        check("goForward() from a lands back on a/b", pane->currentDirectory() == base + "/a/b");
        // Now branch to a NEW directory while sitting mid-history (not
        // at the newest entry) — this should truncate the "a/b/c"
        // forward entry, same convention every browser uses.
        pane->navigateTo(base + "/other");
    });

    QTimer::singleShot(1600, &app, [&]() {
        check("navigating to a new path from mid-history lands there",
              pane->currentDirectory() == base + "/other");
        check("forward history was truncated by the branch (a/b/c is gone)",
              !pane->canGoForward());
        check("back is still possible (a/b is right behind us)", pane->canGoBack());

        // Up: from base/other, parent should be base itself.
        pane->goUp();
    });

    QTimer::singleShot(1800, &app, [&]() {
        check("goUp() from base/other lands on base", pane->currentDirectory() == base);

        // Up again: from base, parent should be /tmp.
        pane->goUp();
    });

    QTimer::singleShot(2000, &app, [&]() {
        check("goUp() from base lands on /tmp", pane->currentDirectory() == "/tmp");

        // Up from filesystem root should be a safe no-op, not an error
        // or a crash — navigate there directly first, then try Up.
        pane->navigateTo("/");
    });

    QTimer::singleShot(2200, &app, [&]() {
        check("navigated to filesystem root", pane->currentDirectory() == "/");
        pane->goUp();
    });

    QTimer::singleShot(2400, &app, [&]() {
        check("goUp() from root is a safe no-op (stays at root)", pane->currentDirectory() == "/");
    });

    // ---------- Regression: overlapping navigation requests must not
    // corrupt history. A real bug: onDirectoryListed() decided whether to
    // push a new history entry using a single shared m_navigatingHistory
    // flag set by whichever navigateTo() call most recently ran — clicking
    // Back twice quickly (no event-loop turn between them, exactly what's
    // done below) let the second call's setup silently stomp the first's
    // still-pending state, corrupting m_history/m_historyIndex. A fresh
    // pane/directory tree, isolated from the sequence above. ----------
    const QString raceBase = "/tmp/nav_test_race";
    QDir().mkpath(raceBase + "/x");
    QDir().mkpath(raceBase + "/y");
    auto *racePane = new FilePaneWidget(new LocalBackend());

    // Polls rather than a fixed delay for the SETUP navigation (not the
    // deliberately-racy part below, which must stay two synchronous,
    // back-to-back calls) — a fixed delay here would be exactly the kind
    // of flaky timing assumption this project avoids elsewhere (see
    // remote_to_remote_test.cpp's own header comment on why it was
    // rewritten away from one). LocalBackend's round trip is normally
    // near-instant, but a fixed 200ms margin was observed to occasionally
    // not be enough under real system load.
    auto waitUntil = [&](std::function<bool()> condition) {
        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeoutTimer.start(5000);
        while (!condition() && timeoutTimer.isActive())
            loop.processEvents(QEventLoop::AllEvents, 20);
    };

    QTimer::singleShot(2600, &app, [&]() {
        racePane->navigateTo(raceBase + "/x");
        waitUntil([&]() { return racePane->currentDirectory() == raceBase + "/x"; });

        racePane->navigateTo(raceBase + "/y");
        waitUntil([&]() { return racePane->currentDirectory() == raceBase + "/y"; });

        check("race setup: at y, with x and (before this) the start dir behind it",
              racePane->currentDirectory() == raceBase + "/y" && racePane->canGoBack());

        // Two goBack() calls back to back, synchronously — no event-loop
        // turn in between, the exact pattern a fast double-click produces.
        // This part deliberately stays a raw, unwaited pair of calls: the
        // whole point is exercising what happens when the second fires
        // before the first's response has landed.
        racePane->goBack();
        racePane->goBack();

        waitUntil([&]() { return racePane->currentDirectory() == raceBase + "/x"; });

        // Pre-fix: the second, overlapping goBack() call still mutated
        // m_historyIndex and issued its own navigateTo() before the
        // first's response arrived, corrupting m_navigatingHistory's
        // shared state — the actual, observable result was landing on
        // the start directory (skipping x entirely) with the forward
        // history (x and y) silently truncated/lost. Post-fix: the second
        // call sees a navigation already in flight and is refused
        // outright, so exactly one step back happens — landing on x,
        // with y still intact as a forward entry.
        check("only ONE of the two overlapping goBack() calls actually took effect — landed on x, "
              "not skipped past it to the start directory",
              racePane->currentDirectory() == raceBase + "/x");
        check("forward history (y) survived — NOT silently truncated by the overlapping call",
              racePane->canGoForward());
    });

    // ---------- Regression: a file operation's own "fire and refresh"
    // directoryListed must not be mistaken for a real navigateTo()
    // response while one is genuinely in flight — a real bug found by
    // code review (see m_pendingFileOpRefreshes's own doc comment in
    // FilePaneWidget.h). Drives the REAL "New File..." context-menu path,
    // not the backend directly — QMenu::exec()/QInputDialog::getText()
    // both pump the event loop internally while blocking, same technique
    // already proven for the real QMessageBox in
    // conflict_resolution_test.cpp — and showContextMenu() is invoked by
    // NAME via QMetaObject::invokeMethod despite being a private slot:
    // Qt's meta-object system permits that for anything declared as a
    // slot regardless of C++ access, since dispatch goes through the
    // moc-generated thunk rather than a direct member-function call.
    // Deliberately exploits the exact window the bug opens (a second
    // navigation action taken between the file op's refresh and the real
    // navigation's own response) rather than a fixed delay: a raw
    // QObject::connect straight to the backend's directoryListed, made
    // AFTER FilePaneWidget's own connection (wired up in its
    // constructor), is guaranteed to run SECOND for the same emission —
    // direct, same-thread connections fire in connection order — so this
    // handler only ever sees FilePaneWidget's state exactly as
    // onDirectoryListed() just left it. ----------
    const QString fileOpRaceBase = "/tmp/nav_test_fileop_race";
    // Unlike every other fixture base in this file (plain directories,
    // or files always reopened WriteOnly so they're implicitly
    // overwritten), "current/racefile.txt" below is created by the real
    // app's own "New File..." UI path, not by this setup code — so a
    // previous run that got killed before cleanup (e.g. this exact test
    // hanging, see below) leaves it behind. A second run then hits a
    // real "file already exists" error, which pops a QMessageBox
    // dialogPump below doesn't expect (only QInputDialog) — menu.exec()
    // then blocks forever waiting for a dialog that will never close.
    // removeRecursively() first makes every run start genuinely fresh,
    // regardless of what an earlier run left behind.
    QDir(fileOpRaceBase).removeRecursively();
    QDir().mkpath(fileOpRaceBase + "/current");
    QDir().mkpath(fileOpRaceBase + "/other");
    auto *fileOpRacePane = new FilePaneWidget(new LocalBackend());

    // Auto-answers whichever dialog showContextMenu()'s "New File..." path
    // opens: the QMenu itself (click "New File..."), then the
    // QInputDialog it opens for the name (fill it in and accept).
    auto *dialogPump = new QTimer(&app);
    dialogPump->setInterval(5);
    QObject::connect(dialogPump, &QTimer::timeout, &app, [&]() {
        if (auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
            for (QAction *action : popup->actions()) {
                if (action->text() == "New File...") {
                    // QAction::trigger() alone does NOT make menu.exec()
                    // return this action — that only happens through
                    // QMenu's own internal activation path, driven by a
                    // real click/key event. QTest::mouseClick() injects a
                    // genuine QMouseEvent at the action's own on-screen
                    // geometry, which QMenu's event handling processes
                    // exactly like a real click would (confirmed with a
                    // standalone probe: raw trigger() left menu.exec()
                    // blocked forever under the offscreen platform, this
                    // doesn't).
                    QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier,
                                       popup->actionGeometry(action).center());
                    return;
                }
            }
        }
        if (auto *inputDialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
            inputDialog->setTextValue("racefile.txt");
            inputDialog->accept();
        }
    });
    dialogPump->start();

    // ---------- Regression: a failed Back/Forward navigation must not
    // leave m_navigatingHistory stuck true — a real bug found by code
    // review. Before this fix, only onDirectoryListed() (a CONFIRMED
    // successful listing) ever cleared it; a failed navigation reports
    // via connectionFailed instead (see m_navigationInFlight's own doc
    // comment on why), which never reached that reset. The next
    // successful fresh navigation would then wrongly take
    // onDirectoryListed()'s "this was Back/Forward" branch and skip
    // pushing itself onto history at all. Chained directly after
    // fileOpRace below (a plain function call, not its own independently
    // wall-clock-scheduled QTimer::singleShot) rather than run
    // concurrently with it: fileOpRace's own QMenu::exec()/
    // QInputDialog::exec()/drain.exec() calls pump the WHOLE
    // application's event queue, not just events relevant to that phase,
    // so a separately-scheduled timer landing during any of that nesting
    // would fire from deep inside it — confirmed empirically causing
    // exactly that interleaving in ~30% of runs, with two separate
    // app.exit() calls racing (the LAST one to run wins, silently
    // discarding whichever phase called it first). ----------
    const QString failedBackBase = "/tmp/nav_test_failed_back";
    QDir().mkpath(failedBackBase + "/gone");
    QDir().mkpath(failedBackBase + "/stay");
    QDir().mkpath(failedBackBase + "/c");
    auto *failedBackPane = new FilePaneWidget(new LocalBackend());

    // ---------- Regression: promptAndRename() must act on the directory
    // that was actually on screen when the context menu was opened, not
    // whatever currentDirectory() returns once its own modal QInputDialog
    // finally closes — a real bug found by code review. showContextMenu()
    // captures both `selected` and `directory` in the same snapshot,
    // before menu.exec(); this phase proves a navigation completing while
    // the menu is still open (before Rename is even clicked) does NOT
    // retarget the eventual rename to the new directory. Defined here
    // (before runFailedBackPhase, which calls it at its own end) so it
    // chains in — same reasoning as fileOpRace chaining into
    // runFailedBackPhase: this phase's own QMenu/QInputDialog nesting
    // would otherwise race an independently wall-clock-scheduled phase. ----------
    const QString renameRaceBase = "/tmp/nav_test_rename_race";
    // Same reasoning as fileOpRaceBase above: "renamed.txt" is created
    // by the real app rename path, not this setup code, so a previous
    // interrupted run can leave it behind. A stale renamed.txt already
    // sitting in dirA makes THIS run's real rename of victim.txt onto
    // that same name fail (the destination already exists), which then
    // fails these checks for a reason that has nothing to do with the
    // race this phase actually tests. removeRecursively() first avoids
    // that entirely.
    QDir(renameRaceBase).removeRecursively();
    QDir().mkpath(renameRaceBase + "/dirA");
    QDir().mkpath(renameRaceBase + "/dirB");
    {
        QFile victim(renameRaceBase + "/dirA/victim.txt");
        victim.open(QIODevice::WriteOnly);
        victim.write("hello");
    }
    auto *renameRacePane = new FilePaneWidget(new LocalBackend());
    auto *renameRaceDialogPump = new QTimer(&app);
    renameRaceDialogPump->setInterval(5);

    // Forward-declared (rather than each phase's lambda simply being
    // reordered ahead of the one it chains into) since renameRace below
    // calls pathBarGuard, and both need to exist as names before either
    // lambda body is compiled — std::function lets each one be assigned
    // after the other's declaration without fighting the textual order
    // this file's comments already establish (dialog automation next to
    // its own explanation).
    std::function<void()> runRenameRacePhase;
    std::function<void()> runPathBarGuardPhase;

    runRenameRacePhase = [&]() {
        renameRacePane->navigateTo(renameRaceBase + "/dirA");
        waitUntil([&]() { return renameRacePane->currentDirectory() == renameRaceBase + "/dirA"; });

        auto *view = renameRacePane->findChild<QTreeView *>();
        int victimRow = -1;
        for (int row = 0; row < view->model()->rowCount(); ++row) {
            if (view->model()->data(view->model()->index(row, 0)).toString() == "victim.txt") {
                victimRow = row;
                break;
            }
        }
        check("renameRace: found victim.txt's row to select", victimRow >= 0);
        view->selectionModel()->select(view->model()->index(victimRow, 0),
                                        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

        bool navigatedDuringMenu = false;
        bool renameClicked = false;
        QObject::connect(renameRaceDialogPump, &QTimer::timeout, &app, [&]() {
            if (auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                if (!navigatedDuringMenu) {
                    navigatedDuringMenu = true;
                    // The race itself: a navigation completes WHILE the
                    // context menu (opened against dirA) is still open,
                    // before Rename has even been clicked yet.
                    renameRacePane->navigateTo(renameRaceBase + "/dirB");
                    return;
                }
                if (!renameClicked && renameRacePane->currentDirectory() == renameRaceBase + "/dirB") {
                    renameClicked = true;
                    for (QAction *action : popup->actions()) {
                        if (action->text() == "Rename...") {
                            QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier,
                                               popup->actionGeometry(action).center());
                            return;
                        }
                    }
                }
                return;
            }
            if (auto *inputDialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
                inputDialog->setTextValue("renamed.txt");
                inputDialog->accept();
                return;
            }
            // Defensive: dismiss any unexpected onFileOperationFailed()
            // warning rather than hanging the whole test on it — this
            // should never actually appear once the fix is in place (the
            // rename correctly targets dirA, where victim.txt genuinely
            // exists), but manually verifying this test against the
            // PRE-fix code confirmed it otherwise pops exactly this
            // (renameEntry() failing on a source that doesn't exist in
            // dirB), and a test that just hangs on an unexpected dialog
            // is worse than one that fails cleanly.
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->accept();
        });
        renameRaceDialogPump->start();

        QMetaObject::invokeMethod(renameRacePane, "showContextMenu", Qt::DirectConnection,
                                   Q_ARG(QPoint, QPoint(5, 5)));

        QEventLoop drain;
        QTimer::singleShot(300, &drain, &QEventLoop::quit);
        drain.exec();

        renameRaceDialogPump->stop();

        check("renameRace: the race navigation to dirB genuinely completed before Rename was "
              "clicked (otherwise this isn't exercising the race at all)",
              renameRacePane->currentDirectory() == renameRaceBase + "/dirB");
        check("renameRace: victim.txt is gone from dirA — the rename it was selected from",
              !QFile::exists(renameRaceBase + "/dirA/victim.txt"));
        check("renameRace: the renamed file landed in dirA — the directory actually on screen "
              "when the menu was opened, not dirB (where the pane ended up by the time the "
              "modal dialog closed)",
              QFile::exists(renameRaceBase + "/dirA/renamed.txt"));
        check("renameRace: nothing was created in dirB — confirms the rename wasn't silently "
              "retargeted to the directory current at dialog-close time",
              !QFile::exists(renameRaceBase + "/dirB/renamed.txt"));

        runPathBarGuardPhase();
    };

    // ---------- Regression: pressing Enter in the path bar while a
    // navigation is already in flight must give real feedback, not
    // silently do nothing — a real bug found by code review.
    // onPathBarReturnPressed() used to just call navigateTo()
    // unconditionally, unlike goBack()/goForward() which self-check
    // m_navigationInFlight first; navigateTo()'s own internal guard
    // still safely drops the request either way, so nothing was ever
    // corrupted, but the person got no indication their Enter press did
    // nothing — and the ORIGINAL in-flight navigation's response then
    // overwrites the path bar with ITS path once it lands, silently
    // erasing whatever they'd just typed. Chained after runRenameRacePhase
    // for the same reason every other phase in this file already follows. ----------
    const QString pathBarGuardBase = "/tmp/nav_test_pathbar_guard";
    QDir().mkpath(pathBarGuardBase + "/a");
    QDir().mkpath(pathBarGuardBase + "/b");
    auto *pathBarGuardPane = new FilePaneWidget(new LocalBackend());

    runPathBarGuardPhase = [&]() {
        pathBarGuardPane->navigateTo(pathBarGuardBase + "/a");
        waitUntil([&]() { return pathBarGuardPane->currentDirectory() == pathBarGuardBase + "/a"; });

        auto *statusLabel = pathBarGuardPane->findChild<QLabel *>();
        check("pathBarGuard: found the pane's status label", statusLabel != nullptr);
        const QString statusBeforeAttempt = statusLabel ? statusLabel->text() : QString();

        // Start a real navigation and DON'T wait for it — m_navigationInFlight
        // is still genuinely true for what follows, no mock/manual flag flip.
        pathBarGuardPane->navigateTo(pathBarGuardBase + "/b");

        // Simulate pressing Enter in the path bar while that's still
        // resolving — onPathBarReturnPressed() is a private slot, but
        // Qt's meta-object system can still invoke it by name regardless
        // of C++ access, same technique already used for showContextMenu()
        // above.
        QMetaObject::invokeMethod(pathBarGuardPane, "onPathBarReturnPressed", Qt::DirectConnection);

        check("pathBarGuard: the in-flight navigation produced visible feedback instead of "
              "silently doing nothing",
              statusLabel && statusLabel->text() != statusBeforeAttempt
                  && statusLabel->text().contains("already in progress"));

        waitUntil([&]() { return pathBarGuardPane->currentDirectory() == pathBarGuardBase + "/b"; });
        check("pathBarGuard: the original in-flight navigation still completed normally — "
              "the refused Enter press didn't corrupt or block it",
              pathBarGuardPane->currentDirectory() == pathBarGuardBase + "/b");

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    };

    auto runFailedBackPhase = [&]() {
        failedBackPane->navigateTo(failedBackBase + "/gone");
        waitUntil([&]() { return failedBackPane->currentDirectory() == failedBackBase + "/gone"; });

        failedBackPane->navigateTo(failedBackBase + "/stay");
        waitUntil([&]() { return failedBackPane->currentDirectory() == failedBackBase + "/stay"; });

        // Remove the directory goBack() is about to target — real, on-disk
        // state, not a mock — so its own navigateTo() genuinely fails.
        QDir(failedBackBase).rmdir("gone");

        failedBackPane->goBack();

        // The failed navigation reports via connectionFailed, not
        // directoryListed, so currentDirectory() never changes — nothing
        // to poll there. LocalBackend's failure path is fully synchronous
        // (no real I/O wait, just a QDir::exists() check), so a short
        // fixed drain is enough for the queued call to actually run,
        // same reasoning as fileOpRace's drain below.
        QEventLoop drain;
        QTimer::singleShot(100, &drain, &QEventLoop::quit);
        drain.exec();

        // Now issue a FRESH, unrelated navigation — the actual condition
        // that exposes whether m_navigatingHistory was left stuck true.
        failedBackPane->navigateTo(failedBackBase + "/c");
        waitUntil([&]() { return failedBackPane->currentDirectory() == failedBackBase + "/c"; });

        check("failedBack: landed on the fresh navigation's real target",
              failedBackPane->currentDirectory() == failedBackBase + "/c");
        check("failedBack: the fresh navigation was correctly pushed onto history — forward is "
              "empty right after landing on it (pre-fix, m_navigatingHistory left stuck true by "
              "the failed goBack() meant nothing got pushed, leaving a stale, wrong forward entry)",
              !failedBackPane->canGoForward());

        runRenameRacePhase();
    };

    QTimer::singleShot(2800, &app, [&]() {
        fileOpRacePane->navigateTo(fileOpRaceBase + "/current");
        waitUntil([&]() { return fileOpRacePane->currentDirectory() == fileOpRaceBase + "/current"; });

        // Armed now, right before triggering the race, so it only ever
        // fires for what follows below — not the setup navigation above.
        bool exploited = false;
        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = QObject::connect(fileOpRacePane->backend(), &RemoteBackend::directoryListed,
                                  &app, [&, conn](const QString &, const QList<RemoteEntry> &) {
            if (exploited)
                return;
            exploited = true;
            QObject::disconnect(*conn);
            // The exploit: attempt a second navigation action right in
            // this handler, which — for the file op's own refresh below —
            // runs exactly when FilePaneWidget's own onDirectoryListed()
            // has JUST finished handling that same emission (see this
            // block's own header comment on why connection order
            // guarantees that). Pre-fix, m_navigationInFlight was wrongly
            // cleared by that refresh even though the navigateTo() issued
            // right after this block is still genuinely pending, so
            // goBack() here wrongly succeeds and corrupts history;
            // post-fix, the flag is untouched (still true), so this is
            // correctly refused — a no-op.
            fileOpRacePane->goBack();
        });

        // Dispatches createFile() (queued) via the REAL "New File..."
        // context-menu path, driven by dialogPump above.
        QMetaObject::invokeMethod(fileOpRacePane, "showContextMenu", Qt::DirectConnection,
                                   Q_ARG(QPoint, QPoint(5, 5)));

        // Immediately navigate elsewhere — no event-loop turn in between,
        // so this queues behind the file op's own refresh (both target
        // the same backend, processed strictly in dispatch order).
        fileOpRacePane->navigateTo(fileOpRaceBase + "/other");

        // Local, in-process queued calls with no real I/O — draining for
        // a fixed 200ms is not a flaky network/disk timing assumption,
        // just headroom for every already-posted event above to run.
        QEventLoop drain;
        QTimer::singleShot(200, &drain, &QEventLoop::quit);
        drain.exec();

        dialogPump->stop();

        check("fileOpRace: the created file actually exists (dialog automation genuinely ran)",
              QFile::exists(fileOpRaceBase + "/current/racefile.txt"));
        check("fileOpRace: the real navigateTo(\"other\") landed there — not silently overridden "
              "by the exploited goBack() call's own navigation",
              fileOpRacePane->currentDirectory() == fileOpRaceBase + "/other");
        check("fileOpRace: history was NOT corrupted — still able to go back",
              fileOpRacePane->canGoBack());

        fileOpRacePane->goBack();
        waitUntil([&]() { return fileOpRacePane->currentDirectory() != fileOpRaceBase + "/other"; });
        check("fileOpRace: goBack() from \"other\" lands cleanly on \"current\", "
              "not skipped past it by the exploited call's own history mutation",
              fileOpRacePane->currentDirectory() == fileOpRaceBase + "/current");

        runFailedBackPhase();
    });

    return app.exec();
}
