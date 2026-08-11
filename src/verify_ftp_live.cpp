// Exercises FtpBackend against REAL servers — not a mock, not just "the
// code compiles" — closing five gaps flagged in ARCHITECTURE.md's
// Known gaps. NOT one of the ten self-contained EXCLUDE_FROM_ALL test
// targets (it has an external precondition those deliberately don't),
// so it isn't part of that suite or CI.
//
// Five phases: (1) a full connect/list/download/upload round trip
// against the plain FTP server — proves the control connection, PASV
// data connections, listing, and real transfers all genuinely work
// against a live server, not just against FtpBackend's own parser unit
// tests. (2) a connection attempt against the FTPS server's self-signed
// cert with no certificate verifier wired up — proves the AUTH TLS
// handshake itself completes and that FtpBackend fails closed with
// nobody to ask, the same "fails safe" contract
// SftpBackend::askUserToTrustHostKey() already has (see
// verify-ftps-trust for the real trust-prompt flow with a verifier
// actually wired up). (3) the same round trip as (1) but against a
// server with MLSD disabled, forcing FtpBackend's real LIST-format
// fallback to trigger. (4) the same round trip again against a server
// with PASV/EPSV disabled, forcing FtpBackend's real active/PORT
// fallback. (5) the same round trip over FTPS, but with the server's
// CA pre-trusted by this harness (an SSL_CERT_FILE env var override,
// not a FtpBackend change — see runCaTrustedFtpsPhase()) — proves a
// full ENCRYPTED transfer completes when the certificate genuinely
// validates, the one case (1)/(3)/(4) don't cover and (2) deliberately
// doesn't reach.
//
// FtpBackend uses QTcpSocket/QSslSocket, both async/event-driven — no
// worker thread needed here the way SftpBackend's blocking libssh2 API
// requires one; running it directly on this harness's own thread still
// exercises the exact same code the app runs on its own worker thread.
//
// Run with:
//   tools/local-test-servers/start-ftp.sh
//   tools/local-test-servers/start-ftps.sh
//   tools/local-test-servers/start-ftp-legacy-list.sh
//   tools/local-test-servers/start-ftp-active-only.sh
//   tools/local-test-servers/start-ftps-trusted.sh
//   cmake --build build --target verify-ftp-live
//   QT_QPA_PLATFORM=offscreen ./build/verify-ftp-live
#include <QCoreApplication>
#include <QTimer>
#include <QFile>
#include <QSslConfiguration>
#include <QSslCertificate>
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

// Shared by every plain (non-CA-trust-testing) phase below: connect,
// list, download sample.txt, upload a roundtrip file, confirm content
// both client-side and by reading the server's own disk directly. tag
// prefixes every check label (distinguishes phases in the output);
// expectedSampleContent is what THIS phase's server's sample.txt
// actually contains (each start-*.sh script writes a distinct string so
// a mismatch can't silently point at the wrong server).
bool runRoundTripPhase(const char *tag, quint16 port, const QString &scratch,
                       const QString &expectedSampleContent, bool useFtps = false)
{
    const QString localDownloadPath = QStringLiteral("/tmp/zephyrftp_verify_%1_download.txt").arg(tag);
    const QString localUploadSourcePath = QStringLiteral("/tmp/zephyrftp_verify_%1_upload_source.txt").arg(tag);
    QFile::remove(localDownloadPath);
    {
        QFile f(localUploadSourcePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("uploaded via FtpBackend live verification harness\n");
    }

    FtpCredentials creds;
    creds.host = "127.0.0.1";
    creds.port = port;
    creds.username = useFtps ? "ftpsuser" : "ftpuser";
    creds.password = useFtps ? "ftpspass" : "ftppass";
    creds.ftpsMode = useFtps ? FtpsMode::Explicit : FtpsMode::None;

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

    check(qPrintable(QStringLiteral("[%1] connected to real local server").arg(tag)), connected);
    check(qPrintable(QStringLiteral("[%1] listDirectory returned real entries").arg(tag)),
          listed && !listedEntries.isEmpty());
    check(qPrintable(QStringLiteral("[%1] downloadFile completed").arg(tag)), downloadOk);
    check(qPrintable(QStringLiteral("[%1] uploadFile completed").arg(tag)), uploadOk);
    if (!failureReason.isEmpty())
        fprintf(stderr, "[%s] failure reason: %s\n", tag, qPrintable(failureReason));

    QFile downloaded(localDownloadPath);
    downloaded.open(QIODevice::ReadOnly);
    check(qPrintable(QStringLiteral("[%1] downloaded content matches the real server file").arg(tag)),
          QString::fromUtf8(downloaded.readAll()).trimmed() == expectedSampleContent);

    QFile roundtrip(scratch + "/root/uploads/verify_roundtrip.txt");
    roundtrip.open(QIODevice::ReadOnly);
    check(qPrintable(QStringLiteral("[%1] uploaded file genuinely landed server-side with correct content").arg(tag)),
          QString::fromUtf8(roundtrip.readAll()) == "uploaded via FtpBackend live verification harness\n");

    return connected && listed && downloadOk && uploadOk;
}

// --- Phase 1: plain FTP, full round trip ---
bool runPlainFtpPhase()
{
    const QString scratch = qEnvironmentVariable(
        "FTP_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/ftp");
    return runRoundTripPhase("FTP", 2121, scratch, "hello from the local ftp test server");
}

// --- Phase 3: legacy LIST fallback, forced by a real server with MLSD
// disabled (start-ftp-legacy-list.sh) — closes the gap flagged in
// ARCHITECTURE.md: only the fallback TRIGGER logic and parseListLine()
// had been exercised in isolation against crafted sample lines, never
// against a real server that genuinely can't do MLSD. Still pyftpdlib
// underneath (not a different vendor's LIST dialect), but this is the
// actual previously-untested code path: a genuine 502 triggering the
// real fallback, and the Unix-`ls -l` branch of parseListLine() parsing
// real wire data from a real server. ---
bool runLegacyListPhase()
{
    const QString scratch = qEnvironmentVariable(
        "FTP_LEGACY_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/ftp-legacy-list");
    return runRoundTripPhase("FTP-legacy-LIST", 2123, scratch,
                              "hello from the local legacy-list ftp test server");
}

// --- Phase 4: active/PORT fallback, forced by a real server with
// PASV/EPSV disabled (start-ftp-active-only.sh) — closes the gap
// flagged in ARCHITECTURE.md: "no active/PORT fallback ... a server
// that requires active mode won't work at all." A genuine PASV
// rejection triggers FtpBackend's real active-mode fallback, and the
// full round trip (list/download/upload) completes over a connection
// the SERVER dialed back to US. ---
bool runActiveModePhase()
{
    const QString scratch = qEnvironmentVariable(
        "FTP_ACTIVE_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/ftp-active-only");
    return runRoundTripPhase("FTP-active-mode", 2125, scratch,
                              "hello from the local active-only ftp test server");
}

// --- Phase 5: a full encrypted transfer over FTPS with a certificate
// that GENUINELY validates (start-ftps-trusted.sh: a leaf cert signed
// by a throwaway local CA) — closes the gap flagged in ARCHITECTURE.md:
// "no full authenticated FTPS transfer has been verified — FtpBackend
// has no override to trust a self-signed cert, by design, so this needs
// a CA-trusted cert to test." Trusting the CA here is pure test-harness
// code (the SSL_CERT_FILE env var OpenSSL's own default-path loading
// respects — see FtpTlsSocket::handshake()'s
// SSL_CTX_set_default_verify_paths() call), not a change to FtpBackend
// itself — production code never gets told about this extra CA. Because
// the cert now genuinely validates, FtpBackend's certificate-TOFU logic
// never even prompts — this proves the plain success path (the one
// every REAL deployment with a proper certificate would take) completes
// a full round trip end to end. ---
bool runCaTrustedFtpsPhase()
{
    const QString scratch = qEnvironmentVariable(
        "FTPS_TRUSTED_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/ftps-trusted");

    const QString caCertPath = scratch + "/ca-cert.pem";
    const QList<QSslCertificate> caCerts = QSslCertificate::fromPath(caCertPath);
    if (caCerts.isEmpty()) {
        check("[FTPS-trusted] loaded the throwaway CA certificate "
              "(start-ftps-trusted.sh must be running)", false);
        return false;
    }
    qputenv("SSL_CERT_FILE", caCertPath.toUtf8());

    return runRoundTripPhase("FTPS-trusted", 2124, scratch,
                              "hello from the local ca-trusted ftps test server", /*useFtps=*/true);
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
    const bool legacyListOk = runLegacyListPhase();
    const bool activeModeOk = runActiveModePhase();
    const bool caTrustedFtpsOk = runCaTrustedFtpsPhase();

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return (allPass && ftpOk && ftpsOk && legacyListOk && activeModeOk && caTrustedFtpsOk) ? 0 : 1;
}
