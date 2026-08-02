// Exercises SftpBackend against Dropbear — a genuinely different SSH
// daemon from OpenSSH (the one verify_sftp_pubkey.cpp/
// start-sftp-pubkey.sh already test), in its own throwaway podman
// container (tools/local-test-servers/start-dropbear.sh). Password auth
// here (vs. that harness's pubkey-only setup) adds auth-method
// diversity too.
//
// Real, checked-for-real limitation, stated plainly rather than glossed
// over: Dropbear's Fedora package ships no sftp-server of its own
// (confirmed: `rpm -ql dropbear` has none) — it shells out to an
// external one, configured to be OpenSSH's own
// (/usr/libexec/openssh/sftp-server). So this proves real SSH
// transport/session/auth-layer diversity, NOT SFTP-wire-protocol
// implementation diversity — the actual SFTP subsystem traffic here is
// most likely OpenSSH's own code regardless of which daemon fronts it.
// See ARCHITECTURE.md's Known Gaps entry for the full accounting.
//
// Same auto-accept-the-real-modal-dialog technique verify_sftp_pubkey.cpp
// already uses for its own host-key TOFU prompt — fully automated, not
// a manual click-through. NOT one of the ten self-contained
// EXCLUDE_FROM_ALL test targets (needs podman + this container already
// running).
//
// Run with:
//   tools/local-test-servers/start-dropbear.sh
//   cmake --build build --target verify-sftp-vendors
//   QT_QPA_PLATFORM=offscreen ./build/verify-sftp-vendors
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QFile>
#include <QMessageBox>
#include <QAbstractButton>
#include <cstdio>
#include "ui/HostKeyVerifier.h"
#include "backends/SftpBackend.h"
#include "backends/SftpCredentials.h"

namespace {
// Identical technique to verify_sftp_pubkey.cpp's autoAcceptHostKeyPrompt()
// — see that file's comment for the full reasoning (a real, synchronous,
// blocking-queued QMessageBox from the worker thread, driven via a
// QTimer polling this thread's own event loop).
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
        QTimer::singleShot(50, qApp, &autoAcceptHostKeyPrompt);
    });
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    bool allPass = true;
    auto check = [&](const char *label, bool condition) {
        fprintf(stderr, "[%s] %s\n", condition ? "PASS" : "FAIL", label);
        if (!condition) allPass = false;
    };

    SftpCredentials creds;
    creds.host = "127.0.0.1";
    creds.port = 2223;
    creds.username = "dropbearuser";
    creds.authMethod = SftpAuthMethod::Password;
    creds.password = "dropbearpass";

    auto *hostKeyVerifier = new HostKeyVerifier(&app);
    auto *backend = new SftpBackend(creds, hostKeyVerifier);
    auto *thread = new QThread(&app);
    backend->moveToThread(thread);

    bool connected = false, listed = false, downloadOk = false, uploadOk = false, roundtripDownloadOk = false;
    QList<RemoteEntry> listedEntries;
    QString failureReason;

    const QString localDownloadPath = "/tmp/zephyrftp_verify_dropbear_download.txt";
    const QString localUploadSourcePath = "/tmp/zephyrftp_verify_dropbear_upload_source.txt";
    const QString localRoundtripBackPath = "/tmp/zephyrftp_verify_dropbear_roundtrip_back.txt";
    QFile::remove(localDownloadPath);
    QFile::remove(localRoundtripBackPath);
    {
        QFile f(localUploadSourcePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("uploaded via SftpBackend Dropbear verification harness\n");
    }

    QObject::connect(backend, &RemoteBackend::connectionFailed, &app, [&](const QString &reason) {
        failureReason = reason;
        qApp->quit();
    });

    QObject::connect(backend, &RemoteBackend::connected, &app, [&]() {
        connected = true;
        QMetaObject::invokeMethod(backend, "listDirectory", Qt::QueuedConnection, Q_ARG(QString, "."));
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
            } else if (fileName == localUploadSourcePath && !uploadOk) {
                uploadOk = true;
                // Self-contained container, no host-mounted scratch dir
                // (same reasoning as verify_ftp_vendors.cpp) — verify
                // the upload genuinely landed by downloading it back
                // through the same session.
                QMetaObject::invokeMethod(backend, "downloadFile", Qt::QueuedConnection,
                                           Q_ARG(QString, "uploads/verify_roundtrip.txt"),
                                           Q_ARG(QString, localRoundtripBackPath),
                                           Q_ARG(qint64, 0));
            } else if (fileName == "uploads/verify_roundtrip.txt") {
                roundtripDownloadOk = true;
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

    check("connected to the real Dropbear server via password auth", connected);
    check("listDirectory returned real entries", listed && !listedEntries.isEmpty());
    check("downloadFile completed", downloadOk);
    check("uploadFile completed", uploadOk);
    check("downloaded the upload back for real (round trip through the server, "
          "not the container's own filesystem)", roundtripDownloadOk);
    if (!failureReason.isEmpty())
        fprintf(stderr, "failure reason: %s\n", qPrintable(failureReason));

    QFile downloaded(localDownloadPath);
    downloaded.open(QIODevice::ReadOnly);
    check("downloaded content matches the real server file",
          QString::fromUtf8(downloaded.readAll()).trimmed()
              == "hello from the local dropbear test server");

    QFile roundtripBack(localRoundtripBackPath);
    roundtripBack.open(QIODevice::ReadOnly);
    check("uploaded content matches on download-back",
          QString::fromUtf8(roundtripBack.readAll())
              == "uploaded via SftpBackend Dropbear verification harness\n");

    backend->deleteLater();
    thread->quit();
    thread->wait();

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
