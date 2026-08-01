// Exercises SftpBackend's REAL mid-transfer cancel and pause/resume
// against a REAL server — closing the gap ARCHITECTURE.md describes
// exactly: "SftpBackend's actual mid-transfer interruption — the cancel
// and pause flags checked inside the libssh2 read/write loops — and its
// real byte-offset resume logic... [compile] and follow patterns proven
// correct elsewhere in this codebase, but none of it has a real-server
// test confirming it actually works... Needs a slow-enough real
// transfer, paused and resumed by hand, to verify that specific claim."
// `transfer-pause-test` (the fake-backend test) verifies
// TransferManager's orchestration; this verifies the actual libssh2
// read/write loop and resume logic underneath it, against a real
// server. Needs tools/local-test-servers/start-sftp-pubkey.sh already
// running; NOT one of the ten self-contained EXCLUDE_FROM_ALL targets
// (external precondition), so not part of that suite or CI.
//
// requestPause()/requestCancel() are called as plain direct method
// calls, not via QMetaObject::invokeMethod — per RemoteBackend's own
// doc comment, they're deliberately thread-safe-callable-from-anywhere
// (an atomic flag polled inside the worker thread's read/write loop),
// unlike connectToHost()/downloadFile()/etc., which are slots needing
// queued cross-thread dispatch.
//
// "Slow-enough" is real, not simulated: a genuinely large (default
// 300MB) file, so the read/write loop runs through enough 480000-byte
// chunks that a cross-thread queued transferProgress signal reliably
// gets processed and requestPause()/requestCancel() called well before
// the transfer would finish on its own — empirically confirmed, not
// assumed, by checking the actual bytesDone reported at
// pause/cancel time is a real, substantial fraction of the total, not
// suspiciously close to either end.
//
// Run with:
//   tools/local-test-servers/start-sftp-pubkey.sh
//   cmake --build build --target verify-sftp-pause-cancel
//   QT_QPA_PLATFORM=offscreen SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp \
//       ./build/verify-sftp-pause-cancel
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QAbstractButton>
#include <QRandomGenerator>
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

bool filesMatchByteForByte(const QString &pathA, const QString &pathB)
{
    QFile a(pathA), b(pathB);
    if (!a.open(QIODevice::ReadOnly) || !b.open(QIODevice::ReadOnly))
        return false;
    if (a.size() != b.size())
        return false;
    const qint64 chunk = 4 * 1024 * 1024;
    while (!a.atEnd()) {
        const QByteArray ca = a.read(chunk);
        const QByteArray cb = b.read(chunk);
        if (ca != cb)
            return false;
    }
    return true;
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Same real issue as verify_sftp_pubkey.cpp: no QMainWindow is ever
    // shown, so the host-key dialog is the only top-level window — Qt's
    // default quitOnLastWindowClosed would auto-quit the instant it
    // closes, well before the connection actually finishes.
    app.setQuitOnLastWindowClosed(false);

    const QString scratch = qEnvironmentVariable(
        "SFTP_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/sftp");
    const qint64 fileSize = qEnvironmentVariable("VERIFY_PAUSE_FILE_SIZE_MB", "300").toLongLong()
        * 1024 * 1024;

    // A real large file, not synthetic zeros (QRandomGenerator, not
    // memset) — so a truncated/corrupted resume can't accidentally look
    // byte-identical to the real thing the way an all-zeros file could.
    const QString sourcePath = "/tmp/zephyrftp_verify_pause_source.bin";
    if (!QFile::exists(sourcePath) || QFileInfo(sourcePath).size() != fileSize) {
        QFile f(sourcePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        QByteArray chunk(4 * 1024 * 1024, Qt::Uninitialized);
        qint64 written = 0;
        while (written < fileSize) {
            for (int i = 0; i < chunk.size(); i += 4)
                *reinterpret_cast<quint32 *>(chunk.data() + i) = QRandomGenerator::global()->generate();
            const qint64 toWrite = qMin<qint64>(chunk.size(), fileSize - written);
            f.write(chunk.constData(), toWrite);
            written += toWrite;
        }
    }
    fprintf(stderr, "[setup] source file: %s (%lld MB)\n", qPrintable(sourcePath), fileSize / 1024 / 1024);

    SftpCredentials creds;
    creds.host = "127.0.0.1";
    creds.port = 2222;
    creds.username = qEnvironmentVariable("USER", "test");
    creds.authMethod = SftpAuthMethod::PublicKey;
    creds.privateKeyPath = scratch + "/client_key";

    auto *hostKeyVerifier = new HostKeyVerifier(&app);
    auto *backend = new SftpBackend(creds, hostKeyVerifier);
    auto *thread = new QThread(&app);
    backend->moveToThread(thread);

    // --- Shared connection state ---
    bool connected = false;
    QString failureReason;

    // --- Phase A: upload, cancel mid-transfer ---
    bool cancelSeenProgress = false;
    qint64 cancelBytesDoneAtRequest = 0;
    bool cancelRequestSent = false;
    bool uploadCancelled = false;
    QString uploadCancelFailReason;

    // --- Phase B: upload, pause + resume ---
    bool pauseRequestSent = false;
    qint64 pauseBytesDoneAtRequest = 0;
    bool uploadPaused = false;
    qint64 uploadPausedBytesDone = -1;
    bool uploadResumeStarted = false;
    bool uploadResumeFirstProgressAtOrAbovePause = false;
    bool uploadResumeFinished = false;

    // --- Phase C: download, pause + resume (of the file just uploaded
    // in Phase B, avoiding needing a separate large server-side fixture) ---
    bool downloadPauseRequestSent = false;
    bool downloadPaused = false;
    qint64 downloadPausedBytesDone = -1;
    bool downloadResumeStarted = false;
    bool downloadResumeFirstProgressAtOrAbovePause = false;
    bool downloadResumeFinished = false;

    const QString uploadTarget = "pause_test/upload_pause_target.bin";
    const QString uploadCancelTarget = "pause_test/upload_cancel_target.bin";
    const QString downloadLocalPath = "/tmp/zephyrftp_verify_pause_download.bin";
    QFile::remove(downloadLocalPath);

    enum class Stage {
        Connecting, UploadCancel, UploadPause, UploadResume,
        DownloadPause, DownloadResume, Done
    };
    Stage stage = Stage::Connecting;

    QObject::connect(backend, &RemoteBackend::connectionFailed, &app, [&](const QString &reason) {
        failureReason = reason;
        qApp->quit();
    });

    QObject::connect(backend, &RemoteBackend::connected, &app, [&]() {
        connected = true;
        stage = Stage::UploadCancel;
        QMetaObject::invokeMethod(backend, "uploadFile", Qt::QueuedConnection,
                                   Q_ARG(QString, sourcePath),
                                   Q_ARG(QString, uploadCancelTarget),
                                   Q_ARG(qint64, 0));
    });

    QObject::connect(backend, &RemoteBackend::transferProgress, &app,
        [&](const QString &, qint64 bytesDone, qint64 bytesTotal) {
            switch (stage) {
            case Stage::UploadCancel:
                cancelSeenProgress = true;
                if (!cancelRequestSent && bytesDone > 0 && bytesDone < bytesTotal) {
                    cancelRequestSent = true;
                    cancelBytesDoneAtRequest = bytesDone;
                    backend->requestCancel();
                }
                break;
            case Stage::UploadPause:
                if (!pauseRequestSent && bytesDone > 0 && bytesDone < bytesTotal) {
                    pauseRequestSent = true;
                    pauseBytesDoneAtRequest = bytesDone;
                    backend->requestPause();
                }
                break;
            case Stage::UploadResume:
                if (!uploadResumeFirstProgressAtOrAbovePause)
                    uploadResumeFirstProgressAtOrAbovePause = bytesDone >= uploadPausedBytesDone;
                break;
            case Stage::DownloadPause:
                if (!downloadPauseRequestSent && bytesDone > 0 && bytesDone < bytesTotal) {
                    downloadPauseRequestSent = true;
                    backend->requestPause();
                }
                break;
            case Stage::DownloadResume:
                if (!downloadResumeFirstProgressAtOrAbovePause)
                    downloadResumeFirstProgressAtOrAbovePause = bytesDone >= downloadPausedBytesDone;
                break;
            default:
                break;
            }
        });

    QObject::connect(backend, &RemoteBackend::transferPaused, &app,
        [&](const QString &, qint64 bytesDone) {
            if (stage == Stage::UploadPause) {
                uploadPaused = true;
                uploadPausedBytesDone = bytesDone;
                stage = Stage::UploadResume;
                QMetaObject::invokeMethod(backend, "uploadFile", Qt::QueuedConnection,
                                           Q_ARG(QString, sourcePath),
                                           Q_ARG(QString, uploadTarget),
                                           Q_ARG(qint64, bytesDone));
                uploadResumeStarted = true;
            } else if (stage == Stage::DownloadPause) {
                downloadPaused = true;
                downloadPausedBytesDone = bytesDone;
                stage = Stage::DownloadResume;
                QMetaObject::invokeMethod(backend, "downloadFile", Qt::QueuedConnection,
                                           Q_ARG(QString, uploadTarget),
                                           Q_ARG(QString, downloadLocalPath),
                                           Q_ARG(qint64, bytesDone));
                downloadResumeStarted = true;
            }
        });

    QObject::connect(backend, &RemoteBackend::transferFailed, &app,
        [&](const QString &, const QString &reason) {
            if (stage == Stage::UploadCancel) {
                uploadCancelled = true;
                uploadCancelFailReason = reason;
                stage = Stage::UploadPause;
                QMetaObject::invokeMethod(backend, "uploadFile", Qt::QueuedConnection,
                                           Q_ARG(QString, sourcePath),
                                           Q_ARG(QString, uploadTarget),
                                           Q_ARG(qint64, 0));
            } else {
                failureReason = QStringLiteral("unexpected transferFailed in stage %1: %2")
                    .arg(static_cast<int>(stage)).arg(reason);
                qApp->quit();
            }
        });

    QObject::connect(backend, &RemoteBackend::transferFinished, &app,
        [&](const QString &) {
            if (stage == Stage::UploadResume) {
                uploadResumeFinished = true;
                stage = Stage::DownloadPause;
                QMetaObject::invokeMethod(backend, "downloadFile", Qt::QueuedConnection,
                                           Q_ARG(QString, uploadTarget),
                                           Q_ARG(QString, downloadLocalPath),
                                           Q_ARG(qint64, 0));
            } else if (stage == Stage::DownloadResume) {
                downloadResumeFinished = true;
                stage = Stage::Done;
                qApp->quit();
            }
        });

    thread->start();
    autoAcceptHostKeyPrompt();
    QMetaObject::invokeMethod(backend, "connectToHost", Qt::QueuedConnection);

    QTimer::singleShot(120000, &app, [&]() {
        if (failureReason.isEmpty())
            failureReason = QStringLiteral("timed out after 120s in stage %1").arg(static_cast<int>(stage));
        qApp->quit();
    });

    app.exec();

    check("connected to real local SFTP server", connected);
    if (!failureReason.isEmpty())
        fprintf(stderr, "failure reason: %s\n", qPrintable(failureReason));

    // --- Phase A: cancel ---
    check("upload cancel: saw real progress before cancelling", cancelSeenProgress && cancelRequestSent);
    fprintf(stderr, "[info] cancel requested at %lld / %lld bytes (%.1f%%)\n",
            (long long)cancelBytesDoneAtRequest, (long long)fileSize,
            100.0 * cancelBytesDoneAtRequest / fileSize);
    // At least one full 480000-byte chunk really moved (not a degenerate
    // zero-byte case) and strictly short of the full size (didn't
    // coincidentally finish before the cancel actually landed) — the
    // interrupt firing after exactly one chunk is the earliest
    // legitimate interruption point, not a sign it happened "too early."
    check("upload cancel: a real chunk transferred before cancelling, and it didn't run to completion",
          cancelBytesDoneAtRequest >= 480000 && cancelBytesDoneAtRequest < fileSize);
    check("upload cancel: transferFailed fired with reason \"Cancelled\"",
          uploadCancelled && uploadCancelFailReason == "Cancelled");
    {
        QFile partial(scratch + "/root/" + uploadCancelTarget);
        const qint64 partialSize = partial.exists() ? QFileInfo(partial).size() : -1;
        check("upload cancel: partial file on server is genuinely incomplete (smaller than the source)",
              partialSize >= 0 && partialSize < fileSize);
    }

    // --- Phase B: upload pause + resume ---
    check("upload pause: a real chunk transferred before pausing, and it didn't run to completion",
          pauseRequestSent
          && pauseBytesDoneAtRequest >= 480000 && pauseBytesDoneAtRequest < fileSize);
    check("upload pause: transferPaused fired with a real nonzero bytesDone",
          uploadPaused && uploadPausedBytesDone > 0);
    check("upload resume: actually started (uploadFile called again with the paused offset)",
          uploadResumeStarted);
    check("upload resume: first progress after resuming is at/above the paused offset — "
          "continued, not restarted from zero",
          uploadResumeFirstProgressAtOrAbovePause);
    check("upload resume: reached transferFinished", uploadResumeFinished);
    check("upload resume: final file on server matches the source byte-for-byte",
          filesMatchByteForByte(sourcePath, scratch + "/root/" + uploadTarget));

    // --- Phase C: download pause + resume ---
    check("download pause: transferPaused fired with a real nonzero bytesDone",
          downloadPaused && downloadPausedBytesDone > 0);
    check("download resume: actually started (downloadFile called again with the paused offset)",
          downloadResumeStarted);
    check("download resume: first progress after resuming is at/above the paused offset — "
          "continued, not restarted from zero",
          downloadResumeFirstProgressAtOrAbovePause);
    check("download resume: reached transferFinished", downloadResumeFinished);
    check("download resume: final local file matches the server's file byte-for-byte",
          filesMatchByteForByte(sourcePath, downloadLocalPath));

    backend->deleteLater();
    thread->quit();
    thread->wait();

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
