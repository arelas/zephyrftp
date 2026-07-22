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
#include "backends/SftpBackend.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    MainWindow window;
    window.show();

    bool failureSignalReceived = false;

    // Port 1 on localhost: nothing listens there, so connect() fails almost
    // instantly with ECONNREFUSED instead of timing out.
    auto *backend = new SftpBackend("127.0.0.1", 1, "testuser", "testpass");
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
    QTimer::singleShot(1000, &app, [&]() {
        qDebug() << "[smoke test] tearing down worker thread...";
        backend->deleteLater();
        thread->quit();
        thread->wait();
        qDebug() << "[smoke test] thread->wait() returned, isFinished() ="
                  << thread->isFinished();
        qDebug() << (failureSignalReceived ? "[smoke test] PASS" : "[smoke test] FAIL: no connectionFailed signal received");
        app.quit();
    });

    return app.exec();
}
