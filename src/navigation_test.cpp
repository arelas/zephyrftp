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
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

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

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}
