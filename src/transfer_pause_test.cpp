// Tests TransferManager's pause/resume ORCHESTRATION — status transitions,
// bytesDone preservation across a pause, and that resumeItem() actually
// causes the next run to start from the paused offset rather than zero.
//
// Does NOT test SftpBackend's real byte-offset resume logic (the seek64
// calls, the local-file clamping/trimming) — that needs a live SFTP
// server and is flagged as such in ARCHITECTURE.md's Known gaps,
// consistent with how SftpBackend's cancel-mid-transfer is already
// flagged unverified there. What CAN be verified without a server is
// whether TransferManager itself correctly orchestrates pause and
// resume — that's what this tests, using a fake backend built for
// exactly this purpose (a legitimate test-double technique: TransferManager
// only ever talks to the RemoteBackend interface, so a fake that honors
// that interface's contract exercises the same code paths a real one would).
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"
#include "backends/RemoteBackend.h"
#include "transfer/TransferManager.h"

namespace {

// Simulates an interruptible async transfer via a QTimer ticking in fixed
// chunks — genuinely asynchronous (not a single synchronous loop), so
// there's a real window between ticks for the test to call
// pauseItem()/resumeItem(), same as there would be with a real backend
// on a real network.
class FakePausableBackend : public RemoteBackend {
    Q_OBJECT
public:
    explicit FakePausableBackend(QObject *parent = nullptr) : RemoteBackend(parent) {
        connect(&m_timer, &QTimer::timeout, this, &FakePausableBackend::tick);
    }

    QString currentPath() const override { return QStringLiteral("/fake"); }
    bool isLocalFilesystem() const override { return false; }   // pretend remote, so pause is meaningful
    void requestCancel() override { m_cancelRequested = true; }
    void requestPause() override { m_pauseRequested = true; }

public slots:
    void connectToHost() override { emit connected(); }
    void listDirectory(const QString &path) override { emit directoryListed(path, {}); }

    void downloadFile(const QString &remotePath, const QString &localPath, qint64 resumeOffset = 0) override {
        Q_UNUSED(localPath);
        start(remotePath, resumeOffset);
    }
    void uploadFile(const QString &localPath, const QString &remotePath, qint64 resumeOffset = 0) override {
        Q_UNUSED(remotePath);
        start(localPath, resumeOffset);
    }

    // Not exercised by this test — it's about pause/resume orchestration,
    // not file management — but RemoteBackend is a pure interface, so a
    // concrete fake needs some implementation to be instantiable at all.
    void deleteEntry(const QString &, bool) override {}
    void renameEntry(const QString &, const QString &) override {}
    void createDirectory(const QString &) override {}
    void createFile(const QString &) override {}
    void listDirectoryForEnumeration(const QString &, int) override {}

private slots:
    void tick() {
        if (m_cancelRequested) {
            m_timer.stop();
            emit transferFailed(m_name, QStringLiteral("Cancelled"));
            return;
        }
        if (m_pauseRequested) {
            m_timer.stop();
            emit transferPaused(m_name, m_done);
            return;
        }
        m_done = qMin(m_done + 100, m_total);
        emit transferProgress(m_name, m_done, m_total);
        if (m_done >= m_total) {
            m_timer.stop();
            emit transferFinished(m_name);
        }
    }

private:
    void start(const QString &name, qint64 resumeOffset) {
        m_cancelRequested = false;
        m_pauseRequested = false;
        m_name = name;
        m_done = resumeOffset;
        m_total = 1000;
        m_timer.start(30);   // ~30ms per 100-byte chunk — fast enough for a test, slow enough to interrupt
    }

    QTimer m_timer;
    QString m_name;
    qint64 m_done = 0;
    qint64 m_total = 0;
    bool m_cancelRequested = false;
    bool m_pauseRequested = false;
};

}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    auto *leftPane = new FilePaneWidget(new LocalBackend());
    auto *rightPane = new FilePaneWidget(new FakePausableBackend());
    auto *manager = new TransferManager(&app);

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    int itemId = -1;
    qint64 bytesDoneAtPause = -1;
    qint64 firstBytesDoneAfterResume = -1;
    bool sawResumeInProgress = false;

    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        itemId = item.id;
    });

    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.id != itemId)
            return;
        qDebug() << "[test] itemUpdated: status =" << static_cast<int>(item.status)
                 << "bytesDone =" << item.bytesDone;

        if (item.status == TransferStatus::Paused && bytesDoneAtPause < 0) {
            bytesDoneAtPause = item.bytesDone;
        }
        if (item.status == TransferStatus::InProgress && bytesDoneAtPause >= 0 && !sawResumeInProgress) {
            // First InProgress update after a resume — capture it once.
            firstBytesDoneAfterResume = item.bytesDone;
            sawResumeInProgress = true;
        }
    });

    // Step 1: start the (fake) transfer.
    QTimer::singleShot(100, &app, [&]() {
        manager->enqueue(leftPane, rightPane, "fakefile.bin");
    });

    // Step 2: pause it partway through (a few ticks in — enough progress
    // to have a meaningful, nonzero bytesDone, not so long it finishes
    // first).
    QTimer::singleShot(250, &app, [&]() {
        check("item exists before pausing", itemId > 0);
        manager->pauseItem(itemId);
    });

    // Step 3: confirm it actually paused with real partial progress.
    QTimer::singleShot(500, &app, [&]() {
        check("paused with nonzero bytesDone", bytesDoneAtPause > 0 && bytesDoneAtPause < 1000);

        bool foundPaused = false;
        for (const TransferItem &it : manager->items()) {
            if (it.id == itemId && it.status == TransferStatus::Paused)
                foundPaused = true;
        }
        check("item status is genuinely Paused (not just a local variable)", foundPaused);

        // Resume it.
        manager->resumeItem(itemId);
    });

    // Step 4: confirm resume started from the paused offset, not zero —
    // the whole point of pause/resume rather than cancel/retry.
    QTimer::singleShot(700, &app, [&]() {
        check("resumed and made progress", sawResumeInProgress);
        check("resume continued from the paused offset rather than restarting from zero",
              firstBytesDoneAfterResume >= bytesDoneAtPause);
        qDebug() << "[test] bytesDoneAtPause =" << bytesDoneAtPause
                 << "firstBytesDoneAfterResume =" << firstBytesDoneAfterResume;
    });

    // Step 5: let it run to completion.
    QTimer::singleShot(1500, &app, [&]() {
        bool foundDone = false;
        qint64 finalBytesDone = -1;
        for (const TransferItem &it : manager->items()) {
            if (it.id == itemId) {
                foundDone = (it.status == TransferStatus::Done);
                finalBytesDone = it.bytesDone;
            }
        }
        check("transfer completed to Done after resuming", foundDone);
        check("final bytesDone reached the full total", finalBytesDone == 1000);

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}

#include "transfer_pause_test.moc"
