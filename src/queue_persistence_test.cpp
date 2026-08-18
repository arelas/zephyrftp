// Tests transfer queue persistence end to end: TransferQueueStore's
// save/load round trip, TransferManager::restorePersistedQueue()'s
// LocalToLocal-dispatches-immediately vs. LocalToRemote/RemoteToLocal-
// becomes-PendingReconnect split, and tryReclaimPendingItems()'s
// reconnect-matching (a non-matching reconnect leaving an item
// untouched, one reconnect claiming several items that share the same
// connection), plus saveQueueForShutdown()'s exclusion of terminal/Move
// items and a raw-JSON check that no secret-shaped key is ever written.
//
// Uses a small FakeRemoteBackend (not LocalBackend) for the "remote"
// side of the RemoteToLocal scenarios specifically because
// LocalBackend::downloadFile() ignores resumeOffset entirely (a real
// local copy is always a fresh QFile::copy(), never a partial resume) —
// this test needs to confirm the persisted bytesDone genuinely reaches
// downloadFile() as the resume offset, which only a backend that
// actually records it can prove. Same minimal-fake-backend technique
// remote_to_remote_test.cpp's own FakeRemoteBackend already establishes.
//
// Run with:
//   QT_QPA_PLATFORM=offscreen ./build/queue-persistence-test
#include <QApplication>
#include <QStandardPaths>
#include <QTimer>
#include <QEventLoop>
#include <QDebug>
#include <QDir>
#include <QFile>
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"
#include "backends/RemoteBackend.h"
#include "backends/SavedSite.h"
#include "transfer/TransferManager.h"
#include "transfer/TransferQueueStore.h"

namespace {
class FakeRemoteBackend : public RemoteBackend {
    Q_OBJECT
public:
    explicit FakeRemoteBackend(QObject *parent = nullptr) : RemoteBackend(parent) {}

    QString currentPath() const override { return QStringLiteral("/remote"); }
    bool isLocalFilesystem() const override { return false; }
    QString connectionIdentity() const override { return QStringLiteral("fake"); }
    void requestCancel() override {}
    void requestPause() override {}

    // Test hook -- every downloadFile() call's resumeOffset, in dispatch
    // order (a QList, not a single scalar: TransferManager is serial, so
    // more than one item can dispatch against the same fake backend
    // instance over the test's lifetime, and a single "last" field would
    // just get overwritten by the second call before the test ever
    // checks the first's value).
    QList<qint64> downloadResumeOffsets;

public slots:
    void connectToHost() override { emit connected(); }
    void listDirectory(const QString &path) override { emit directoryListed(path, {}); }
    void downloadFile(const QString &remotePath, const QString &localPath, qint64 resumeOffset = 0) override {
        downloadResumeOffsets.append(resumeOffset);
        QFile f(localPath);
        f.open(resumeOffset > 0 ? QIODevice::Append : (QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("remote-content");
        f.close();
        emit transferProgress(remotePath, 100, 100);
        emit transferFinished(remotePath);
    }
    void uploadFile(const QString &localPath, const QString &remotePath, qint64 resumeOffset = 0) override {
        Q_UNUSED(localPath);
        Q_UNUSED(resumeOffset);
        emit transferProgress(remotePath, 100, 100);
        emit transferFinished(remotePath);
    }
    void deleteEntry(const QString &, bool) override {}
    void renameEntry(const QString &, const QString &) override {}
    void moveEntry(const QString &, const QString &, int) override {}
    void createDirectory(const QString &, bool = false) override {}
    void createFile(const QString &) override {}
    void setPermissions(const QString &, int) override {}
    void listDirectoryForEnumeration(const QString &path, int requestId) override {
        emit directoryEnumerated(path, {}, requestId);
    }
    void checkExists(const QString &, int requestId) override {
        emit existsChecked(QString(), false, false, requestId);
    }
};
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Isolates BOTH queue.json (this feature) and sites.json (read by
    // TransferQueueWidget::statusText()'s savedSiteId->name lookup, not
    // directly exercised here, but TransferQueueStore/SiteStore share the
    // same AppConfigLocation) from the real user's actual config — same
    // convention site-store-test/app-settings-test already establish.
    QStandardPaths::setTestModeEnabled(true);

    const QString testDir = QStringLiteral("/tmp/queue_persistence_test");
    QDir().mkpath(testDir);
    {
        QDir d(testDir);
        for (const QString &name : d.entryList(QDir::Files))
            QFile::remove(d.filePath(name));
    }

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    // Same polling idiom move-entry-test.cpp/edit-session-test.cpp
    // already establish.
    auto waitFor = [&](bool &flag, int timeoutMs = 5000) {
        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeoutTimer.start(timeoutMs);
        while (!flag && timeoutTimer.isActive())
            loop.processEvents(QEventLoop::AllEvents, 20);
        return flag;
    };

    // --- Scenario 1: LocalToLocal round trip -- dispatches immediately, no reconnect needed ---
    {
        const QString srcPath = testDir + QStringLiteral("/local_source.txt");
        const QString dstPath = testDir + QStringLiteral("/local_dest.txt");
        QFile::remove(dstPath);
        {
            QFile f(srcPath);
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            f.write("local content");
        }

        PersistedTransferItem entry;
        entry.fileName = QStringLiteral("local_source.txt");
        entry.sourcePath = srcPath;
        entry.destPath = dstPath;
        entry.direction = TransferDirection::LocalToLocal;
        check("setup: queue.json saved with one LocalToLocal entry", TransferQueueStore::save({entry}));

        auto *pane = new FilePaneWidget(new LocalBackend());
        auto *manager = new TransferManager(&app);
        bool done = false;
        QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
            if (item.direction == TransferDirection::LocalToLocal && item.status == TransferStatus::Done)
                done = true;
        });
        manager->restorePersistedQueue(pane);
        check("scenario 1: restored LocalToLocal item dispatches and completes with no reconnect",
              waitFor(done));
        QFile out(dstPath);
        out.open(QIODevice::ReadOnly);
        check("scenario 1: dest content matches source", out.readAll() == "local content");
    }

    // --- Scenario 2/3/4: RemoteToLocal -- PendingReconnect, non-matching
    // reconnect leaves it alone, matching reconnect claims + resumes,
    // one reconnect claims multiple items sharing a connection ---
    {
        const QString dest1 = testDir + QStringLiteral("/remote_download_1.txt");
        const QString dest2 = testDir + QStringLiteral("/remote_download_2.txt");
        QFile::remove(dest1);
        QFile::remove(dest2);

        PersistedTransferItem entry1;
        entry1.fileName = QStringLiteral("file1.txt");
        entry1.sourcePath = QStringLiteral("/remote/file1.txt");
        entry1.destPath = dest1;
        entry1.direction = TransferDirection::RemoteToLocal;
        entry1.bytesDone = 500;
        entry1.bytesTotal = 1000;
        entry1.sourceConnection.protocol = Protocol::Sftp;
        entry1.sourceConnection.host = QStringLiteral("test-server.example.com");
        entry1.sourceConnection.port = 22;
        entry1.sourceConnection.username = QStringLiteral("alice");

        PersistedTransferItem entry2 = entry1;
        entry2.fileName = QStringLiteral("file2.txt");
        entry2.sourcePath = QStringLiteral("/remote/file2.txt");
        entry2.destPath = dest2;
        entry2.bytesDone = 0;

        check("setup: queue.json saved with two RemoteToLocal entries sharing one connection",
              TransferQueueStore::save({entry1, entry2}));

        auto *localPane = new FilePaneWidget(new LocalBackend());
        auto *manager = new TransferManager(&app);
        manager->restorePersistedQueue(localPane);

        int pendingCount = 0;
        for (const TransferItem &item : manager->items()) {
            if (item.direction == TransferDirection::RemoteToLocal && item.status == TransferStatus::PendingReconnect)
                ++pendingCount;
        }
        check("scenario 2: both restored RemoteToLocal items are PendingReconnect", pendingCount == 2);

        // A reconnect to a DIFFERENT server must not claim either item.
        auto *wrongFake = new FakeRemoteBackend();
        auto *wrongPane = new FilePaneWidget(wrongFake);
        ConnectionDescriptor wrongDescriptor;
        wrongDescriptor.protocol = Protocol::Sftp;
        wrongDescriptor.host = QStringLiteral("different-server.example.com");
        wrongDescriptor.port = 22;
        wrongDescriptor.username = QStringLiteral("alice");
        wrongPane->setConnectionDescriptor(wrongDescriptor);
        manager->tryReclaimPendingItems(wrongPane);

        pendingCount = 0;
        for (const TransferItem &item : manager->items()) {
            if (item.direction == TransferDirection::RemoteToLocal && item.status == TransferStatus::PendingReconnect)
                ++pendingCount;
        }
        check("scenario 3: a non-matching reconnect leaves both items PendingReconnect", pendingCount == 2);

        // The matching reconnect should claim BOTH items in one call.
        auto *fake = new FakeRemoteBackend();
        auto *remotePane = new FilePaneWidget(fake);
        ConnectionDescriptor descriptor;
        descriptor.protocol = Protocol::Sftp;
        descriptor.host = QStringLiteral("test-server.example.com");
        descriptor.port = 22;
        descriptor.username = QStringLiteral("alice");
        remotePane->setConnectionDescriptor(descriptor);

        int doneCount = 0;
        QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
            if (item.direction == TransferDirection::RemoteToLocal && item.status == TransferStatus::Done)
                ++doneCount;
        });
        manager->tryReclaimPendingItems(remotePane);

        bool bothDone = false;
        {
            QEventLoop loop;
            QTimer timeoutTimer;
            timeoutTimer.setSingleShot(true);
            QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeoutTimer.start(5000);
            while (doneCount < 2 && timeoutTimer.isActive())
                loop.processEvents(QEventLoop::AllEvents, 20);
            bothDone = doneCount == 2;
        }
        check("scenario 4: one matching reconnect claims and completes both pending items", bothDone);
        // file1 (bytesDone=500 when persisted) dispatches before file2
        // (bytesDone=0) -- startNext() scans m_items in insertion order,
        // and restorePersistedQueue() appends them in the order
        // TransferQueueStore::load() returned them, which preserves the
        // save-time order (entry1 then entry2).
        check("scenario 2: file1's persisted resume offset (500) reached downloadFile()",
              fake->downloadResumeOffsets.size() == 2 && fake->downloadResumeOffsets.first() == 500);
        check("scenario 2: file2's persisted resume offset (0) reached downloadFile()",
              fake->downloadResumeOffsets.size() == 2 && fake->downloadResumeOffsets.last() == 0);
    }

    // --- Scenario 5: saveQueueForShutdown() excludes terminal and Move items ---
    {
        const QString excludeSrcDir = testDir + QStringLiteral("/exclude_src");
        const QString excludeDstDir = testDir + QStringLiteral("/exclude_dst");
        QDir().mkpath(excludeSrcDir);
        QDir().mkpath(excludeDstDir);
        const QString srcPath = excludeSrcDir + QStringLiteral("/exclude_source.txt");
        const QString dstPath = excludeDstDir + QStringLiteral("/exclude_source.txt");
        QFile::remove(dstPath);
        {
            QFile f(srcPath);
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            f.write("x");
        }

        // Two separate panes/directories, not one pane used as both
        // source and destination -- enqueue(pane, pane, name) would
        // build an IDENTICAL source and destination path (the file
        // "already exists" at its own location), triggering a real,
        // undismissed Overwrite/Skip QMessageBox that would hang this
        // test forever. Same two-pane-two-directory shape
        // transfer_pause_test.cpp's own LocalToLocal scenario already
        // uses, for the same reason.
        auto *srcPane = new FilePaneWidget(new LocalBackend());
        auto *dstPane = new FilePaneWidget(new LocalBackend());
        auto *manager = new TransferManager(&app);

        // navigateTo() dispatches asynchronously -- give both a real
        // chance to land before enqueue() reads currentDirectory(), same
        // ordering transfer_pause_test.cpp's own navigateTo() call sites
        // rely on.
        bool navigated = false;
        srcPane->navigateTo(excludeSrcDir);
        dstPane->navigateTo(excludeDstDir);
        QTimer::singleShot(100, &app, [&]() { navigated = true; });
        waitFor(navigated, 2000);

        bool done = false;
        QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
            if (item.status == TransferStatus::Done)
                done = true;
        });
        manager->enqueue(srcPane, dstPane, QStringLiteral("exclude_source.txt"));
        check("setup: a real LocalToLocal item reaches Done", waitFor(done));

        manager->saveQueueForShutdown();
        const QList<PersistedTransferItem> afterDone = TransferQueueStore::load();
        check("scenario 5: a Done item is not persisted", afterDone.isEmpty());

        // A RemoteToRemote item is excluded by direction alone,
        // regardless of status -- checked synchronously, right after
        // enqueue() returns and before any event-loop turn processes its
        // async checkExists() call, so the item is still exactly Queued
        // here (the same early-continue in saveQueueForShutdown() also
        // excludes Move/EditDownload/EditUpload/Unsupported identically;
        // RemoteToRemote is the one of those that's actually
        // constructible through the ordinary enqueue() path with two
        // fake backends, same technique remote_to_remote_test.cpp uses).
        auto *fakeA = new FakeRemoteBackend();
        auto *fakeB = new FakeRemoteBackend();
        auto *paneA = new FilePaneWidget(fakeA);
        auto *paneB = new FilePaneWidget(fakeB);
        manager->enqueue(paneA, paneB, QStringLiteral("remote_file.txt"));
        manager->saveQueueForShutdown();
        const QList<PersistedTransferItem> afterRemoteToRemote = TransferQueueStore::load();
        check("scenario 5: a RemoteToRemote item is not persisted", afterRemoteToRemote.isEmpty());
    }

    // --- Scenario 6: queue.json never contains a secret-shaped key ---
    {
        PersistedTransferItem entry;
        entry.fileName = QStringLiteral("f.txt");
        entry.sourcePath = QStringLiteral("/a/f.txt");
        entry.destPath = QStringLiteral("/b/f.txt");
        entry.direction = TransferDirection::RemoteToLocal;
        entry.sourceConnection.protocol = Protocol::Sftp;
        entry.sourceConnection.host = QStringLiteral("host");
        entry.sourceConnection.port = 22;
        entry.sourceConnection.username = QStringLiteral("alice");
        TransferQueueStore::save({entry});

        const QString queuePath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/queue.json");
        QFile raw(queuePath);
        check("scenario 6: queue.json exists on disk", raw.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(raw.readAll());
        check("scenario 6: no password-shaped key in queue.json", !content.contains(QStringLiteral("password"), Qt::CaseInsensitive));
        check("scenario 6: no passphrase-shaped key in queue.json", !content.contains(QStringLiteral("passphrase"), Qt::CaseInsensitive));
    }

    qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}

#include "queue_persistence_test.moc"
