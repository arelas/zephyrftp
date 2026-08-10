// Headless smoke test: exercises the real thread lifecycle (moveToThread,
// start, connectToHost -> connect() fails fast against a closed local port,
// connectionFailed signal delivered back to GUI thread, then teardown via
// quit()/wait()) without needing a live SFTP server or a display.
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QDebug>
#include "ui/MainWindow.h"
#include "ui/FilePaneWidget.h"
#include "ui/HostKeyVerifier.h"
#include "backends/SftpBackend.h"
#include "backends/SftpCredentials.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    MainWindow window;
    window.show();

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
        qDebug() << (failureSignalReceived ? "[smoke test] PASS" : "[smoke test] FAIL: no connectionFailed signal received");
        app.exit(failureSignalReceived ? 0 : 1);
    });

    return app.exec();
}
