// Exercises FtpBackend against REAL servers — not a mock, not just "the
// code compiles" — closing the gap flagged in ARCHITECTURE.md: "the
// control connection, PASV handling, the AUTH TLS upgrade, and actual
// transfers have never been exercised against a live FTP or FTPS
// server." Needs tools/local-test-servers/start-ftp.sh and
// start-ftps.sh already running; this is NOT one of the ten
// self-contained EXCLUDE_FROM_ALL test targets (it has an external
// precondition those deliberately don't), so it isn't part of that
// suite or CI.
//
// Two phases: (1) a full connect/list/download/upload round trip
// against the plain FTP server — proves the control connection, PASV
// data connections, listing, and real transfers all genuinely work
// against a live server, not just against FtpBackend's own parser unit
// tests. (2) a connection attempt against the FTPS server's self-signed
// cert — proves the AUTH TLS handshake itself completes (the control
// connection successfully upgrades to TLS) and that FtpBackend then
// correctly rejects the untrusted certificate rather than silently
// accepting it, which is the deliberate, documented behavior (see
// ARCHITECTURE.md's FtpBackend entry) — not a bug being routed around.
// A full authenticated FTPS transfer isn't reachable this way on
// purpose: FtpBackend has no override to trust a self-signed cert, by
// design.
//
// FtpBackend uses QTcpSocket/QSslSocket, both async/event-driven — no
// worker thread needed here the way SftpBackend's blocking libssh2 API
// requires one; running it directly on this harness's own thread still
// exercises the exact same code the app runs on its own worker thread.
//
// Run with:
//   tools/local-test-servers/start-ftp.sh
//   tools/local-test-servers/start-ftps.sh
//   cmake --build build --target verify-ftp-live
//   QT_QPA_PLATFORM=offscreen FTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/ftp \
//       ./build/verify-ftp-live
#include <QCoreApplication>
#include <QTimer>
#include <QFile>
#include <cstdio>
#include "backends/FtpBackend.h"
#include "backends/FtpCredentials.h"

namespace {
bool allPass = true;

void check(const char *label, bool condition)
{
    fprintf(stderr, "[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) allPass = false;
}

// --- Phase 1: plain FTP, full round trip ---
bool runPlainFtpPhase()
{
    const QString scratch = qEnvironmentVariable(
        "FTP_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/ftp");
    const QString localDownloadPath = "/tmp/zephyrftp_verify_ftp_download.txt";
    const QString localUploadSourcePath = "/tmp/zephyrftp_verify_ftp_upload_source.txt";
    QFile::remove(localDownloadPath);
    {
        QFile f(localUploadSourcePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("uploaded via FtpBackend live verification harness\n");
    }

    FtpCredentials creds;
    creds.host = "127.0.0.1";
    creds.port = 2121;
    creds.username = "ftpuser";
    creds.password = "ftppass";
    creds.ftpsMode = FtpsMode::None;

    FtpBackend backend(creds);

    QEventLoop loop;
    bool connected = false, listed = false, downloadOk = false, uploadOk = false;
    QList<RemoteEntry> listedEntries;
    QString failureReason;

    QObject::connect(&backend, &RemoteBackend::connectionFailed, &loop,
        [&](const QString &reason) { failureReason = reason; loop.quit(); });

    QObject::connect(&backend, &RemoteBackend::connected, &loop, [&]() {
        connected = true;
        backend.listDirectory(".");
    });

    QObject::connect(&backend, &RemoteBackend::directoryListed, &loop,
        [&](const QString &, const QList<RemoteEntry> &entries) {
            listed = true;
            listedEntries = entries;
            backend.downloadFile("sample.txt", localDownloadPath, 0);
        });

    QObject::connect(&backend, &RemoteBackend::transferFinished, &loop,
        [&](const QString &fileName) {
            if (fileName == "sample.txt" && !downloadOk) {
                downloadOk = true;
                backend.uploadFile(localUploadSourcePath, "uploads/verify_roundtrip.txt", 0);
            } else if (fileName == localUploadSourcePath) {
                uploadOk = true;
                loop.quit();
            }
        });

    QObject::connect(&backend, &RemoteBackend::transferFailed, &loop,
        [&](const QString &fileName, const QString &reason) {
            failureReason = QStringLiteral("transfer of %1 failed: %2").arg(fileName, reason);
            loop.quit();
        });

    QTimer::singleShot(15000, &loop, [&]() {
        failureReason = QStringLiteral("timed out after 15s");
        loop.quit();
    });

    QTimer::singleShot(0, &loop, [&]() { backend.connectToHost(); });
    loop.exec();

    check("[FTP] connected to real local FTP server", connected);
    check("[FTP] listDirectory returned real entries", listed && !listedEntries.isEmpty());
    check("[FTP] downloadFile completed", downloadOk);
    check("[FTP] uploadFile completed", uploadOk);
    if (!failureReason.isEmpty())
        fprintf(stderr, "[FTP] failure reason: %s\n", qPrintable(failureReason));

    QFile downloaded(localDownloadPath);
    downloaded.open(QIODevice::ReadOnly);
    check("[FTP] downloaded content matches the real server file",
          QString::fromUtf8(downloaded.readAll()).trimmed()
              == "hello from the local ftp test server");

    QFile roundtrip(scratch + "/root/uploads/verify_roundtrip.txt");
    roundtrip.open(QIODevice::ReadOnly);
    check("[FTP] uploaded file genuinely landed server-side with correct content",
          QString::fromUtf8(roundtrip.readAll())
              == "uploaded via FtpBackend live verification harness\n");

    return connected && listed && downloadOk && uploadOk;
}

// --- Phase 2: FTPS, AUTH TLS handshake against a real (self-signed) cert ---
bool runFtpsCertRejectionPhase()
{
    FtpCredentials creds;
    creds.host = "127.0.0.1";
    creds.port = 2122;
    creds.username = "ftpsuser";
    creds.password = "ftpspass";
    creds.ftpsMode = FtpsMode::Explicit;

    FtpBackend backend(creds);

    QEventLoop loop;
    QString failureReason;
    bool sawConnected = false;

    QObject::connect(&backend, &RemoteBackend::connectionFailed, &loop,
        [&](const QString &reason) { failureReason = reason; loop.quit(); });
    QObject::connect(&backend, &RemoteBackend::connected, &loop,
        [&]() { sawConnected = true; loop.quit(); });

    QTimer::singleShot(15000, &loop, [&]() {
        failureReason = QStringLiteral("timed out after 15s — AUTH TLS handshake never completed");
        loop.quit();
    });

    QTimer::singleShot(0, &loop, [&]() { backend.connectToHost(); });
    loop.exec();

    check("[FTPS] AUTH TLS handshake reached the certificate-verification stage "
          "(connectionFailed fired with a real reason, not a timeout)",
          !failureReason.isEmpty() && !failureReason.contains("timed out"));
    check("[FTPS] self-signed certificate correctly rejected as untrusted "
          "(FtpBackend's designed fail-closed behavior, confirmed for real)",
          failureReason.contains("certificate", Qt::CaseInsensitive));
    check("[FTPS] did NOT silently accept the untrusted certificate", !sawConnected);
    fprintf(stderr, "[FTPS] actual reason reported: %s\n", qPrintable(failureReason));

    return !failureReason.isEmpty() && failureReason.contains("certificate", Qt::CaseInsensitive)
        && !sawConnected;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const bool ftpOk = runPlainFtpPhase();
    const bool ftpsOk = runFtpsCertRejectionPhase();

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return (allPass && ftpOk && ftpsOk) ? 0 : 1;
}
