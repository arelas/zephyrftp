// Exercises FtpBackend's IMPLICIT FTPS mode against a REAL server —
// closes the gap that every other FTPS verification in this project
// (verify-ftps-trust, verify-ftp-vendors, verify-ftp-live) only ever
// exercises EXPLICIT FTPS. Needs tools/local-test-servers/
// start-vsftpd-implicit.sh already running (a real vsftpd instance
// configured with implicit_ssl=YES — pyftpdlib's TLS_FTPHandler has no
// equivalent mode at all, confirmed by reading its source before
// choosing vsftpd here); this is NOT one of the required, self-contained
// EXCLUDE_FROM_ALL test targets (it has an external precondition those
// deliberately don't), so it isn't part of that suite or CI — same
// treatment as verify-ftps-trust/verify-ftp-vendors.
//
// A single real connection, then a full transfer round trip — list,
// download the server's own sample.txt (content verified), upload a
// fresh file, download it back through the SAME session (not the
// container's filesystem — this container is self-contained, same
// reasoning verify_ftp_vendors.cpp's own header comment gives), content
// verified again. The transfer round trip specifically is what proves
// the real, security-relevant bug found while implementing this feature
// actually stays fixed: FtpBackend::usesTls() replaced four independent
// `ftpsMode == FtpsMode::Explicit` checks (control connection, PASV
// data connection, active/PORT server socket, data-channel adoption) —
// left as Explicit-only, the control connection would connect and
// authenticate fine, but every DATA connection (list, download, upload)
// would fail, since m_dataProtected would be true (PBSZ/PROT P succeed
// identically for both modes) while the data socket itself was never
// actually TLS-capable. A trust prompt (same real, auto-accepted
// QMessageBox technique verify_sftp_pubkey.cpp/verify_ftps_trust.cpp
// already establish) is expected on the first connection to this
// host:port — a fresh, never-before-seen implicit-FTPS certificate.
//
// Run with:
//   tools/local-test-servers/start-vsftpd-implicit.sh
//   cmake --build build --target verify-ftps-implicit
//   QT_QPA_PLATFORM=offscreen ./build/verify-ftps-implicit
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QEventLoop>
#include <QMessageBox>
#include <QAbstractButton>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <cstdio>
#include <memory>
#include "ui/CertificateVerifier.h"
#include "backends/FtpBackend.h"
#include "backends/FtpCredentials.h"

namespace {
bool allPass = true;

void check(const char *label, bool condition)
{
    fprintf(stderr, "[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) allPass = false;
}

const char *kHost = "127.0.0.1";
constexpr int kPort = 2128;

QString knownCertsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + QStringLiteral("/known_certs.json");
}

// Same reasoning as verify_ftps_trust.cpp's identical helper — starts
// this run genuinely at "never seen this certificate before" rather
// than depending on whatever a previous run left behind.
void clearStoredFingerprint()
{
    QJsonArray kept;
    QFile file(knownCertsPath());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();
        if (doc.isArray()) {
            for (const QJsonValue &value : doc.array()) {
                const QJsonObject obj = value.toObject();
                if (obj.value(QStringLiteral("host")).toString() == QLatin1String(kHost)
                    && obj.value(QStringLiteral("port")).toInt() == kPort) {
                    continue;
                }
                kept.append(obj);
            }
        }
    }
    QFile writeFile(knownCertsPath());
    if (writeFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        writeFile.write(QJsonDocument(kept).toJson());
}

// Same "poll the active modal, click its button" technique
// verify_sftp_pubkey.cpp/verify_ftps_trust.cpp already establish,
// including verify_ftps_trust.cpp's own `active` flag: this is a
// SINGLE-phase test (no risk of racing a later phase's own poller the
// way that file's own comment describes), but the connection could
// still finish (success or failure) without a prompt ever appearing at
// all, leaving one already-scheduled poll still pending — `active`
// being flipped false once the event loop returns turns that into a
// harmless no-op instead of a dangling-pointer access into a
// std::shared_ptr the last real reference to which may otherwise have
// already gone out of scope.
struct PromptWatcher {
    bool sawPrompt = false;
    bool active = true;
};

void autoAcceptTrustPrompt(const std::shared_ptr<PromptWatcher> &watcher)
{
    QTimer::singleShot(50, qApp, [watcher] {
        if (!watcher->active)
            return;
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (box) {
            watcher->sawPrompt = true;
            for (QAbstractButton *button : box->buttons()) {
                if (box->buttonRole(button) == QMessageBox::YesRole) {
                    button->click();
                    return;
                }
            }
            return;
        }
        autoAcceptTrustPrompt(watcher);
    });
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    clearStoredFingerprint();

    const QString scratchDir = QDir::tempPath() + QStringLiteral("/zephyrftp_verify_ftps_implicit");
    QDir(scratchDir).removeRecursively();
    QDir().mkpath(scratchDir);
    const QString localDownloadPath = scratchDir + QStringLiteral("/sample_downloaded.txt");
    const QString localUploadSourcePath = scratchDir + QStringLiteral("/to_upload.txt");
    const QString localRoundtripBackPath = scratchDir + QStringLiteral("/roundtrip_back.txt");
    const QString remoteUploadPath = QStringLiteral("uploads/implicit_ftps_probe.txt");
    const QString uploadContent = QStringLiteral(
        "uploaded via FtpBackend implicit-FTPS verification harness\n");

    {
        QFile f(localUploadSourcePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(uploadContent.toUtf8());
    }

    FtpCredentials creds;
    creds.host = QLatin1String(kHost);
    creds.port = kPort;
    creds.username = QStringLiteral("vsftpuser");
    creds.password = QStringLiteral("vsftppass");
    creds.ftpsMode = FtpsMode::Implicit;

    auto *verifier = new CertificateVerifier();
    auto *backend = new FtpBackend(creds, verifier);
    auto *thread = new QThread();
    backend->moveToThread(thread);

    QEventLoop loop;
    bool connected = false, listed = false, downloadOk = false, uploadOk = false, roundtripOk = false;
    QList<RemoteEntry> listedEntries;
    QString failureReason;
    auto watcher = std::make_shared<PromptWatcher>();

    QObject::connect(backend, &RemoteBackend::connectionFailed, &loop,
        [&](const QString &reason) { failureReason = reason; loop.quit(); });

    QObject::connect(backend, &RemoteBackend::connected, &loop, [&]() {
        connected = true;
        QMetaObject::invokeMethod(backend, "listDirectory", Qt::QueuedConnection, Q_ARG(QString, QStringLiteral(".")));
    });

    QObject::connect(backend, &RemoteBackend::directoryListed, &loop,
        [&](const QString &, const QList<RemoteEntry> &entries) {
            if (listed)
                return;
            listed = true;
            listedEntries = entries;
            QMetaObject::invokeMethod(backend, "downloadFile", Qt::QueuedConnection,
                                       Q_ARG(QString, QStringLiteral("sample.txt")),
                                       Q_ARG(QString, localDownloadPath), Q_ARG(qint64, 0));
        });

    QObject::connect(backend, &RemoteBackend::transferFinished, &loop,
        [&](const QString &fileName) {
            if (fileName == QStringLiteral("sample.txt") && !downloadOk) {
                downloadOk = true;
                QMetaObject::invokeMethod(backend, "uploadFile", Qt::QueuedConnection,
                                           Q_ARG(QString, localUploadSourcePath),
                                           Q_ARG(QString, remoteUploadPath), Q_ARG(qint64, 0));
            } else if (fileName == localUploadSourcePath && !uploadOk) {
                uploadOk = true;
                QMetaObject::invokeMethod(backend, "downloadFile", Qt::QueuedConnection,
                                           Q_ARG(QString, remoteUploadPath),
                                           Q_ARG(QString, localRoundtripBackPath), Q_ARG(qint64, 0));
            } else if (fileName == remoteUploadPath) {
                roundtripOk = true;
                loop.quit();
            }
        });

    QObject::connect(backend, &RemoteBackend::transferFailed, &loop,
        [&](const QString &fileName, const QString &reason) {
            failureReason = QStringLiteral("transfer of %1 failed: %2").arg(fileName, reason);
            loop.quit();
        });

    QTimer::singleShot(15000, &loop, [&]() {
        if (failureReason.isEmpty())
            failureReason = QStringLiteral("timed out after 15s");
        loop.quit();
    });

    thread->start();
    autoAcceptTrustPrompt(watcher);
    QMetaObject::invokeMethod(backend, "connectToHost", Qt::QueuedConnection);
    loop.exec();
    watcher->active = false;   // any still-pending poll becomes a harmless no-op from here on

    check("connected to the real implicit-FTPS server (TLS from the first byte, no AUTH TLS)", connected);
    check("a trust prompt appeared for this never-before-seen certificate", watcher->sawPrompt);
    check("a real directory listing succeeded", listed && !listedEntries.isEmpty());
    check("downloadFile (over the PASV data channel) completed", downloadOk);
    check("uploadFile (over the PASV data channel) completed", uploadOk);
    check("downloaded the upload back for real (round trip through the server)", roundtripOk);
    if (!failureReason.isEmpty())
        fprintf(stderr, "failure reason: %s\n", qPrintable(failureReason));

    QFile downloaded(localDownloadPath);
    downloaded.open(QIODevice::ReadOnly);
    check("downloaded content matches the real server's sample.txt",
          QString::fromUtf8(downloaded.readAll()).trimmed()
              == QStringLiteral("hello from the local implicit-FTPS vsftpd test server"));

    QFile roundtripBack(localRoundtripBackPath);
    roundtripBack.open(QIODevice::ReadOnly);
    check("uploaded content matches exactly on download-back (proves the DATA channel is genuinely "
          "TLS-protected, not silently falling back to plaintext or failing)",
          QString::fromUtf8(roundtripBack.readAll()) == uploadContent);

    backend->deleteLater();
    thread->quit();
    thread->wait();
    delete thread;
    delete verifier;

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
