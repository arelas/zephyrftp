// Measures real SFTP throughput (SftpBackend, this project's own libssh2
// code) against a real, non-loopback remote server, plus an independent
// scp/ssh baseline on the exact same link — closing the one open question
// left in ARCHITECTURE.md's throughput Known Gaps entry: after the
// pipelined-I/O and buffer-tuning fixes brought SFTP from ~4MB/s to
// ~22-25MB/s, a ~1.6-1.8x gap to the ~40MB/s FileZilla/Termius/SMB
// comparison remained, and whether that's an inherent libssh2/
// architectural ceiling or a further fixable bottleneck was unknown —
// every test server available here was loopback (~0ms RTT), which is
// exactly the variable that gap is about.
//
// Deliberately NOT one of the ten self-contained EXCLUDE_FROM_ALL test
// targets, and NOT modeled on a throwaway podman container like this
// project's other verify-* harnesses: this one needs a real, externally
// reachable server this project doesn't and can't provide for itself, so
// it reads connection details from the environment rather than hardcoding
// a local container's fixed host/port/credentials, and fails fast with a
// clear message if they're not set rather than silently defaulting to
// something meaningless like "127.0.0.1".
//
// Same QApplication + SftpBackend-on-its-own-QThread + host-key-TOFU
// auto-accept structure as verify_sftp_vendors.cpp (the only existing
// harness already using password auth against a non-container-internal-
// style connection) — see that file's autoAcceptHostKeyPrompt() for the
// technique this reuses verbatim.
//
// Run with (all required except port/size, which default as shown):
//   ZEPHYR_THROUGHPUT_HOST=<host> ZEPHYR_THROUGHPUT_USER=<user> \
//   ZEPHYR_THROUGHPUT_PASSWORD=<password> [ZEPHYR_THROUGHPUT_PORT=22] \
//   [ZEPHYR_THROUGHPUT_FILE_SIZE_MB=100] \
//   cmake --build build --target verify-sftp-throughput && \
//   QT_QPA_PLATFORM=offscreen ./build/verify-sftp-throughput
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QCryptographicHash>
#include <QMessageBox>
#include <QAbstractButton>
#include <cstdio>
#include "ui/HostKeyVerifier.h"
#include "backends/SftpBackend.h"
#include "backends/SftpCredentials.h"

namespace {

// Identical technique to verify_sftp_vendors.cpp's autoAcceptHostKeyPrompt().
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

double megabytesPerSecond(qint64 bytes, qint64 elapsedMs)
{
    if (elapsedMs <= 0) return 0.0;
    return (static_cast<double>(bytes) / (1024.0 * 1024.0)) / (static_cast<double>(elapsedMs) / 1000.0);
}

// Fills path with sizeMb megabytes of pseudo-random content, written in
// 1MB chunks rather than one giant in-memory buffer.
bool generateTestFile(const QString &path, qint64 sizeMb)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QByteArray chunk(1024 * 1024, Qt::Uninitialized);
    quint32 *words = reinterpret_cast<quint32 *>(chunk.data());
    for (int i = 0; i < chunk.size() / 4; ++i)
        words[i] = QRandomGenerator::global()->generate();
    for (qint64 i = 0; i < sizeMb; ++i) {
        if (f.write(chunk) != chunk.size()) return false;
    }
    return true;
}

QByteArray hashFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&f);
    return hash.result();
}

// Independent scp baseline against the same host/file, using OpenSSH's
// own client rather than this project's code at all. Non-interactive
// password auth via SSH_ASKPASS_REQUIRE=force (OpenSSH 8.4+, no extra
// package needed) rather than sshpass, which isn't installed here.
// Best-effort: a failure here is reported but doesn't fail the harness —
// this is a comparison baseline, not correctness-critical.
struct ScpBaselineResult {
    bool ok = false;
    double uploadMBps = 0.0;
    double downloadMBps = 0.0;
    QString diagnostic;
};

ScpBaselineResult runScpBaseline(const QString &host, int port, const QString &user,
                                  const QString &password, const QString &localUploadPath,
                                  const QString &localScpDownloadPath, qint64 fileSizeBytes)
{
    ScpBaselineResult result;

    const QString askpassPath = QDir::tempPath() + "/zephyrftp_throughput_askpass.sh";
    {
        QFile askpass(askpassPath);
        if (!askpass.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            result.diagnostic = "could not write askpass helper script";
            return result;
        }
        askpass.write("#!/bin/sh\necho \"$ZEPHYR_THROUGHPUT_PASSWORD\"\n");
        askpass.close();
        askpass.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("SSH_ASKPASS", askpassPath);
    env.insert("SSH_ASKPASS_REQUIRE", "force");
    env.insert("ZEPHYR_THROUGHPUT_PASSWORD", password);

    const QString remoteScpPath = "zephyrftp_throughput_scp_test.bin";
    const QStringList sshOpts = {"-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null"};

    auto runProcess = [&](const QString &program, const QStringList &args) -> bool {
        QProcess proc;
        proc.setProcessEnvironment(env);
        proc.start(program, args);
        if (!proc.waitForStarted(5000)) {
            result.diagnostic = QString("%1 failed to start: %2").arg(program, proc.errorString());
            return false;
        }
        if (!proc.waitForFinished(120000)) {
            result.diagnostic = QString("%1 timed out").arg(program);
            proc.kill();
            return false;
        }
        if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
            result.diagnostic = QString("%1 exited %2: %3").arg(program)
                .arg(proc.exitCode())
                .arg(QString::fromUtf8(proc.readAllStandardError()));
            return false;
        }
        return true;
    };

    QElapsedTimer timer;

    timer.start();
    if (!runProcess("scp", QStringList() << sshOpts << "-P" << QString::number(port)
                     << localUploadPath << QString("%1@%2:%3").arg(user, host, remoteScpPath)))
        return result;
    result.uploadMBps = megabytesPerSecond(fileSizeBytes, timer.elapsed());

    timer.restart();
    if (!runProcess("scp", QStringList() << sshOpts << "-P" << QString::number(port)
                     << QString("%1@%2:%3").arg(user, host, remoteScpPath) << localScpDownloadPath))
        return result;
    result.downloadMBps = megabytesPerSecond(fileSizeBytes, timer.elapsed());

    // Best-effort cleanup of the remote scp test file; failure here
    // doesn't affect the measured result.
    runProcess("ssh", QStringList() << sshOpts << "-p" << QString::number(port)
               << QString("%1@%2").arg(user, host) << QString("rm -f %1").arg(remoteScpPath));

    result.ok = true;
    return result;
}

}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    const QString host = qEnvironmentVariable("ZEPHYR_THROUGHPUT_HOST");
    const QString user = qEnvironmentVariable("ZEPHYR_THROUGHPUT_USER");
    const QString password = qEnvironmentVariable("ZEPHYR_THROUGHPUT_PASSWORD");
    const int port = qEnvironmentVariableIntValue("ZEPHYR_THROUGHPUT_PORT") > 0
                          ? qEnvironmentVariableIntValue("ZEPHYR_THROUGHPUT_PORT") : 22;
    const int fileSizeMb = qEnvironmentVariableIntValue("ZEPHYR_THROUGHPUT_FILE_SIZE_MB") > 0
                                ? qEnvironmentVariableIntValue("ZEPHYR_THROUGHPUT_FILE_SIZE_MB") : 100;

    if (host.isEmpty() || user.isEmpty() || password.isEmpty()) {
        fprintf(stderr,
            "verify-sftp-throughput needs a real, externally-reachable SFTP server — "
            "set ZEPHYR_THROUGHPUT_HOST, ZEPHYR_THROUGHPUT_USER, and "
            "ZEPHYR_THROUGHPUT_PASSWORD (optionally ZEPHYR_THROUGHPUT_PORT, default 22, "
            "and ZEPHYR_THROUGHPUT_FILE_SIZE_MB, default 100). Not run: no default server "
            "would be meaningful here (unlike this project's other verify-* harnesses, "
            "which spin up their own throwaway local containers).\n");
        return 2;
    }

    const QString localUploadPath = QDir::tempPath() + "/zephyrftp_throughput_upload.bin";
    const QString localDownloadPath = QDir::tempPath() + "/zephyrftp_throughput_download.bin";
    const QString localScpDownloadPath = QDir::tempPath() + "/zephyrftp_throughput_scp_download.bin";
    const qint64 fileSizeBytes = static_cast<qint64>(fileSizeMb) * 1024 * 1024;

    fprintf(stderr, "Generating %d MB test file...\n", fileSizeMb);
    if (!generateTestFile(localUploadPath, fileSizeMb)) {
        fprintf(stderr, "FAIL: could not generate local test file\n");
        return 1;
    }
    const QByteArray uploadHash = hashFile(localUploadPath);

    fprintf(stderr, "Running scp/ssh baseline against %s:%d...\n", qPrintable(host), port);
    const ScpBaselineResult scpResult = runScpBaseline(host, port, user, password,
        localUploadPath, localScpDownloadPath, fileSizeBytes);
    if (scpResult.ok) {
        fprintf(stderr, "scp baseline: upload %.2f MB/s, download %.2f MB/s\n",
                scpResult.uploadMBps, scpResult.downloadMBps);
    } else {
        fprintf(stderr, "scp baseline SKIPPED (not fatal): %s\n", qPrintable(scpResult.diagnostic));
    }

    bool allPass = true;
    auto check = [&](const char *label, bool condition) {
        fprintf(stderr, "[%s] %s\n", condition ? "PASS" : "FAIL", label);
        if (!condition) allPass = false;
    };

    SftpCredentials creds;
    creds.host = host;
    creds.port = port;
    creds.username = user;
    creds.authMethod = SftpAuthMethod::Password;
    creds.password = password;

    auto *hostKeyVerifier = new HostKeyVerifier(&app);
    auto *backend = new SftpBackend(creds, hostKeyVerifier);
    auto *thread = new QThread(&app);
    backend->moveToThread(thread);

    bool connected = false, uploadOk = false, downloadOk = false;
    QString failureReason;
    QElapsedTimer transferTimer;
    qint64 uploadElapsedMs = 0, downloadElapsedMs = 0;
    const QString remoteTestPath = "zephyrftp_throughput_test.bin";

    QObject::connect(backend, &RemoteBackend::connectionFailed, &app, [&](const QString &reason) {
        failureReason = reason;
        qApp->quit();
    });

    QObject::connect(backend, &RemoteBackend::connected, &app, [&]() {
        connected = true;
        transferTimer.start();
        QMetaObject::invokeMethod(backend, "uploadFile", Qt::QueuedConnection,
                                   Q_ARG(QString, localUploadPath),
                                   Q_ARG(QString, remoteTestPath),
                                   Q_ARG(qint64, 0));
    });

    QObject::connect(backend, &RemoteBackend::transferFinished, &app,
        [&](const QString &fileName) {
            if (fileName == localUploadPath && !uploadOk) {
                uploadOk = true;
                uploadElapsedMs = transferTimer.elapsed();
                transferTimer.restart();
                QMetaObject::invokeMethod(backend, "downloadFile", Qt::QueuedConnection,
                                           Q_ARG(QString, remoteTestPath),
                                           Q_ARG(QString, localDownloadPath),
                                           Q_ARG(qint64, 0));
            } else if (fileName == remoteTestPath) {
                downloadOk = true;
                downloadElapsedMs = transferTimer.elapsed();
                QMetaObject::invokeMethod(backend, "deleteEntry", Qt::QueuedConnection,
                                           Q_ARG(QString, remoteTestPath),
                                           Q_ARG(bool, false));
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

    QTimer::singleShot(300000, &app, [&]() {
        failureReason = QStringLiteral("timed out after 300s");
        qApp->quit();
    });

    app.exec();

    check("connected to the real server via password auth", connected);
    check("uploadFile completed", uploadOk);
    check("downloadFile completed", downloadOk);
    if (!failureReason.isEmpty())
        fprintf(stderr, "failure reason: %s\n", qPrintable(failureReason));

    const QByteArray downloadHash = hashFile(localDownloadPath);
    check("downloaded content matches the uploaded content byte-for-byte",
          !uploadHash.isEmpty() && uploadHash == downloadHash);

    if (uploadOk && downloadOk) {
        const double uploadMBps = megabytesPerSecond(fileSizeBytes, uploadElapsedMs);
        const double downloadMBps = megabytesPerSecond(fileSizeBytes, downloadElapsedMs);
        fprintf(stderr, "\nZephyrFTP SftpBackend: upload %.2f MB/s, download %.2f MB/s "
                "(%d MB file, %lld ms / %lld ms)\n",
                uploadMBps, downloadMBps, fileSizeMb,
                static_cast<long long>(uploadElapsedMs), static_cast<long long>(downloadElapsedMs));
        if (scpResult.ok) {
            fprintf(stderr, "scp baseline:          upload %.2f MB/s, download %.2f MB/s\n",
                    scpResult.uploadMBps, scpResult.downloadMBps);
            fprintf(stderr, "ratio (scp/ZephyrFTP): upload %.2fx, download %.2fx\n",
                    uploadMBps > 0 ? scpResult.uploadMBps / uploadMBps : 0.0,
                    downloadMBps > 0 ? scpResult.downloadMBps / downloadMBps : 0.0);
        }
    }

    backend->deleteLater();
    thread->quit();
    thread->wait();

    QFile::remove(localUploadPath);
    QFile::remove(localDownloadPath);
    QFile::remove(localScpDownloadPath);

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
