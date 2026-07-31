// Headless functional test of the transfer queue. Not a unit test of one
// class in isolation — this exercises the real path: two FilePaneWidgets
// each backed by a real LocalBackend, a real TransferManager, real files
// on disk.
//
// Three phases:
//   1. A real transfer completes: file moves, hash matches, Queued ->
//      InProgress -> Done with plausible progress numbers.
//   2. Cancelling a QUEUED (not yet started) item. Exploits a real,
//      deterministic property of TransferManager rather than racing a
//      timer: enqueue() marks the new item InProgress synchronously if
//      nothing else is running, or leaves it Queued (also synchronously)
//      if something already is — so two enqueue() calls in a row, with
//      nothing else running between them, reliably produce one InProgress
//      item and one Queued item with no timing window to miss.
//   3. Retrying a FAILED item actually re-runs the transfer (proven by
//      pointing at a nonexistent source file, so both the original attempt
//      and the retry fail deterministically — this tests that retryItem()
//      genuinely re-triggers execution, not just resets a status field).
//
// UNVERIFIED — flagged, not silently skipped: this only exercises
// LocalBackend's requestCancel(), which is a documented no-op (QFile::copy
// can't be interrupted mid-call). SftpBackend's actual mid-transfer
// interruption (the cancel flag checked inside the libssh2 read/write
// loops) has no automated coverage — it needs a real SFTP server slow
// enough to catch mid-transfer, which isn't available here.
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QFile>
#include <QCryptographicHash>
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"
#include "transfer/TransferManager.h"

namespace {
QString fileHash(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Md5).toHex();
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const QString srcDir = "/tmp/transfer_test/src_dir";
    const QString dstDir = "/tmp/transfer_test/dst_dir";
    const QString fileName = "testfile.bin";
    const QString srcHashBefore = fileHash(srcDir + "/" + fileName);

    // Second small file for the cancel-while-queued phase.
    {
        QFile f(srcDir + "/testfile2.bin");
        f.open(QIODevice::WriteOnly);
        f.write(QByteArray(1000, 'x'));
    }

    auto *leftPane = new FilePaneWidget(new LocalBackend());
    auto *rightPane = new FilePaneWidget(new LocalBackend());
    auto *manager = new TransferManager(&app);

    // ---- Phase 1 tracking ----
    bool sawQueued = false, sawInProgress = false, sawDone = false;
    bool sawPlausibleProgress = false;
    bool phase1Pass = false;

    // ---- Phase 2 tracking ----
    int cancelTargetId = -1;
    bool phase2SyncPass = false;
    bool phase2Pass = false;

    // ---- Phase 3 tracking ----
    int retryTargetId = -1;
    int retryFailedCount = 0;
    bool sawRetryRequeued = false;
    bool phase3Pass = false;

    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        qDebug() << "[test] itemAdded: status =" << static_cast<int>(item.status)
                 << "src =" << item.sourcePath << "dst =" << item.destPath;
        if (item.status == TransferStatus::Queued)
            sawQueued = true;
        if (item.fileName == "testfile2.bin")
            cancelTargetId = item.id;
        if (item.fileName == "doesnotexist.bin")
            retryTargetId = item.id;
    });

    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        qDebug() << "[test] itemUpdated: id =" << item.id << "status =" << static_cast<int>(item.status)
                 << "bytesDone =" << item.bytesDone << "bytesTotal =" << item.bytesTotal;
        if (item.status == TransferStatus::InProgress) {
            sawInProgress = true;
            if (item.bytesTotal == 500000 && item.bytesDone <= item.bytesTotal)
                sawPlausibleProgress = true;
        }
        if (item.status == TransferStatus::Done)
            sawDone = true;

        if (item.id == retryTargetId) {
            if (item.status == TransferStatus::Failed)
                retryFailedCount++;
            if (item.status == TransferStatus::Queued && retryFailedCount == 1)
                sawRetryRequeued = true;   // the re-queue specifically happened AFTER the first failure
        }
    });

    leftPane->navigateTo(srcDir);
    rightPane->navigateTo(dstDir);

    // ---- Phase 1: real transfer ----
    QTimer::singleShot(300, &app, [&]() {
        manager->enqueue(leftPane, rightPane, fileName);
    });

    QTimer::singleShot(1200, &app, [&]() {
        const QString dstPath = dstDir + "/" + fileName;
        const bool fileExists = QFile::exists(dstPath);
        const QString dstHash = fileExists ? fileHash(dstPath) : QString();
        const bool hashMatches = fileExists && (dstHash == srcHashBefore);

        qDebug() << "[phase1] dest file exists:" << fileExists;
        qDebug() << "[phase1] hash matches source:" << hashMatches;
        qDebug() << "[phase1] saw Queued->InProgress->Done:" << sawQueued << sawInProgress << sawDone;
        qDebug() << "[phase1] saw plausible progress numbers:" << sawPlausibleProgress;

        const bool pass = fileExists && hashMatches && sawQueued && sawInProgress && sawDone && sawPlausibleProgress;
        phase1Pass = pass;
        qDebug() << (pass ? "[phase1] PASS" : "[phase1] FAIL");
    });

    // ---- Phase 2: cancel a queued (not-yet-started) item ----
    QTimer::singleShot(1400, &app, [&]() {
        QFile::remove(dstDir + "/testfile2.bin");   // clean slate in case a prior run left it
        // testfile.bin already exists at this destination from phase 1 —
        // remove it first so re-queueing the same transfer doesn't hit
        // the (correct, intentional) new destination-conflict prompt
        // this test isn't set up to handle. Conflict resolution has its
        // own dedicated coverage; this phase is specifically about
        // cancelling a queued item, not about overwrite behavior.
        QFile::remove(dstDir + "/testfile.bin");

        // Back-to-back, synchronous, no event-loop turn between them: the
        // first becomes InProgress immediately (nothing else running),
        // the second is left Queued immediately (guard in startNext()).
        manager->enqueue(leftPane, rightPane, "testfile.bin");
        manager->enqueue(leftPane, rightPane, "testfile2.bin");

        bool cancelledSynchronously = false;
        manager->cancelItem(cancelTargetId);
        for (const TransferItem &it : manager->items()) {
            if (it.id == cancelTargetId && it.status == TransferStatus::Cancelled)
                cancelledSynchronously = true;
        }
        qDebug() << "[phase2] queued item cancelled synchronously, before any event loop turn:"
                 << cancelledSynchronously;
        phase2SyncPass = cancelledSynchronously;
        qDebug() << (cancelledSynchronously ? "[phase2-sync] PASS" : "[phase2-sync] FAIL");
    });

    QTimer::singleShot(2200, &app, [&]() {
        const bool neverCopied = !QFile::exists(dstDir + "/testfile2.bin");
        bool finalStatusStillCancelled = false;
        for (const TransferItem &it : manager->items()) {
            if (it.id == cancelTargetId && it.status == TransferStatus::Cancelled)
                finalStatusStillCancelled = true;
        }
        qDebug() << "[phase2] testfile2.bin never actually copied:" << neverCopied;
        qDebug() << "[phase2] status still Cancelled (wasn't silently resumed):" << finalStatusStillCancelled;
        const bool pass = neverCopied && finalStatusStillCancelled;
        phase2Pass = pass;
        qDebug() << (pass ? "[phase2] PASS" : "[phase2] FAIL");
    });

    // ---- Phase 3: retry a failed item ----
    QTimer::singleShot(2400, &app, [&]() {
        manager->enqueue(leftPane, rightPane, "doesnotexist.bin");   // guaranteed to fail — source doesn't exist
    });

    QTimer::singleShot(3000, &app, [&]() {
        qDebug() << "[phase3] failed once before retry:" << (retryFailedCount == 1);
        manager->retryItem(retryTargetId);
    });

    QTimer::singleShot(3600, &app, [&]() {
        qDebug() << "[phase3] was actually re-queued after the first failure:" << sawRetryRequeued;
        qDebug() << "[phase3] failed again after retry (proves retry re-ran, not just reset a flag):"
                 << (retryFailedCount == 2);
        const bool pass = sawRetryRequeued && (retryFailedCount == 2);
        phase3Pass = pass;
        qDebug() << (pass ? "[phase3] PASS" : "[phase3] FAIL");

        const bool overallPass = phase1Pass && phase2SyncPass && phase2Pass && phase3Pass;
        qDebug() << (overallPass ? "[test] ALL PHASES PASS" : "[test] AT LEAST ONE PHASE FAILED");
        app.exit(overallPass ? 0 : 1);
    });

    return app.exec();
}
