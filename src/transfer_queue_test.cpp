// Headless functional test of the transfer queue. Not a unit test of one
// class in isolation — this exercises the real path: two FilePaneWidgets
// each backed by a real LocalBackend, a real TransferManager, a real file
// on disk. Verifies the file actually moves, its contents are unchanged,
// and the queue reports Queued -> InProgress -> Done with plausible byte
// counts along the way.
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

    auto *leftPane = new FilePaneWidget(new LocalBackend());
    auto *rightPane = new FilePaneWidget(new LocalBackend());
    auto *manager = new TransferManager(&app);

    bool sawQueued = false, sawInProgress = false, sawDone = false;
    bool sawPlausibleProgress = false;

    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        qDebug() << "[test] itemAdded: status =" << static_cast<int>(item.status)
                 << "src =" << item.sourcePath << "dst =" << item.destPath;
        if (item.status == TransferStatus::Queued)
            sawQueued = true;
    });

    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        qDebug() << "[test] itemUpdated: status =" << static_cast<int>(item.status)
                 << "bytesDone =" << item.bytesDone << "bytesTotal =" << item.bytesTotal;
        if (item.status == TransferStatus::InProgress) {
            sawInProgress = true;
            if (item.bytesTotal == 500000 && item.bytesDone <= item.bytesTotal)
                sawPlausibleProgress = true;
        }
        if (item.status == TransferStatus::Done)
            sawDone = true;
    });

    // Navigate both panes to their directories, then queue the transfer
    // once both have actually reported their listings back (LocalBackend
    // is queued-invoked too, so this isn't synchronous even though it's
    // all on one thread).
    leftPane->navigateTo(srcDir);
    rightPane->navigateTo(dstDir);

    QTimer::singleShot(300, &app, [&]() {
        manager->enqueue(leftPane, rightPane, fileName);
    });

    QTimer::singleShot(1500, &app, [&]() {
        const QString dstPath = dstDir + "/" + fileName;
        const bool fileExists = QFile::exists(dstPath);
        const QString dstHash = fileExists ? fileHash(dstPath) : QString();
        const bool hashMatches = fileExists && (dstHash == srcHashBefore);

        qDebug() << "[test] dest file exists:" << fileExists;
        qDebug() << "[test] hash matches source:" << hashMatches;
        qDebug() << "[test] saw Queued->InProgress->Done:" << sawQueued << sawInProgress << sawDone;
        qDebug() << "[test] saw plausible progress numbers:" << sawPlausibleProgress;

        const bool pass = fileExists && hashMatches && sawQueued && sawInProgress && sawDone && sawPlausibleProgress;
        qDebug() << (pass ? "[test] PASS" : "[test] FAIL");
        app.exit(pass ? 0 : 1);
    });

    return app.exec();
}
