// Exercises SftpBackend/FtpBackend (plain FTP and FTPS) against REAL
// local servers through a REAL HTTP CONNECT proxy (tinyproxy, in a
// throwaway podman container — see tools/local-test-servers/
// start-http-connect-proxy.sh and containers/Containerfile.tinyproxy).
// Same shape and same reasoning as verify_socks5_proxy.cpp (see that
// file's own header comment for the fuller story of why raw fd
// extraction needed a hand-rolled handshake instead of
// QAbstractSocket::setProxy() — ProxyConnect.h/.cpp), just proving the
// OTHER proxy type/mechanism (QNetworkProxy::HttpProxy's CONNECT verb,
// not SOCKS5) against a genuinely different, independent proxy
// implementation.
//
// tinyproxy's BasicAuth is enabled (see tinyproxy.conf) — unlike
// verify_socks5_proxy.cpp's ssh -D proxy (which needs no auth at all),
// this gives real coverage of ProxyConnect.cpp's Proxy-Authorization
// header path. The negative control here is deliberately WRONG
// credentials (proving that header is genuinely checked, not just
// sent) rather than verify_socks5_proxy.cpp's wrong-port control —
// different proxy, different thing worth proving wrong on purpose.
//
// Run with:
//   tools/local-test-servers/start-sftp-pubkey.sh
//   tools/local-test-servers/start-http-connect-proxy.sh
//   tools/local-test-servers/start-ftp.sh
//   tools/local-test-servers/start-ftps-trusted.sh
//   cmake --build build --target verify-http-connect-proxy
//   QT_QPA_PLATFORM=offscreen ./build/verify-http-connect-proxy
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

// Same technique verify_sftp_pubkey.cpp/verify_socks5_proxy.cpp already
// established — see either file's own comment.
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

// proxyPassword is a parameter (not hardcoded) so the same function
// drives both the negative control (wrong credentials) and the
// positive one (the real password tinyproxy.conf's BasicAuth expects).
bool runSftpPhase(const QString &proxyPassword, bool expectSuccess)
{
    const QString tag = expectSuccess ? "sftp-via-http-connect" : "sftp-via-http-connect-wrong-creds";
    const QString scratch = qEnvironmentVariable(
        "SFTP_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/sftp");
    const quint16 proxyPort = static_cast<quint16>(
        qEnvironmentVariable("TINYPROXY_TEST_PORT", "8888").toUShort());

    SftpCredentials creds;
    creds.host = QStringLiteral("127.0.0.1");
    creds.port = 2222;
    creds.username = qEnvironmentVariable("USER", "test");
    creds.authMethod = SftpAuthMethod::PublicKey;
    creds.privateKeyPath = scratch + "/client_key";
    creds.proxy.type = ProxyType::Http;
    creds.proxy.host = QStringLiteral("127.0.0.1");
    creds.proxy.port = proxyPort;
    creds.proxy.username = QStringLiteral("proxyuser");
    creds.proxy.password = proxyPassword;

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
        f.write("uploaded via SftpBackend HTTP CONNECT proxy verification harness\n");
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
    timeoutTimer.start(10000);
    loop.exec();

    thread->quit();
    thread->wait();
    backend->deleteLater();

    if (expectSuccess) {
        check(qPrintable(QStringLiteral("[%1] connected through the real HTTP CONNECT proxy").arg(tag)), connected);
        check(qPrintable(QStringLiteral("[%1] listDirectory returned real entries").arg(tag)),
              listed && !listedEntries.isEmpty());
        check(qPrintable(QStringLiteral("[%1] downloadFile completed").arg(tag)), downloadOk);
        check(qPrintable(QStringLiteral("[%1] uploadFile completed").arg(tag)), uploadOk);
        if (!failureReason.isEmpty())
            fprintf(stderr, "[%s] failure reason: %s\n", qPrintable(tag), qPrintable(failureReason));
        return connected && listed && downloadOk && uploadOk;
    }
    check(qPrintable(QStringLiteral("[%1] connection attempt with wrong proxy credentials genuinely "
                                     "FAILS (proves Proxy-Authorization is really checked by tinyproxy, "
                                     "not just sent)").arg(tag)),
          !connected && !listed && !downloadOk && !uploadOk);
    return !connected && !listed && !downloadOk && !uploadOk;
}

// Real FTP/FTPS connect + list + download + upload through the real
// HTTP CONNECT proxy — same synchronous-call shape verify_ftp_vendors.cpp/
// verify_socks5_proxy.cpp already established.
bool runFtpPhase(const char *tag, quint16 port, const QString &username, const QString &password,
                  FtpsMode ftpsMode, quint16 proxyPort)
{
    const QString localDownloadPath = QStringLiteral("/tmp/zephyrftp_verify_%1_download.txt").arg(tag);
    const QString localUploadSourcePath = QStringLiteral("/tmp/zephyrftp_verify_%1_upload_source.txt").arg(tag);
    QFile::remove(localDownloadPath);
    {
        QFile f(localUploadSourcePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("uploaded via FtpBackend HTTP CONNECT proxy verification harness\n");
    }

    FtpCredentials creds;
    creds.host = QStringLiteral("127.0.0.1");
    creds.port = port;
    creds.username = username;
    creds.password = password;
    creds.ftpsMode = ftpsMode;
    creds.proxy.type = ProxyType::Http;
    creds.proxy.host = QStringLiteral("127.0.0.1");
    creds.proxy.port = proxyPort;
    creds.proxy.username = QStringLiteral("proxyuser");
    creds.proxy.password = QStringLiteral("proxypass");

    FtpBackend backend(creds, nullptr);

    QEventLoop loop;
    bool connected = false, listed = false, downloadOk = false, uploadOk = false;
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

    check(qPrintable(QStringLiteral("[%1] connected through the real HTTP CONNECT proxy").arg(tag)), connected);
    check(qPrintable(QStringLiteral("[%1] listDirectory returned real entries").arg(tag)),
          listed && !listedEntries.isEmpty());
    check(qPrintable(QStringLiteral("[%1] downloadFile completed").arg(tag)), downloadOk);
    check(qPrintable(QStringLiteral("[%1] uploadFile completed — the PASV data connection was "
                                     "proxied too, not just the control connection").arg(tag)),
          uploadOk);
    if (!failureReason.isEmpty())
        fprintf(stderr, "[%s] failure reason: %s\n", tag, qPrintable(failureReason));

    return connected && listed && downloadOk && uploadOk;
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    const quint16 proxyPort = static_cast<quint16>(
        qEnvironmentVariable("TINYPROXY_TEST_PORT", "8888").toUShort());

    const bool negativeControlOk = runSftpPhase(QStringLiteral("wrong-password"), false);
    const bool sftpOk = runSftpPhase(QStringLiteral("proxypass"), true);
    const bool ftpOk = runFtpPhase("ftp-via-http-connect", 2121, QStringLiteral("ftpuser"),
                                    QStringLiteral("ftppass"), FtpsMode::None, proxyPort);

    const QString ftpsScratch = qEnvironmentVariable(
        "FTPS_TRUSTED_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/ftps-trusted");
    const QList<QSslCertificate> caCerts = QSslCertificate::fromPath(ftpsScratch + "/ca-cert.pem");
    check("loaded the FTPS CA certificate start-ftps-trusted.sh wrote (rerun that script if this fails)",
          !caCerts.isEmpty());
    if (!caCerts.isEmpty())
        qputenv("SSL_CERT_FILE", (ftpsScratch + "/ca-cert.pem").toUtf8());
    const bool ftpsOk = caCerts.isEmpty()
        ? false
        : runFtpPhase("ftps-via-http-connect", 2124, QStringLiteral("ftpsuser"), QStringLiteral("ftpspass"),
                       FtpsMode::Explicit, proxyPort);

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return (allPass && negativeControlOk && sftpOk && ftpOk && ftpsOk) ? 0 : 1;
}
