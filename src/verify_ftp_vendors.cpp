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
// the SSL_CERT_FILE env var (see trustVendorFtpsCa()) — appropriate
// here too since this is a plain QCoreApplication harness with no GUI
// event loop for a TOFU dialog to run in.
//
// vsftpd-ftps AND proftpd-ftps now BOTH genuinely pass a full round
// trip — connect, list, download, upload, content verified both ways,
// over real FTPS against a real vendor, including proftpd's mod_tls
// left at its default STRICT session-reuse enforcement (see
// containers/proftpd.conf's comment). Getting the vsftpd phase working
// found and fixed three real, independent client-side bugs in
// FtpBackend.cpp's handling of a server that closes a data connection
// uncleanly (no TLS close_notify) once it's actually done with it, in
// both directions; getting the proftpd phase working needed replacing
// FTPS's TLS layer entirely (FtpTlsSocket, raw OpenSSL forced to TLS
// 1.2, genuine SSL_SESSION reuse) after confirming Qt's QSslSocket has
// no public API that can satisfy mod_tls's strict check — see
// ARCHITECTURE.md's Known Gaps and FtpTlsSocket.h's own doc comment for
// the full story either way. vsftpd.conf still deliberately ships with
// require_ssl_reuse left OFF — a real, confirmed deadlock in vsftpd's
// own privilege-separated architecture under this container environment
// when it's on, unrelated to session-ticket reuse (identical hang with
// reuse disabled entirely) and not something a client-side change can
// fix; see that file's own comment.
//
// The plain (non-FTPS) vsftpd and proftpd phases additionally exercise
// FtpBackend::setPermissions() — a real SITE CHMOD against real vendor
// software, closing the gap verify_sftp_pubkey.cpp's own chmod
// verification leaves (that one only proves libssh2_sftp_setstat()
// against SFTP; SITE CHMOD is a completely different, non-standard FTP
// extension with its own vendor-support question). Not repeated in the
// two FTPS phases: SITE CHMOD's handling is identical over the
// encrypted command channel (same sendCommand()/FtpReply mechanism,
// TLS is transparent to it) — a second run would exercise the TLS
// layer again, not this new code path.
//
// Run with:
//   tools/local-test-servers/start-vsftpd.sh
//   tools/local-test-servers/start-proftpd.sh
//   cmake --build build --target verify-ftp-vendors
//   QT_QPA_PLATFORM=offscreen ./build/verify-ftp-vendors
#include <QCoreApplication>
#include <QTimer>
#include <QFile>
#include <QProcess>
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
// identical pattern this mirrors) and trusts it for real FTPS
// connections — pure test-harness code, FtpBackend itself is never told
// about any extra CA. Returns false (and reports why) if the scratch
// file isn't there, most likely because the matching start script
// hasn't been run (or predates this CA-copying capability).
//
// Sets the SSL_CERT_FILE env var (OpenSSL's own standard override,
// respected by SSL_CTX_set_default_verify_paths() — see
// FtpTlsSocket::handshake()) rather than (or in addition to) injecting
// into QSslConfiguration's process-wide default: FTPS's control/data
// connections are FtpTlsSocket (raw OpenSSL), not QSslSocket, so
// nothing reads Qt's own CA list anymore. Read fresh by every
// handshake(), so it's safe to call this again for a later phase with
// a different CA — no cross-phase leftover trust the way a single
// process-wide QSslConfiguration would risk.
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

    qputenv("SSL_CERT_FILE", caCertPath.toUtf8());
    return true;
}

// tag prefixes every check label (distinguishes phases in the output).
// expectedSampleContent is what THIS phase's server's sample.txt
// actually contains (each Containerfile writes a distinct string so a
// mismatch can't silently point at the wrong server). ftpsMode selects
// plain FTP (the original two phases) vs. explicit FTPS (the two new
// ones) against the exact same server/credentials/content. testChmod
// additionally exercises a real SITE CHMOD (see this file's header
// comment for why it's only set true for the two plain-FTP phases);
// containerName is only used when testChmod is true (see its own
// verification code below for why).
bool runVendorRoundTripPhase(const char *tag, quint16 port, const QString &username,
                              const QString &password, const QString &expectedSampleContent,
                              FtpsMode ftpsMode, bool testChmod, const char *containerName = nullptr)
{
    const QString localDownloadPath = QStringLiteral("/tmp/zephyrftp_verify_%1_download.txt").arg(tag);
    const QString localUploadSourcePath = QStringLiteral("/tmp/zephyrftp_verify_%1_upload_source.txt").arg(tag);
    const QString localRoundtripBackPath = QStringLiteral("/tmp/zephyrftp_verify_%1_roundtrip_back.txt").arg(tag);
    // Remote path is tag-qualified too, not just the local scratch
    // files — a real bug found by running this after the FTPS phases
    // could finally reach it (previously never exercised: the TLS layer
    // itself failed first): the "proftpd" (plain) and "proftpd-ftps"
    // phases both target the SAME server/filesystem, so a shared,
    // un-tagged remote path meant the second phase's STOR hit a file
    // the first phase had already created in THIS SAME run — denied by
    // proftpd's default AllowOverwrite off, a real server policy, not a
    // client bug, but not what this phase means to test either.
    const QString remoteUploadPath = QStringLiteral("uploads/verify_roundtrip_%1.txt").arg(tag);
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

    // Chmod-phase state — only ever populated when testChmod is true (see
    // this file's header comment for why that's just the two plain-FTP
    // phases).
    bool chmodFailed = false;
    QString chmodFailureReason;

    QObject::connect(&backend, &RemoteBackend::connectionFailed, &loop,
        [&](const QString &reason) { failureReason = reason; loop.quit(); });

    QObject::connect(&backend, &RemoteBackend::fileOperationFailed, &loop,
        [&](const QString &operation, const QString &, const QString &reason) {
            if (operation == QStringLiteral("Change permissions")) {
                chmodFailed = true;
                chmodFailureReason = reason;
            }
        });

    QObject::connect(&backend, &RemoteBackend::connected, &loop, [&]() {
        connected = true;
        backend.listDirectory(".");
    });

    QObject::connect(&backend, &RemoteBackend::directoryListed, &loop,
        [&](const QString &, const QList<RemoteEntry> &entries) {
            // Also fires again after a successful setPermissions() below
            // (its own "fire and refresh" contract re-lists m_currentPath)
            // — harmless no-op the second time, nothing here depends on
            // that listing's content.
            if (listed)
                return;
            listed = true;
            listedEntries = entries;
            backend.downloadFile("sample.txt", localDownloadPath, 0);
        });

    QObject::connect(&backend, &RemoteBackend::transferFinished, &loop,
        [&](const QString &fileName) {
            if (fileName == "sample.txt" && !downloadOk) {
                downloadOk = true;
                backend.uploadFile(localUploadSourcePath, remoteUploadPath, 0);
            } else if (fileName == localUploadSourcePath && !uploadOk) {
                uploadOk = true;
                // Self-contained container, no host-mounted scratch dir
                // (see this file's header comment) — verify the upload
                // genuinely landed by downloading it back through the
                // same session, not by reading the container's
                // filesystem directly.
                backend.downloadFile(remoteUploadPath, localRoundtripBackPath, 0);
            } else if (fileName == remoteUploadPath) {
                roundtripDownloadOk = true;
                if (testChmod) {
                    // Synchronous (sendCommand() blocks on the network
                    // round trip) — by the time this returns, the
                    // fileOperationFailed connection above (if it fired)
                    // has already updated chmodFailed/chmodFailureReason.
                    backend.setPermissions(remoteUploadPath, 0640);
                }
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

    bool chmodModeConfirmed = false;
    if (testChmod) {
        if (chmodFailed)
            fprintf(stderr, "[%s] SITE CHMOD failure reason: %s\n", tag, qPrintable(chmodFailureReason));
        check(qPrintable(QStringLiteral("[%1] SITE CHMOD: server accepted the command").arg(tag)),
              !chmodFailed);

        // Can't verify the applied mode by re-listing through FtpBackend
        // itself: its LIST/MLSD parsers deliberately hardcode
        // RemoteEntry::permissions to "-" for every FTP entry (a
        // pre-existing, disclosed display limitation — see
        // FtpBackend.cpp's own comments at each entry->permissions
        // assignment — unrelated to chmod and not something this feature
        // needs to fix). Ground truth instead, straight from the
        // container's own filesystem via `podman exec`/`stat`, entirely
        // independent of this client's own listing code — this is what
        // actually caught that SITE CHMOD itself was working correctly
        // the first time this phase was run, when the RemoteEntry-based
        // check above was still in place and failing for the wrong
        // reason.
        QProcess stat;
        stat.start(QStringLiteral("podman"),
                   {QStringLiteral("exec"), QString::fromLatin1(containerName), QStringLiteral("stat"),
                    QStringLiteral("-c"), QStringLiteral("%a"),
                    QStringLiteral("/srv/ftp/%1").arg(remoteUploadPath)});
        stat.waitForFinished(5000);
        const QString actualMode = QString::fromUtf8(stat.readAllStandardOutput()).trimmed();
        chmodModeConfirmed = (actualMode == QStringLiteral("640"));
        if (!chmodModeConfirmed)
            fprintf(stderr, "[%s] podman exec stat reported mode: \"%s\" (stderr: %s)\n", tag,
                    qPrintable(actualMode), qPrintable(QString::fromUtf8(stat.readAllStandardError())));
        check(qPrintable(QStringLiteral("[%1] SITE CHMOD: container's own filesystem confirms mode 640 "
                                         "was actually applied").arg(tag)),
              chmodModeConfirmed);
    }

    return connected && listed && downloadOk && uploadOk && roundtripDownloadOk
        && (!testChmod || (!chmodFailed && chmodModeConfirmed));
}

bool runVsftpdPhase()
{
    return runVendorRoundTripPhase("vsftpd", 2126, "vsftpuser", "vsftppass",
                                    "hello from the local vsftpd test server", FtpsMode::None, true,
                                    "zephyrftp-test-vsftpd");
}

bool runProftpdPhase()
{
    return runVendorRoundTripPhase("proftpd", 2127, "proftpduser", "proftppass",
                                    "hello from the local proftpd test server", FtpsMode::None, true,
                                    "zephyrftp-test-proftpd");
}

bool runVsftpdFtpsPhase()
{
    if (!trustVendorFtpsCa("vsftpd-ftps", "VSFTPD_TEST_CA_SCRATCH",
                            "/tmp/zephyrftp-local-test-servers/vsftpd-ftps-ca"))
        return false;
    return runVendorRoundTripPhase("vsftpd-ftps", 2126, "vsftpuser", "vsftppass",
                                    "hello from the local vsftpd test server", FtpsMode::Explicit, false);
}

bool runProftpdFtpsPhase()
{
    if (!trustVendorFtpsCa("proftpd-ftps", "PROFTPD_TEST_CA_SCRATCH",
                            "/tmp/zephyrftp-local-test-servers/proftpd-ftps-ca"))
        return false;
    return runVendorRoundTripPhase("proftpd-ftps", 2127, "proftpduser", "proftppass",
                                    "hello from the local proftpd test server", FtpsMode::Explicit, false);
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
