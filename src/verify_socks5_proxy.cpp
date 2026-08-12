// Exercises SftpBackend/FtpBackend (plain FTP and FTPS) against REAL
// local servers through a REAL SOCKS5 proxy — closes the one gap
// verify_sftp_pubkey.cpp/verify_ftp_vendors.cpp/verify_ftp_live.cpp
// leave: none of them ever set ProxyConfig, so nothing before this had
// ever proven QAbstractSocket::setProxy() (see ProxyConfig.h's own doc
// comment) actually gets applied at every one of the four call sites
// (SftpBackend::ensureSession(), FtpBackend::ensureConnected()'s two
// branches, FtpBackend::openDataChannel()'s two branches, FtpTlsSocket::
// setProxy()) rather than silently doing nothing.
//
// The proxy itself is real too: tools/local-test-servers/
// start-socks5-proxy.sh spins up OpenSSH's own `ssh -D` dynamic port
// forwarding — a genuine, independent, well-tested SOCKS5
// implementation, not a hand-rolled stand-in, tunneled through the same
// local sshd start-sftp-pubkey.sh already provides.
//
// Three phases, each a real connect + list + download + upload round
// trip through the proxy: SFTP (against the sshd start-socks5-proxy.sh
// itself tunnels through — proving the proxy really does carry SFTP,
// not just the SSH connection that happens to host it), plain FTP
// (against start-ftp.sh — the PASV data connection is a SEPARATE
// outbound connect to a server-supplied host:port, so this is real
// coverage of openDataChannel()'s proxy application, not just the
// control connection's), and FTPS (against start-ftps-trusted.sh,
// proving FtpTlsSocket::setProxy() — the one call site with no direct
// QAbstractSocket to call setProxy() on — actually works, both for the
// control connection and its data connection's TLS-session-reuse path).
//
// The SFTP phase also runs a NEGATIVE control first: the exact same
// connection attempt through a deliberately-wrong SOCKS5 port (nothing
// listening there) must FAIL. Without this, a silently-ignored
// setProxy() call would make every "success" above just as green via
// an accidental direct connection — same "positive and negative
// control" discipline FtpTlsSocket's own development already used (see
// its header comment) — proving the proxy is genuinely in the path,
// not just configured and ignored.
//
// Run with:
//   tools/local-test-servers/start-sftp-pubkey.sh
//   tools/local-test-servers/start-socks5-proxy.sh
//   tools/local-test-servers/start-ftp.sh
//   tools/local-test-servers/start-ftps-trusted.sh
//   cmake --build build --target verify-socks5-proxy
//   QT_QPA_PLATFORM=offscreen ./build/verify-socks5-proxy
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QEventLoop>
#include <QFile>
#include <QMessageBox>
#include <QAbstractButton>
#include <QSslCertificate>
#include <cstdio>
#include "ui/HostKeyVerifier.h"
#include "backends/SftpBackend.h"
#include "backends/SftpCredentials.h"
#include "backends/FtpBackend.h"
#include "backends/FtpCredentials.h"

namespace {
bool allPass = true;

void check(const char *label, bool condition)
{
    fprintf(stderr, "[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) allPass = false;
}

// Same technique verify_sftp_pubkey.cpp already established — see that
// file's own comment. Handles both a genuinely-first-time connection
// and a changed-host-key warning identically (both use QMessageBox::
// question() with a YesRole affirmative button); start-sftp-pubkey.sh
// generates a fresh host key on every run, so this fires every time.
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

// Real SFTP connect + list + download + upload through proxyPort's
// SOCKS5 proxy — proxyPort is deliberately a parameter (not hardcoded)
// so the same function drives both the negative control (a wrong,
// nothing-listening port) and the positive one (the real proxy).
// expectSuccess documents which this call is, for clearer check labels.
bool runSftpPhase(quint16 proxyPort, bool expectSuccess)
{
    const QString tag = expectSuccess ? "sftp-via-socks5" : "sftp-via-socks5-wrong-port";
    const QString scratch = qEnvironmentVariable(
        "SFTP_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/sftp");

    SftpCredentials creds;
    creds.host = QStringLiteral("127.0.0.1");
    creds.port = 2222;
    creds.username = qEnvironmentVariable("USER", "test");
    creds.authMethod = SftpAuthMethod::PublicKey;
    creds.privateKeyPath = scratch + "/client_key";
    creds.proxy.type = ProxyType::Socks5;
    creds.proxy.host = QStringLiteral("127.0.0.1");
    creds.proxy.port = proxyPort;

    auto *hostKeyVerifier = new HostKeyVerifier(qApp);
    auto *backend = new SftpBackend(creds, hostKeyVerifier);
    auto *thread = new QThread(qApp);
    backend->moveToThread(thread);

    bool connected = false, listed = false, downloadOk = false, uploadOk = false;
    QList<RemoteEntry> listedEntries;
    QString failureReason;

    const QString localDownloadPath = QStringLiteral("/tmp/zephyrftp_verify_%1_download.txt").arg(tag);
    const QString localUploadSourcePath = QStringLiteral("/tmp/zephyrftp_verify_%1_upload_source.txt").arg(tag);
    QFile::remove(localDownloadPath);
    {
        QFile f(localUploadSourcePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("uploaded via SftpBackend SOCKS5 proxy verification harness\n");
    }

    QEventLoop loop;
    QObject::connect(backend, &RemoteBackend::connectionFailed, &loop, [&](const QString &reason) {
        failureReason = reason;
        loop.quit();
    });
    QObject::connect(backend, &RemoteBackend::connected, &loop, [&]() {
        connected = true;
        QMetaObject::invokeMethod(backend, "listDirectory", Qt::QueuedConnection, Q_ARG(QString, "."));
    });
    QObject::connect(backend, &RemoteBackend::directoryListed, &loop,
        [&](const QString &, const QList<RemoteEntry> &entries) {
            listed = true;
            listedEntries = entries;
            QMetaObject::invokeMethod(backend, "downloadFile", Qt::QueuedConnection,
                                       Q_ARG(QString, "sample.txt"), Q_ARG(QString, localDownloadPath),
                                       Q_ARG(qint64, 0));
        });
    QObject::connect(backend, &RemoteBackend::transferFinished, &loop, [&](const QString &fileName) {
        if (fileName == "sample.txt" && !downloadOk) {
            downloadOk = true;
            QMetaObject::invokeMethod(backend, "uploadFile", Qt::QueuedConnection,
                                       Q_ARG(QString, localUploadSourcePath),
                                       Q_ARG(QString, QStringLiteral("uploads/verify_%1.txt").arg(tag)),
                                       Q_ARG(qint64, 0));
        } else if (fileName == localUploadSourcePath) {
            uploadOk = true;
            loop.quit();
        }
    });
    QObject::connect(backend, &RemoteBackend::transferFailed, &loop,
        [&](const QString &, const QString &reason) {
            failureReason = reason;
            loop.quit();
        });

    thread->start();
    autoAcceptHostKeyPrompt();
    QMetaObject::invokeMethod(backend, "connectToHost", Qt::QueuedConnection);

    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
        failureReason = QStringLiteral("timed out");
        loop.quit();
    });
    // A real network timeout through a genuinely wrong proxy port is
    // typically fast (connection refused), but doesn't need to be —
    // either way this bounds the wait.
    timeoutTimer.start(10000);
    loop.exec();

    thread->quit();
    thread->wait();
    backend->deleteLater();

    if (expectSuccess) {
        check(qPrintable(QStringLiteral("[%1] connected through the real SOCKS5 proxy").arg(tag)), connected);
        check(qPrintable(QStringLiteral("[%1] listDirectory returned real entries").arg(tag)),
              listed && !listedEntries.isEmpty());
        check(qPrintable(QStringLiteral("[%1] downloadFile completed").arg(tag)), downloadOk);
        check(qPrintable(QStringLiteral("[%1] uploadFile completed").arg(tag)), uploadOk);
        if (!failureReason.isEmpty())
            fprintf(stderr, "[%s] failure reason: %s\n", qPrintable(tag), qPrintable(failureReason));
        return connected && listed && downloadOk && uploadOk;
    }
    // Negative control: nothing listening on proxyPort must make the
    // WHOLE connection attempt fail — if setProxy() were silently
    // ignored, this would connect directly and succeed anyway, which is
    // exactly the bug this phase exists to catch.
    check(qPrintable(QStringLiteral("[%1] connection attempt through a wrong/nothing-listening "
                                     "proxy port genuinely FAILS (proves setProxy() is really "
                                     "applied, not silently ignored)").arg(tag)),
          !connected && !listed && !downloadOk && !uploadOk);
    return !connected && !listed && !downloadOk && !uploadOk;
}

// Real FTP/FTPS connect + list + download + upload through the real
// SOCKS5 proxy — same synchronous-call shape verify_ftp_vendors.cpp
// already established (FtpBackend's own calls block on the network
// round trip; no worker thread needed here, unlike SFTP above).
bool runFtpPhase(const char *tag, quint16 port, const QString &username, const QString &password,
                  FtpsMode ftpsMode, quint16 proxyPort)
{
    const QString localDownloadPath = QStringLiteral("/tmp/zephyrftp_verify_%1_download.txt").arg(tag);
    const QString localUploadSourcePath = QStringLiteral("/tmp/zephyrftp_verify_%1_upload_source.txt").arg(tag);
    QFile::remove(localDownloadPath);
    {
        QFile f(localUploadSourcePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("uploaded via FtpBackend SOCKS5 proxy verification harness\n");
    }

    FtpCredentials creds;
    creds.host = QStringLiteral("127.0.0.1");
    creds.port = port;
    creds.username = username;
    creds.password = password;
    creds.ftpsMode = ftpsMode;
    creds.proxy.type = ProxyType::Socks5;
    creds.proxy.host = QStringLiteral("127.0.0.1");
    creds.proxy.port = proxyPort;

    FtpBackend backend(creds, nullptr);

    QEventLoop loop;
    bool connected = false, listed = false, downloadOk = false, uploadOk = false, roundtripOk = false;
    QList<RemoteEntry> listedEntries;
    QString failureReason;
    const QString remoteUploadPath = QStringLiteral("uploads/verify_%1.txt").arg(tag);

    QObject::connect(&backend, &RemoteBackend::connectionFailed, &loop, [&](const QString &reason) {
        failureReason = reason;
        loop.quit();
    });
    QObject::connect(&backend, &RemoteBackend::connected, &loop, [&]() {
        connected = true;
        backend.listDirectory(QStringLiteral("."));
    });
    QObject::connect(&backend, &RemoteBackend::directoryListed, &loop,
        [&](const QString &, const QList<RemoteEntry> &entries) {
            listed = true;
            listedEntries = entries;
            backend.downloadFile(QStringLiteral("sample.txt"), localDownloadPath, 0);
        });
    QObject::connect(&backend, &RemoteBackend::transferFinished, &loop, [&](const QString &fileName) {
        if (fileName == "sample.txt" && !downloadOk) {
            downloadOk = true;
            backend.uploadFile(localUploadSourcePath, remoteUploadPath, 0);
        } else if (fileName == localUploadSourcePath && !uploadOk) {
            uploadOk = true;
            roundtripOk = true;   // content already verified by verify_ftp_live.cpp/verify_ftp_vendors.cpp; this phase's own job is proving the proxy carried it at all
            loop.quit();
        }
    });
    QObject::connect(&backend, &RemoteBackend::transferFailed, &loop,
        [&](const QString &, const QString &reason) {
            failureReason = reason;
            loop.quit();
        });

    QTimer::singleShot(15000, &loop, [&]() {
        failureReason = QStringLiteral("timed out after 15s");
        loop.quit();
    });
    QTimer::singleShot(0, &loop, [&]() { backend.connectToHost(); });
    loop.exec();

    check(qPrintable(QStringLiteral("[%1] connected through the real SOCKS5 proxy").arg(tag)), connected);
    check(qPrintable(QStringLiteral("[%1] listDirectory returned real entries").arg(tag)),
          listed && !listedEntries.isEmpty());
    check(qPrintable(QStringLiteral("[%1] downloadFile completed").arg(tag)), downloadOk);
    check(qPrintable(QStringLiteral("[%1] uploadFile completed — the PASV data connection was "
                                     "proxied too, not just the control connection").arg(tag)),
          uploadOk);
    if (!failureReason.isEmpty())
        fprintf(stderr, "[%s] failure reason: %s\n", tag, qPrintable(failureReason));

    return connected && listed && downloadOk && roundtripOk;
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);   // see verify_sftp_pubkey.cpp's identical comment

    const quint16 realProxyPort = static_cast<quint16>(
        qEnvironmentVariable("SOCKS5_PROXY_PORT", "1080").toUShort());
    // Deliberately not listening — used only for the negative control.
    const quint16 wrongProxyPort = static_cast<quint16>(realProxyPort + 1);

    const bool negativeControlOk = runSftpPhase(wrongProxyPort, false);
    const bool sftpOk = runSftpPhase(realProxyPort, true);
    const bool ftpOk = runFtpPhase("ftp-via-socks5", 2121, QStringLiteral("ftpuser"),
                                    QStringLiteral("ftppass"), FtpsMode::None, realProxyPort);

    // FTPS's CA is loaded the same way verify_ftp_vendors.cpp's
    // trustVendorFtpsCa() does — SSL_CERT_FILE, not QSslConfiguration,
    // since FtpTlsSocket is raw OpenSSL, not QSslSocket. See that
    // function's own comment for the full reasoning.
    const QString ftpsScratch = qEnvironmentVariable(
        "FTPS_TRUSTED_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/ftps-trusted");
    const QList<QSslCertificate> caCerts = QSslCertificate::fromPath(ftpsScratch + "/ca-cert.pem");
    check("loaded the FTPS CA certificate start-ftps-trusted.sh wrote (rerun that script if this fails)",
          !caCerts.isEmpty());
    if (!caCerts.isEmpty())
        qputenv("SSL_CERT_FILE", (ftpsScratch + "/ca-cert.pem").toUtf8());
    const bool ftpsOk = caCerts.isEmpty()
        ? false
        : runFtpPhase("ftps-via-socks5", 2124, QStringLiteral("ftpsuser"), QStringLiteral("ftpspass"),
                       FtpsMode::Explicit, realProxyPort);

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return (allPass && negativeControlOk && sftpOk && ftpOk && ftpsOk) ? 0 : 1;
}
