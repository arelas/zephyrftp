// Exercises SftpBackend's new per-transfer bandwidth throttle
// (SftpCredentials::bandwidthLimitKBps, applied via BandwidthThrottle —
// see that class's own doc comment) against a REAL server — closing the
// gap bandwidth-throttle-test structurally can't: that test proves
// BandwidthThrottle's own pacing math in isolation (fake byte counts, no
// real I/O); this proves a REAL SftpBackend upload, over a real network
// connection, actually achieves close to the configured real-world KB/s
// rather than the link's genuine unthrottled speed.
//
// Two uploads, same fixture size, same real local server, one after the
// other (not concurrent — a clean, unambiguous rate comparison, not
// entangled with this project's separate concurrent-transfer work):
//   1. bandwidthLimitKBps = 0 (unlimited) — establishes this machine's
//      real, unthrottled loopback SFTP upload rate.
//   2. bandwidthLimitKBps = a real configured limit — confirms the
//      achieved rate lands close to that configured number, not the
//      much higher unthrottled rate from step 1.
//
// Needs tools/local-test-servers/start-sftp-pubkey.sh already running;
// NOT one of the required EXCLUDE_FROM_ALL targets (external
// precondition: one real server), so not part of that suite or CI — same
// treatment as verify-sftp-pause-cancel/verify-sftp-throughput.
//
// Run with:
//   tools/local-test-servers/start-sftp-pubkey.sh
//   cmake --build build --target verify-bandwidth-throttle-live
//   QT_QPA_PLATFORM=offscreen SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp \
//       ./build/verify-bandwidth-throttle-live
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QAbstractButton>
#include <QRandomGenerator>
#include <QCryptographicHash>
#include <cstdio>
#include "ui/HostKeyVerifier.h"
#include "backends/SftpBackend.h"
#include "backends/SftpCredentials.h"

namespace {
bool allPass = true;

void check(const char *label, bool condition)
{
    fprintf(stderr, "[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) allPass = false;
}

// Same technique verify_sftp_pause_cancel.cpp/verify_sftp_throughput.cpp
// already use.
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

bool generateTestFile(const QString &path, qint64 sizeBytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QByteArray chunk(64 * 1024, Qt::Uninitialized);
    qint64 written = 0;
    while (written < sizeBytes) {
        for (int i = 0; i < chunk.size(); i += 4)
            *reinterpret_cast<quint32 *>(chunk.data() + i) = QRandomGenerator::global()->generate();
        const qint64 toWrite = qMin<qint64>(chunk.size(), sizeBytes - written);
        if (f.write(chunk.constData(), toWrite) != toWrite) return false;
        written += toWrite;
    }
    return true;
}

// Runs one upload to completion against a fresh SftpBackend/QThread with
// the given bandwidthLimitKBps, returns the measured KB/s (0.0 on
// failure — checked by the caller). hostKeyVerifier/scratch/port are
// shared across both calls; each call gets its own backend/thread/
// remote filename so the two runs can never interfere with each other.
double runUploadAndMeasureKBps(HostKeyVerifier *hostKeyVerifier, const QString &scratch, int port,
                                const QString &localPath, qint64 fileSizeBytes,
                                int bandwidthLimitKBps, const QString &remoteFileName)
{
    SftpCredentials creds;
    creds.host = "127.0.0.1";
    creds.port = port;
    creds.username = qEnvironmentVariable("USER", "test");
    creds.authMethod = SftpAuthMethod::PublicKey;
    creds.privateKeyPath = scratch + "/client_key";
    creds.bandwidthLimitKBps = bandwidthLimitKBps;

    auto *backend = new SftpBackend(creds, hostKeyVerifier);
    auto *thread = new QThread;
    backend->moveToThread(thread);

    bool connected = false, uploadOk = false;
    QString failureReason;
    QElapsedTimer timer;
    qint64 elapsedMs = 0;

    QObject::connect(backend, &RemoteBackend::connectionFailed, qApp, [&](const QString &reason) {
        failureReason = reason;
        qApp->quit();
    });
    QObject::connect(backend, &RemoteBackend::connected, qApp, [&]() {
        connected = true;
        timer.start();
        QMetaObject::invokeMethod(backend, "uploadFile", Qt::QueuedConnection,
                                   Q_ARG(QString, localPath), Q_ARG(QString, remoteFileName),
                                   Q_ARG(qint64, 0));
    });
    QObject::connect(backend, &RemoteBackend::transferFinished, qApp, [&](const QString &) {
        uploadOk = true;
        elapsedMs = timer.elapsed();
        qApp->quit();
    });
    QObject::connect(backend, &RemoteBackend::transferFailed, qApp, [&](const QString &, const QString &reason) {
        failureReason = "transfer failed: " + reason;
        qApp->quit();
    });

    thread->start();
    autoAcceptHostKeyPrompt();
    QMetaObject::invokeMethod(backend, "connectToHost", Qt::QueuedConnection);

    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, qApp, [&]() {
        failureReason = "timed out after 120s";
        qApp->quit();
    });
    timeoutTimer.start(120000);

    qApp->exec();

    check(qPrintable(QStringLiteral("connected for bandwidthLimitKBps=%1").arg(bandwidthLimitKBps)), connected);
    check(qPrintable(QStringLiteral("upload completed for bandwidthLimitKBps=%1").arg(bandwidthLimitKBps)), uploadOk);
    if (!failureReason.isEmpty())
        fprintf(stderr, "failure reason: %s\n", qPrintable(failureReason));

    backend->deleteLater();
    thread->quit();
    thread->wait();
    delete thread;

    if (!uploadOk || elapsedMs <= 0)
        return 0.0;
    return (static_cast<double>(fileSizeBytes) / 1024.0) / (static_cast<double>(elapsedMs) / 1000.0);
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    const QString scratch = qEnvironmentVariable("SFTP_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/sftp");
    const int port = qEnvironmentVariable("SFTP_TEST_PORT", "2222").toInt();
    const QString serverRoot = scratch + "/root";

    // Deliberately modest (2MB default) — large enough to run for a real,
    // measurable stretch of wall-clock time even at a low throttle
    // setting, small enough this harness doesn't take minutes to finish.
    const qint64 fileSizeBytes = qEnvironmentVariable("VERIFY_THROTTLE_FILE_SIZE_KB", "2000").toLongLong() * 1024;
    const int throttleLimitKBps = qEnvironmentVariable("VERIFY_THROTTLE_LIMIT_KBPS", "300").toInt();

    const QString localPath = QDir::tempPath() + "/zephyrftp_throttle_verify_upload.bin";
    fprintf(stderr, "[setup] generating %lld KB local fixture...\n", fileSizeBytes / 1024);
    if (!generateTestFile(localPath, fileSizeBytes)) {
        fprintf(stderr, "FAIL: could not generate local test file\n");
        return 1;
    }
    const QString remoteUnthrottled = "throttle_verify_unthrottled.bin";
    const QString remoteThrottled = "throttle_verify_throttled.bin";
    // Idempotency across repeated runs — same reasoning
    // verify_remote_to_remote_live.cpp's own header comment documents.
    QFile::remove(serverRoot + "/" + remoteUnthrottled);
    QFile::remove(serverRoot + "/" + remoteThrottled);

    auto *hostKeyVerifier = new HostKeyVerifier(&app);

    fprintf(stderr, "[run 1/2] uploading unthrottled (bandwidthLimitKBps=0)...\n");
    const double unthrottledKBps = runUploadAndMeasureKBps(
        hostKeyVerifier, scratch, port, localPath, fileSizeBytes, 0, remoteUnthrottled);
    fprintf(stderr, "[run 1/2] measured %.1f KB/s unthrottled\n", unthrottledKBps);

    fprintf(stderr, "[run 2/2] uploading throttled to %d KB/s...\n", throttleLimitKBps);
    const double throttledKBps = runUploadAndMeasureKBps(
        hostKeyVerifier, scratch, port, localPath, fileSizeBytes, throttleLimitKBps, remoteThrottled);
    fprintf(stderr, "[run 2/2] measured %.1f KB/s throttled (configured limit: %d KB/s)\n",
            throttledKBps, throttleLimitKBps);

    // Real headroom, not a coincidence: the unthrottled rate should be
    // meaningfully faster than the configured throttle limit itself
    // (otherwise this loopback link is too slow for this comparison to
    // mean anything), and the throttled rate should land close to that
    // configured limit rather than anywhere near the unthrottled rate.
    check("unthrottled upload achieved a real rate meaningfully above the configured throttle limit "
          "(confirms this link is fast enough for the comparison below to be meaningful)",
          unthrottledKBps > throttleLimitKBps * 2.0);
    check("throttled upload landed close to the configured limit (0.6x-1.3x), not the unthrottled rate",
          throttledKBps >= throttleLimitKBps * 0.6 && throttledKBps <= throttleLimitKBps * 1.3);
    check("throttled upload was meaningfully slower than the unthrottled run "
          "(proves the limit genuinely constrained real network I/O)",
          throttledKBps < unthrottledKBps * 0.7);

    auto fileHash = [](const QString &path) -> QByteArray {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return {};
        return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
    };
    const QByteArray localHash = fileHash(localPath);
    check("unthrottled upload's content matches the local original byte-for-byte",
          !localHash.isEmpty() && localHash == fileHash(serverRoot + "/" + remoteUnthrottled));
    check("throttled upload's content matches the local original byte-for-byte",
          !localHash.isEmpty() && localHash == fileHash(serverRoot + "/" + remoteThrottled));

    QFile::remove(localPath);

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
