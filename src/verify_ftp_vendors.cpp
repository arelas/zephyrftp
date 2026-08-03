// Exercises FtpBackend against REAL, independently-implemented FTP
// server software — not pyftpdlib (a Python stand-in this project
// already controls), but actual vsftpd and proftpd, each in its own
// throwaway podman container (tools/local-test-servers/start-vsftpd.sh,
// start-proftpd.sh). Closes the gap ARCHITECTURE.md used to flag: the
// legacy-LIST fallback had only ever been exercised against pyftpdlib
// with MLSD manually disabled via a flag, never a genuinely different
// vendor's server. NOT one of the ten self-contained EXCLUDE_FROM_ALL
// test targets (needs podman + these containers already running) and
// kept SEPARATE from verify-ftp-live (which has no such extra
// precondition) — same reasoning verify-ftps-trust is its own target.
//
// Two phases, same round-trip shape as verify_ftp_live.cpp's phases:
// connect, list a real directory, download a real file and confirm its
// exact byte content, then upload a file and download it back to
// confirm it genuinely landed server-side with correct content. Unlike
// verify_ftp_live.cpp's pyftpdlib-backed servers, these containers are
// self-contained (test content baked into the image, no host-mounted
// scratch directory — see start-vsftpd.sh/start-proftpd.sh's own
// comments on why), so the round-trip upload is verified by downloading
// it back through the SAME protocol session rather than reading the
// container's filesystem directly from the host.
//
// Neither phase specifically asserts "MLSD was rejected, LIST was used
// instead" — FtpBackend has no signal exposing which one it picked.
// That specific fallback-trigger logic (a 500/502/550 reply engaging
// the fallback) is what ftp_parsing_test.cpp and verify_ftp_live.cpp's
// legacy-LIST phase already cover directly. What THIS harness proves is
// narrower and just as real: against actual vsftpd (never implements
// MLSD) and actual proftpd (MLSD explicitly denied here, see
// containers/proftpd.conf), the whole listing+transfer round trip still
// completes correctly end to end — which is only possible if the
// fallback genuinely worked.
//
// Two more phases attempt the same round trip over FTPS (explicit AUTH
// TLS) against both vendors, closing a second ARCHITECTURE.md gap:
// FTPS had only ever been tested against this project's own pyftpdlib
// stand-in (verify_ftp_live.cpp), never a genuinely different vendor's
// TLS implementation. The CA certificates each container's start script
// copies out (start-vsftpd.sh/start-proftpd.sh) are pre-trusted here via
// QSslConfiguration::setDefaultConfiguration() — the same pure-test-
// harness pattern verify_ftp_live.cpp's runCaTrustedFtpsPhase() already
// uses, appropriate here too since this is a plain QCoreApplication
// harness with no GUI event loop for a TOFU dialog to run in.
//
// vsftpd-ftps: a full real round trip now genuinely passes — connect,
// list, download, upload, content verified both ways, over real FTPS
// against a real vendor. Getting there found and fixed three real,
// independent client-side bugs in FtpBackend.cpp's handling of a
// server that closes a data connection uncleanly (no TLS close_notify)
// once it's actually done with it, in both directions — see
// ARCHITECTURE.md's Known Gaps for the full writeup, including why
// vsftpd.conf deliberately ships with require_ssl_reuse left OFF (a
// real, confirmed deadlock in vsftpd's own privilege-separated
// architecture under this container environment when it's on, not a
// FtpBackend bug — the reasoning and how to flip it back on for further
// investigation are documented directly in that file).
//
// proftpd-ftps: EXPECTED to fail, honestly, not silently — proftpd's
// own mod_tls is left at its default STRICT session-reuse enforcement
// (see containers/proftpd.conf's comment), and this project's own
// TLS-session-ticket-reuse implementation (FtpBackend.cpp's
// m_controlSessionTicket) genuinely does not satisfy it, confirmed via
// proftpd's own TLSLog ("client did not reuse TLS session, rejecting
// data connection") — a real, negative, informative answer to whether a
// genuinely strict server accepts this project's reuse attempt, not a
// bug to fix. See ARCHITECTURE.md's Known Gaps for the full story.
//
// Run with:
//   tools/local-test-servers/start-vsftpd.sh
//   tools/local-test-servers/start-proftpd.sh
//   cmake --build build --target verify-ftp-vendors
//   QT_QPA_PLATFORM=offscreen ./build/verify-ftp-vendors
#include <QCoreApplication>
#include <QTimer>
#include <QFile>
#include <QSslCertificate>
#include <QSslConfiguration>
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

// Loads a CA certificate a start-*.sh script copied out of its
// container (see verify_ftp_live.cpp's runCaTrustedFtpsPhase() for the
// identical pattern this mirrors) and adds it to the process-wide
// default QSslConfiguration, so a genuinely CA-signed leaf certificate
// validates for real — pure test-harness code, FtpBackend itself is
// never told about any extra CA. Returns false (and reports why) if the
// scratch file isn't there, most likely because the matching start
// script hasn't been run (or predates this CA-copying capability).
bool trustVendorFtpsCa(const char *tag, const char *scratchEnvVar, const char *defaultScratch)
{
    const QString scratch = qEnvironmentVariable(scratchEnvVar, defaultScratch);
    const QString caCertPath = scratch + "/ca-cert.pem";
    const QList<QSslCertificate> caCerts = QSslCertificate::fromPath(caCertPath);
    check(qPrintable(QStringLiteral("[%1] loaded the CA certificate its container's start script "
                                     "copied out (rerun that script if this fails)").arg(tag)),
          !caCerts.isEmpty());
    if (caCerts.isEmpty())
        return false;

    QSslConfiguration config = QSslConfiguration::defaultConfiguration();
    config.addCaCertificate(caCerts.first());
    QSslConfiguration::setDefaultConfiguration(config);
    return true;
}

// tag prefixes every check label (distinguishes phases in the output).
// expectedSampleContent is what THIS phase's server's sample.txt
// actually contains (each Containerfile writes a distinct string so a
// mismatch can't silently point at the wrong server). ftpsMode selects
// plain FTP (the original two phases) vs. explicit FTPS (the two new
// ones) against the exact same server/credentials/content.
bool runVendorRoundTripPhase(const char *tag, quint16 port, const QString &username,
                              const QString &password, const QString &expectedSampleContent,
                              FtpsMode ftpsMode)
{
    const QString localDownloadPath = QStringLiteral("/tmp/zephyrftp_verify_%1_download.txt").arg(tag);
    const QString localUploadSourcePath = QStringLiteral("/tmp/zephyrftp_verify_%1_upload_source.txt").arg(tag);
    const QString localRoundtripBackPath = QStringLiteral("/tmp/zephyrftp_verify_%1_roundtrip_back.txt").arg(tag);
    QFile::remove(localDownloadPath);
    QFile::remove(localRoundtripBackPath);
    {
        QFile f(localUploadSourcePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("uploaded via FtpBackend vendor-diversity verification harness\n");
    }

    FtpCredentials creds;
    creds.host = QStringLiteral("127.0.0.1");
    creds.port = port;
    creds.username = username;
    creds.password = password;
    creds.ftpsMode = ftpsMode;

    FtpBackend backend(creds);

    QEventLoop loop;
    bool connected = false, listed = false, downloadOk = false, uploadOk = false, roundtripDownloadOk = false;
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
            } else if (fileName == localUploadSourcePath && !uploadOk) {
                uploadOk = true;
                // Self-contained container, no host-mounted scratch dir
                // (see this file's header comment) — verify the upload
                // genuinely landed by downloading it back through the
                // same session, not by reading the container's
                // filesystem directly.
                backend.downloadFile("uploads/verify_roundtrip.txt", localRoundtripBackPath, 0);
            } else if (fileName == "uploads/verify_roundtrip.txt") {
                roundtripDownloadOk = true;
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

    check(qPrintable(QStringLiteral("[%1] connected to the real server").arg(tag)), connected);
    check(qPrintable(QStringLiteral("[%1] listDirectory returned real entries").arg(tag)),
          listed && !listedEntries.isEmpty());
    check(qPrintable(QStringLiteral("[%1] downloadFile completed").arg(tag)), downloadOk);
    check(qPrintable(QStringLiteral("[%1] uploadFile completed").arg(tag)), uploadOk);
    check(qPrintable(QStringLiteral("[%1] downloaded the upload back for real (round trip through the "
                                    "server, not the container's own filesystem)").arg(tag)),
          roundtripDownloadOk);
    if (!failureReason.isEmpty())
        fprintf(stderr, "[%s] failure reason: %s\n", tag, qPrintable(failureReason));

    QFile downloaded(localDownloadPath);
    downloaded.open(QIODevice::ReadOnly);
    check(qPrintable(QStringLiteral("[%1] downloaded content matches the real server file").arg(tag)),
          QString::fromUtf8(downloaded.readAll()).trimmed() == expectedSampleContent);

    QFile roundtripBack(localRoundtripBackPath);
    roundtripBack.open(QIODevice::ReadOnly);
    check(qPrintable(QStringLiteral("[%1] uploaded content matches on download-back").arg(tag)),
          QString::fromUtf8(roundtripBack.readAll())
              == "uploaded via FtpBackend vendor-diversity verification harness\n");

    return connected && listed && downloadOk && uploadOk && roundtripDownloadOk;
}

bool runVsftpdPhase()
{
    return runVendorRoundTripPhase("vsftpd", 2126, "vsftpuser", "vsftppass",
                                    "hello from the local vsftpd test server", FtpsMode::None);
}

bool runProftpdPhase()
{
    return runVendorRoundTripPhase("proftpd", 2127, "proftpduser", "proftppass",
                                    "hello from the local proftpd test server", FtpsMode::None);
}

bool runVsftpdFtpsPhase()
{
    if (!trustVendorFtpsCa("vsftpd-ftps", "VSFTPD_TEST_CA_SCRATCH",
                            "/tmp/zephyrftp-local-test-servers/vsftpd-ftps-ca"))
        return false;
    return runVendorRoundTripPhase("vsftpd-ftps", 2126, "vsftpuser", "vsftppass",
                                    "hello from the local vsftpd test server", FtpsMode::Explicit);
}

bool runProftpdFtpsPhase()
{
    if (!trustVendorFtpsCa("proftpd-ftps", "PROFTPD_TEST_CA_SCRATCH",
                            "/tmp/zephyrftp-local-test-servers/proftpd-ftps-ca"))
        return false;
    return runVendorRoundTripPhase("proftpd-ftps", 2127, "proftpduser", "proftppass",
                                    "hello from the local proftpd test server", FtpsMode::Explicit);
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const bool vsftpdOk = runVsftpdPhase();
    const bool proftpdOk = runProftpdPhase();
    const bool vsftpdFtpsOk = runVsftpdFtpsPhase();
    const bool proftpdFtpsOk = runProftpdFtpsPhase();

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return (allPass && vsftpdOk && proftpdOk && vsftpdFtpsOk && proftpdFtpsOk) ? 0 : 1;
}
