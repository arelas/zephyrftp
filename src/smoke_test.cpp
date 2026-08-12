// Headless smoke test: exercises the real thread lifecycle (moveToThread,
// start, connectToHost -> connect() fails fast against a closed local port,
// connectionFailed signal delivered back to GUI thread, then teardown via
// quit()/wait()) without needing a live SFTP server or a display.
//
// Also covers a real MainWindow-level regression found by code review:
// closeEvent() must REFUSE to close (QCloseEvent::ignore()) while either
// pane is still mid-connect, rather than silently letting the window (and,
// via main.cpp's default quitOnLastWindowClosed, the whole app) close with
// a worker QThread still running and still parented to it — which used to
// reintroduce the exact qFatal("QThread: Destroyed while thread is still
// running") crash this whole mechanism exists to prevent (see
// MainWindow.h's own header doc comment). Tested with a fake backend
// whose connectToHost() deliberately never emits connected/connectionFailed
// — not a real hang, but the same observable state (isConnecting() stuck
// true) without depending on real network timing or a live server.
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QDebug>
#include "ui/MainWindow.h"
#include "ui/FilePaneWidget.h"
#include "ui/HostKeyVerifier.h"
#include "backends/SftpBackend.h"
#include "backends/SftpCredentials.h"
#include "backends/LocalBackend.h"

namespace {
// Deliberately never emits connected() or connectionFailed() — keeps
// FilePaneWidget::isConnecting() stuck true for as long as this backend
// stays attached, standing in for a connection attempt that hasn't
// resolved yet without needing real (and unreliably-timed) network I/O.
class FakeNeverConnectsBackend : public RemoteBackend {
    Q_OBJECT
public:
    explicit FakeNeverConnectsBackend(QObject *parent = nullptr) : RemoteBackend(parent) {}

    QString currentPath() const override { return QString(); }
    bool isLocalFilesystem() const override { return false; }
    QString connectionIdentity() const override { return QStringLiteral("fake-never-connects"); }
    void requestCancel() override {}
    void requestPause() override {}

public slots:
    void connectToHost() override {}   // deliberately does nothing — see class doc comment
    void listDirectory(const QString &) override {}
    void downloadFile(const QString &, const QString &, qint64 = 0) override {}
    void uploadFile(const QString &, const QString &, qint64 = 0) override {}
    void deleteEntry(const QString &, bool) override {}
    void renameEntry(const QString &, const QString &) override {}
    void moveEntry(const QString &, const QString &, int requestId) override {
        emit entryMoveFailed(QStringLiteral("Not implemented"), requestId);
    }
    void createDirectory(const QString &) override {}
    void createFile(const QString &) override {}
    void setPermissions(const QString &, int) override {}
    void listDirectoryForEnumeration(const QString &, int) override {}
    void checkExists(const QString &, int) override {}
};
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    // Without this, the close()/reopen sequence in the closeEvent()
    // regression phase below would trigger Qt's default "quit when the
    // last window closes" the instant the window genuinely closes (once
    // no pane is connecting), ending the whole test's event loop before
    // the later async SFTP phase's 4-second timer ever gets to fire —
    // same fix navigation-test's own header comment explains in more
    // detail for the identical reason.
    app.setQuitOnLastWindowClosed(false);
    MainWindow window;
    window.show();

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    // ---------- Regression: closeEvent() must refuse to close while a
    // pane is still mid-connect (see this file's own header comment). ----------
    {
        auto panes = window.findChildren<FilePaneWidget *>();
        check("found both panes to test against", panes.size() == 2);
        FilePaneWidget *pane = panes.isEmpty() ? nullptr : panes.first();

        auto *stuckBackend = new FakeNeverConnectsBackend();
        auto *stuckThread = new QThread(&window);   // matches startConnection()'s own parenting exactly
        stuckBackend->moveToThread(stuckThread);
        stuckThread->start();

        // setBackend() sets m_connecting = true synchronously, before
        // connectToHost() is even dispatched (Qt::QueuedConnection, not
        // run until the next event-loop turn) — so isConnecting() is
        // already true here, no event-loop turn needed to arm this.
        pane->setBackend(stuckBackend, stuckThread);
        check("pane reports isConnecting() immediately after setBackend()", pane->isConnecting());

        const bool closeWasAccepted = window.close();
        check("closeEvent() refused to close while a pane is still connecting",
              !closeWasAccepted);
        check("window is still open after the refused close", window.isVisible());

        // Clean teardown for the rest of this test — swap back to a real
        // LocalBackend via the same setBackend() teardown path production
        // code uses (deleteLater() + thread->quit()/wait()), now that
        // isConnecting() no longer needs to stay true for anything else
        // here. Safe to do directly (bypassing MainWindow's own
        // disconnectPane(), which would itself refuse for the same
        // isConnecting() reason): FakeNeverConnectsBackend never enters a
        // blocking call, so quit()/wait() below returns immediately, no
        // hang risk the way a real stuck backend would have.
        pane->setBackend(new LocalBackend(), nullptr);

        const bool closeAfterCleanup = window.close();
        check("closeEvent() allows closing again once no pane is connecting",
              closeAfterCleanup);
        // Reopen for the rest of this test — this phase's own close()
        // calls above are otherwise unrelated to the SFTP phase below.
        window.show();
    }

    bool failureSignalReceived = false;

    // Port 1 on localhost: nothing listens there, so connect() fails almost
    // instantly with ECONNREFUSED instead of timing out. Never reaches
    // host-key verification or auth, so a real HostKeyVerifier is
    // constructed here mainly to match production wiring, not because
    // this test exercises it.
    auto *hostKeyVerifier = new HostKeyVerifier(&window);
    SftpCredentials creds;
    creds.host = "127.0.0.1";
    creds.port = 1;
    creds.username = "testuser";
    creds.authMethod = SftpAuthMethod::Password;
    creds.password = "testpass";

    auto *backend = new SftpBackend(creds, hostKeyVerifier);
    auto *thread = new QThread(&window);
    backend->moveToThread(thread);

    QObject::connect(backend, &RemoteBackend::connectionFailed, &app,
        [&](const QString &reason) {
            failureSignalReceived = true;
            qDebug() << "[smoke test] connectionFailed signal received on GUI thread:" << reason;
            qDebug() << "[smoke test] receiving thread == main thread?"
                      << (QThread::currentThread() == qApp->thread());
        });

    thread->start();
    qDebug() << "[smoke test] worker thread started, id =" << thread;
    QMetaObject::invokeMethod(backend, "connectToHost", Qt::QueuedConnection);

    // Give the worker thread a moment to run connectToHost(), then tear down
    // cleanly and report pass/fail.
    //
    // A real bug found running this on native Windows (not just under
    // CI's wine emulation): 1000ms assumed ECONNREFUSED on a closed local
    // port arrives almost instantly everywhere, which holds on Linux but
    // not on native Windows, where refused-connection latency is ~2.2s
    // (confirmed via a direct .NET TcpClient probe). With the old 1000ms
    // value, this timer could fire while connectToHost() was still
    // blocked inside its connect() syscall on the worker thread —
    // thread->quit() can't stop a loop that isn't processing anything, so
    // thread->wait() below would then block THIS (main) thread for
    // however much longer Windows' TCP stack took to actually give up.
    // Widened to comfortably exceed that real, measured latency.
    QTimer::singleShot(4000, &app, [&]() {
        qDebug() << "[smoke test] tearing down worker thread...";
        backend->deleteLater();
        thread->quit();
        thread->wait();
        // Belt-and-suspenders on top of the widened timer above: even
        // once connectToHost() has genuinely finished and emitted
        // connectionFailed, wait() only guarantees the WORKER thread's
        // event loop has stopped — it does not pump THIS (main) thread's
        // own event queue, so the queued cross-thread delivery of that
        // signal could still be sitting unprocessed the instant wait()
        // returns. Flushing it explicitly here removes that race
        // entirely rather than just making it unlikely with a wide
        // enough timer.
        QCoreApplication::processEvents();
        qDebug() << "[smoke test] thread->wait() returned, isFinished() ="
                  << thread->isFinished();
        check("connectionFailed signal received", failureSignalReceived);
        qDebug() << (allPass ? "[smoke test] PASS" : "[smoke test] FAIL");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}

#include "smoke_test.moc"
