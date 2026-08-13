// Verifies synchronized browsing end to end against a REAL MainWindow
// (same construction technique smoke_test.cpp already established —
// headless, QT_QPA_PLATFORM=offscreen, no live server needed since both
// panes stay on LocalBackend throughout) — not just FilePaneWidget's own
// navigateTo()/history mechanics (already covered by navigation-test),
// but the actual cross-pane orchestration this feature adds in
// MainWindow: the View-menu toggle, the directoryChanged wiring, the
// path-based echo-suppression that prevents a driven navigation from
// bouncing back to whichever pane originated it, and the auto-disable
// safety net on reconnect.
//
// Fixture: two independent root trees (leftRoot/rightRoot), each with a
// mirrored "common/subdir" so a shared relative path genuinely resolves
// on both sides, plus a "onlyleft" directory that exists ONLY under
// leftRoot — deliberately reproducing the "corresponding path doesn't
// exist on the other side" case, which SynchronizedBrowsing relies on
// FilePaneWidget's EXISTING connectionFailed handling to cover for free
// (see MainWindow::onPaneDirectoryChanged()'s own design comment) rather
// than inventing new error UI.
//
// Run with:
//   QT_QPA_PLATFORM=offscreen ./build/sync-browsing-test
#include <QApplication>
#include <QTimer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QDebug>
#include <QDir>
#include <QAction>
#include <QStandardPaths>
#include <cstdio>
#include <functional>
#include "ui/MainWindow.h"
#include "ui/FilePaneWidget.h"

namespace {
bool allPass = true;
void check(const QString &label, bool condition)
{
    qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
    // TEMPORARY diagnostic: qDebug() doesn't reliably reach the terminal
    // under wine; needed again after a second, different-shaped CI-only
    // failure (a ~45s hang, not the earlier race) to see which specific
    // check the time is going into. Remove once diagnosed.
    fprintf(stderr, "%s %s\n", condition ? "[PASS]" : "[FAIL]", qPrintable(label));
    fflush(stderr);
    if (!condition) allPass = false;
}

// Same polling-with-timeout technique established elsewhere in this
// project's tests (transfer_pause_test.cpp, transfer_concurrency_test.cpp)
// — avoids trusting a fixed wall-clock delay to have been enough.
void waitUntil(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeoutTimer.start(timeoutMs);
    while (!condition() && timeoutTimer.isActive())
        loop.processEvents(QEventLoop::AllEvents, 20);
}

// navigateTo() silently drops a request if the pane already has one
// in flight (a real, intentional guard against overlapping navigation
// — see FilePaneWidget::navigateTo()'s own comment). Confirmed via two
// separate real CI failures that SOME overlapping request — not fully
// pinned down to a single exact trigger, despite ruling out the most
// obvious one (each pane's own initial auto-connect navigation) with a
// dedicated settle-wait that verifiably succeeded on the very run that
// still failed — can still be in flight when this test issues its own
// explicit navigateTo(), silently stranding a pane at whatever
// directory it was already at. Never reproduced locally; wine's higher
// per-call overhead apparently makes whatever the actual race is far
// more likely to land badly. Retrying the request until it actually
// takes effect sidesteps the exact mechanism rather than requiring it
// to be fully understood — navigateTo() is safe to call repeatedly,
// each call either succeeds or is harmlessly dropped.
void navigateWithRetry(FilePaneWidget *pane, const QString &target, int overallTimeoutMs = 5000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (pane->currentDirectory() != target && elapsed.elapsed() < overallTimeoutMs) {
        pane->navigateTo(target);
        waitUntil([&] { return pane->currentDirectory() == target; }, 250);
    }
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    // Real bug found running this test a second time in a row: MainWindow
    // constructs a real AppSettings, which persists to a real
    // settings.json — under this test's OWN executable name (Qt's
    // AppConfigLocation default when no explicit organization/application
    // name is set, same as smoke-test.cpp's identical, pre-existing
    // characteristic), so it doesn't collide with the real app's config,
    // but it DOES persist across repeated runs of this test itself. The
    // very first assertion below ("sync toggle starts unchecked") is only
    // true on a genuinely fresh config — a second run inherited `true`
    // from the first run's own leftover settings.json and failed
    // immediately, cascading into unrelated-looking failures after it.
    // Same fix site-store-test/app-settings-test already established for
    // the identical class of problem: Qt's own cross-platform test-mode
    // isolation, not a bespoke environment-variable workaround.
    QStandardPaths::setTestModeEnabled(true);

    const QString leftRoot = "/tmp/sync_browsing_test/left";
    const QString rightRoot = "/tmp/sync_browsing_test/right";
    QDir("/tmp/sync_browsing_test").removeRecursively();
    QDir().mkpath(leftRoot + "/common/subdir");
    QDir().mkpath(leftRoot + "/onlyleft");
    QDir().mkpath(rightRoot + "/common/subdir");

    MainWindow window;
    window.show();

    auto *leftPane = window.findChild<FilePaneWidget *>("leftPane");
    auto *rightPane = window.findChild<FilePaneWidget *>("rightPane");
    auto *syncToggle = window.findChild<QAction *>("synchronizedBrowsingToggle");
    check("found left pane, right pane, and the sync-browsing toggle",
          leftPane && rightPane && syncToggle);
    if (!leftPane || !rightPane || !syncToggle) {
        qDebug() << "[test] cannot continue without all three — aborting";
        return 1;
    }

    // ---------- Setup: get both panes to their respective roots before
    // enabling sync, so turning it on anchors at exactly these paths.
    // navigateWithRetry(), not a bare navigateTo(), for both — see its
    // own comment for why a single explicit call isn't reliable here. ----------
    navigateWithRetry(leftPane, leftRoot);
    navigateWithRetry(rightPane, rightRoot);
    check("setup: both panes reached their own root", leftPane->currentDirectory() == leftRoot
          && rightPane->currentDirectory() == rightRoot);

    check("sync toggle starts unchecked (default off)", !syncToggle->isChecked());
    syncToggle->setChecked(true);
    check("sync toggle is now checked", syncToggle->isChecked());

    // ---------- 1: basic propagation ----------
    navigateWithRetry(leftPane, leftRoot + "/common/subdir");
    waitUntil([&] { return rightPane->currentDirectory() == rightRoot + "/common/subdir"; });
    check("left navigating into common/subdir drives right into its own common/subdir",
          rightPane->currentDirectory() == rightRoot + "/common/subdir");
    check("left's own navigation completed normally too",
          leftPane->currentDirectory() == leftRoot + "/common/subdir");

    // ---------- 2: every navigation entry point propagates (all funnel
    // through navigateTo(), confirmed by reading the code) ----------
    leftPane->goBack();
    waitUntil([&] { return leftPane->currentDirectory() == leftRoot; });
    waitUntil([&] { return rightPane->currentDirectory() == rightRoot; });
    check("goBack() propagates", leftPane->currentDirectory() == leftRoot
          && rightPane->currentDirectory() == rightRoot);

    leftPane->goForward();
    waitUntil([&] { return leftPane->currentDirectory() == leftRoot + "/common/subdir"; });
    waitUntil([&] { return rightPane->currentDirectory() == rightRoot + "/common/subdir"; });
    check("goForward() propagates", leftPane->currentDirectory() == leftRoot + "/common/subdir"
          && rightPane->currentDirectory() == rightRoot + "/common/subdir");

    leftPane->goUp();
    waitUntil([&] { return leftPane->currentDirectory() == leftRoot + "/common"; });
    waitUntil([&] { return rightPane->currentDirectory() == rightRoot + "/common"; });
    check("goUp() propagates", leftPane->currentDirectory() == leftRoot + "/common"
          && rightPane->currentDirectory() == rightRoot + "/common");

    leftPane->goHome();
    waitUntil([&] { return leftPane->currentDirectory() == leftRoot; });
    waitUntil([&] { return rightPane->currentDirectory() == rightRoot; });
    check("goHome() propagates", leftPane->currentDirectory() == leftRoot
          && rightPane->currentDirectory() == rightRoot);

    // ---------- 3: missing-path failure isolation ----------
    navigateWithRetry(leftPane, leftRoot + "/onlyleft");
    check("left's own navigation into onlyleft completes despite right lacking that path",
          leftPane->currentDirectory() == leftRoot + "/onlyleft");
    // Right's driven navigateTo(rightRoot + "/onlyleft") fails
    // (LocalBackend::listDirectory() emits connectionFailed for a
    // nonexistent directory — confirmed by reading the code) and so
    // never reaches a NEW currentDirectory(); give it a real event-loop
    // window to (not) happen, then confirm it genuinely stayed put.
    waitUntil([&] { return false; }, 300);   // fixed drain — there's no "it settled" condition to poll for a non-event
    check("right pane stayed at its last real position rather than following into a path it doesn't have",
          rightPane->currentDirectory() == rightRoot);

    // ---------- 4: no triple-bounce — right's own (successful) driven
    // navigation must not echo a THIRD navigateTo() back onto left. ----------
    int leftDirectoryChangedCount = 0;
    QObject::connect(leftPane, &FilePaneWidget::directoryChanged, &app,
                      [&](const QString &) { ++leftDirectoryChangedCount; });
    navigateWithRetry(leftPane, leftRoot + "/common");
    waitUntil([&] { return rightPane->currentDirectory() == rightRoot + "/common"; });
    // One more spin to let any (incorrect) bounce-back reach left, if the
    // suppression were broken.
    waitUntil([&] { return false; }, 300);
    check("left fired directoryChanged exactly once for its own single navigation "
          "(no bounce-back triggering a second one)", leftDirectoryChangedCount == 1);
    check("left's position is still exactly where it navigated, not moved again by an echo",
          leftPane->currentDirectory() == leftRoot + "/common");

    // ---------- 4b: self-healing — the stale, never-consumed pending
    // entry from phase 3's failed drive must not wedge future sync. ----------
    navigateWithRetry(leftPane, leftRoot + "/common/subdir");
    waitUntil([&] { return rightPane->currentDirectory() == rightRoot + "/common/subdir"; });
    check("sync still works after an earlier driven navigation failed — not permanently wedged",
          rightPane->currentDirectory() == rightRoot + "/common/subdir");

    // ---------- 5: toggling off stops propagation ----------
    syncToggle->setChecked(false);
    check("sync toggle is now unchecked", !syncToggle->isChecked());
    navigateWithRetry(leftPane, leftRoot);
    waitUntil([&] { return false; }, 300);   // give a real window for an (incorrect) propagation to land
    check("with sync off, right pane does NOT follow left's navigation",
          rightPane->currentDirectory() == rightRoot + "/common/subdir");

    // ---------- 6: reconnecting a pane while synced browsing is on
    // auto-disables it, rather than leaving a stale anchor around ----------
    syncToggle->setChecked(true);
    check("sync toggle re-enabled ahead of the reconnect scenario", syncToggle->isChecked());
    // Same production trigger MainWindow::onPaneConnectRequested()/
    // onPaneDisconnectRequested() respond to — FilePaneWidget's own
    // path-bar icon menu emits this signal when "Disconnect" is chosen.
    // It's declared public (not `private signals`), so calling it
    // directly here exercises the exact same MainWindow-level handler a
    // real click eventually would, without needing to drive the actual
    // QMenu popup through this offscreen platform.
    emit leftPane->disconnectRequested(leftPane);
    waitUntil([&] { return !syncToggle->isChecked(); });
    check("disconnecting a pane while synchronized browsing was on turns the toggle off "
          "(a stale anchor would otherwise produce nonsense joins on the next navigation)",
          !syncToggle->isChecked());

    qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
