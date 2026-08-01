// Exercises SftpBackend's PublicKey auth path against a REAL server —
// not a mock, not just "the code compiles" — closing the "public-key
// authentication has never been tried against a real key file" gap
// flagged in ARCHITECTURE.md. Needs tools/local-test-servers/start-sftp-pubkey.sh
// already running; this is NOT one of the ten fully-self-contained
// EXCLUDE_FROM_ALL test targets (it has an external precondition those
// deliberately don't), so it isn't part of that suite or CI.
//
// Run with:
//   tools/local-test-servers/start-sftp-pubkey.sh
//   cmake --build build --target verify-sftp-pubkey
//   QT_QPA_PLATFORM=offscreen SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp \
//       ./build/verify-sftp-pubkey
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QMessageBox>
#include <QAbstractButton>
#include <cstdio>
#include "ui/HostKeyVerifier.h"
#include "backends/SftpBackend.h"
#include "backends/SftpCredentials.h"

namespace {
// The host-key TOFU prompt is a real, synchronous, blocking-queued
// QMessageBox::question() call from the worker thread onto this one
// (see HostKeyVerifier.cpp) — exec() pumps the event loop internally,
// same technique already proven in conflict_resolution_test.cpp for
// driving a still-open QMessageBox from a QTimer.
void autoAcceptHostKeyPrompt()
{
    QTimer::singleShot(50, qApp, [] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (box) {
            for (QAbstractButton *button : box->buttons()) {
                if (box->buttonRole(button) == QMessageBox::YesRole) {
                    button->click();
                    return;
                }
            }
        }
        // Not up yet — this is a genuinely first-ever connection to this
        // host/port combo (a fresh scratch known_hosts each run would
        // make it so every time, but XDG_CONFIG_HOME isn't overridden
        // here — see the header comment on why that's fine for this
        // harness), so keep polling briefly rather than assuming timing.
        QTimer::singleShot(50, qApp, &autoAcceptHostKeyPrompt);
    });
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    bool allPass = true;
    auto check = [&](const char *label, bool condition) {
        fprintf(stderr, "[%s] %s\n", condition ? "PASS" : "FAIL", label);
        if (!condition) allPass = false;
    };

    const QString scratch = qEnvironmentVariable(
        "SFTP_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/sftp");

    SftpCredentials creds;
    creds.host = "127.0.0.1";
    creds.port = 2222;
    creds.username = qEnvironmentVariable("USER", "test");
    creds.authMethod = SftpAuthMethod::PublicKey;
    creds.privateKeyPath = scratch + "/client_key";

    auto *hostKeyVerifier = new HostKeyVerifier(&app);
    auto *backend = new SftpBackend(creds, hostKeyVerifier);
    auto *thread = new QThread(&app);
    backend->moveToThread(thread);

    bool connected = false;
    QList<RemoteEntry> listedEntries;
    bool listed = false;
    bool uploadOk = false;
    bool downloadOk = false;
    QString failureReason;

    const QString localDownloadPath = "/tmp/zephyrftp_verify_sftp_download.txt";
    const QString localUploadSourcePath = "/tmp/zephyrftp_verify_sftp_upload_source.txt";
    QFile::remove(localDownloadPath);
    QFile(localUploadSourcePath).open(QIODevice::WriteOnly);
    {
        QFile f(localUploadSourcePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("uploaded via SftpBackend PublicKey auth verification harness\n");
    }

    QObject::connect(backend, &RemoteBackend::connectionFailed, &app, [&](const QString &reason) {
        failureReason = reason;
        qApp->quit();
    });

    QObject::connect(backend, &RemoteBackend::connected, &app, [&]() {
        connected = true;
        QMetaObject::invokeMethod(backend, "listDirectory", Qt::QueuedConnection,
                                   Q_ARG(QString, "."));
    });

    QObject::connect(backend, &RemoteBackend::directoryListed, &app,
        [&](const QString &, const QList<RemoteEntry> &entries) {
            listed = true;
            listedEntries = entries;
            QMetaObject::invokeMethod(backend, "downloadFile", Qt::QueuedConnection,
                                       Q_ARG(QString, "sample.txt"),
                                       Q_ARG(QString, localDownloadPath),
                                       Q_ARG(qint64, 0));
        });

    QObject::connect(backend, &RemoteBackend::transferFinished, &app,
        [&](const QString &fileName) {
            if (fileName == "sample.txt" && !downloadOk) {
                downloadOk = true;
                QMetaObject::invokeMethod(backend, "uploadFile", Qt::QueuedConnection,
                                           Q_ARG(QString, localUploadSourcePath),
                                           Q_ARG(QString, "uploads/verify_roundtrip.txt"),
                                           Q_ARG(qint64, 0));
            } else if (fileName == localUploadSourcePath) {
                uploadOk = true;
                qApp->quit();
            }
        });

    QObject::connect(backend, &RemoteBackend::transferFailed, &app,
        [&](const QString &fileName, const QString &reason) {
            failureReason = QStringLiteral("transfer of %1 failed: %2").arg(fileName, reason);
            qApp->quit();
        });

    thread->start();
    autoAcceptHostKeyPrompt();
    QMetaObject::invokeMethod(backend, "connectToHost", Qt::QueuedConnection);

    QTimer::singleShot(15000, &app, [&]() {
        failureReason = QStringLiteral("timed out after 15s");
        qApp->quit();
    });

    app.exec();

    check("connected to real local SFTP server via PublicKey auth", connected);
    check("listDirectory returned real entries", listed && !listedEntries.isEmpty());
    check("downloadFile completed", downloadOk);
    check("uploadFile completed", uploadOk);
    if (!failureReason.isEmpty())
        fprintf(stderr, "failure reason: %s\n", qPrintable(failureReason));

    // Confirms the downloaded content is genuinely correct, not just
    // that a transferFinished signal fired.
    QFile downloaded(localDownloadPath);
    downloaded.open(QIODevice::ReadOnly);
    const QString downloadedContent = QString::fromUtf8(downloaded.readAll());
    check("downloaded content matches the real server file",
          downloadedContent.trimmed() == "hello from the local sftp test server");

    // Confirms the upload genuinely landed server-side with correct
    // content, not just that the client thought it finished — reads the
    // file back through the same SFTP session's own listing/download
    // machinery would be circular, so this reads the server's actual
    // scratch directory directly from disk (this harness and the server
    // are on the same machine).
    QFile roundtrip(scratch + "/root/uploads/verify_roundtrip.txt");
    roundtrip.open(QIODevice::ReadOnly);
    const QString roundtripContent = QString::fromUtf8(roundtrip.readAll());
    check("uploaded file genuinely landed server-side with correct content",
          roundtripContent == "uploaded via SftpBackend PublicKey auth verification harness\n");

    backend->deleteLater();
    thread->quit();
    thread->wait();

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
