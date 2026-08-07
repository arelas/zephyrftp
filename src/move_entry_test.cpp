// Tests the server-side Move feature end to end at the TransferManager
// level: TransferManager::moveEligible()'s identity-equality guard,
// moveEntry()/moveFolder()'s request-id-correlated dispatch to a backend's
// moveEntry() (NOT downloadFile()/uploadFile() — the whole point of Move is
// skipping the data-transfer path entirely), and that a whole-folder move
// issues exactly one backend call rather than walking the tree via
// FolderEnumerator. Also confirms LocalBackend::moveEntry() performs a real
// QDir::rename() against real temp files/directories, including the
// pre-existing-file-destination overwrite case — mirrors
// file_operations_test.cpp's "test the backend directly against a real
// temp directory" approach for that part.
//
// Event-driven throughout, not fixed-delay — see remote_to_remote_test.cpp's
// own header comment for why a fixed wall-clock delay isn't safe against
// arbitrary system load. Every backend call here resolves in at most one
// queued-connection hop (no multi-tick timers), so each stage simply reacts
// to the specific signal that proves that hop actually happened.
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QTimer>
#include <utility>
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"
#include "backends/RemoteBackend.h"
#include "transfer/TransferManager.h"

namespace {

// A minimal fake RemoteBackend whose moveEntry() resolves immediately
// (success or a configured simulated failure) — there's no meaningful
// "in-flight" state for a single control-connection round trip the way
// FakeRemoteBackend's download/upload ticks simulate one for real data
// transfer, so this doesn't need a QTimer at all. connectionIdentity() is
// passed in at construction so a test can build two fakes that either
// match (move-eligible) or don't (ineligible).
class FakeMoveBackend : public RemoteBackend {
    Q_OBJECT
public:
    explicit FakeMoveBackend(QString identity, QObject *parent = nullptr)
        : RemoteBackend(parent), m_identity(std::move(identity)) {}

    QString currentPath() const override { return m_currentPath; }
    bool isLocalFilesystem() const override { return false; }
    QString connectionIdentity() const override { return m_identity; }
    void requestCancel() override {}
    void requestPause() override {}

    // Test hooks.
    int moveEntryCallCount = 0;
    int listDirectoryForEnumerationCallCount = 0;
    QString lastMoveOldPath;
    QString lastMoveNewPath;
    bool existsAtCheck = false;   // controls checkExists()'s response below

public slots:
    void connectToHost() override { emit connected(); }
    void listDirectory(const QString &path) override { emit directoryListed(path, {}); }
    void downloadFile(const QString &, const QString &, qint64 = 0) override {}
    void uploadFile(const QString &, const QString &, qint64 = 0) override {}
    void deleteEntry(const QString &, bool) override {}
    void renameEntry(const QString &, const QString &) override {}
    void createDirectory(const QString &) override {}
    void createFile(const QString &) override {}

    // Deliberately a no-op beyond the call counter — the folder-move
    // scenario below asserts this is NEVER called, so it never needs a
    // real response.
    void listDirectoryForEnumeration(const QString &, int) override {
        ++listDirectoryForEnumerationCallCount;
    }

    // Must actually respond (not just satisfy the interface) — moveEntry()/
    // moveFolder() issue a checkExists() call before dispatching, and wait
    // for existsChecked() before proceeding. A no-op stub here would leave
    // that wait unanswered forever.
    void checkExists(const QString &path, int requestId) override {
        emit existsChecked(path, existsAtCheck, false, requestId);
    }

    void moveEntry(const QString &oldPath, const QString &newPath, int requestId) override {
        ++moveEntryCallCount;
        lastMoveOldPath = oldPath;
        lastMoveNewPath = newPath;
        emit entryMoved(requestId);
    }

private:
    QString m_identity;
    QString m_currentPath = QStringLiteral("/fake");
};

}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    // ---------- Part 1: LocalBackend::moveEntry() against real temp files/
    // directories — confirms the backend implementation itself, independent
    // of TransferManager's dispatch logic above it. ----------
    {
        const QString base = QStringLiteral("/tmp/move_entry_test");
        QDir(base).removeRecursively();
        QDir().mkpath(base);

        auto *local = new LocalBackend();

        bool moved = false, failed = false;
        int lastRequestId = -1;
        QString lastFailReason;
        QObject::connect(local, &RemoteBackend::entryMoved, &app, [&](int requestId) {
            moved = true;
            lastRequestId = requestId;
        });
        QObject::connect(local, &RemoteBackend::entryMoveFailed, &app,
                          [&](const QString &reason, int requestId) {
            failed = true;
            lastRequestId = requestId;
            lastFailReason = reason;
        });

        auto waitFor = [&](bool &flag) {
            QEventLoop loop;
            QTimer timeoutTimer;
            timeoutTimer.setSingleShot(true);
            QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeoutTimer.start(5000);
            while (!flag && timeoutTimer.isActive())
                loop.processEvents(QEventLoop::AllEvents, 50);
            return flag;
        };

        // A plain file move.
        {
            QFile source(base + "/source.txt");
            source.open(QIODevice::WriteOnly);
            source.write("hello");
            source.close();

            moved = false; failed = false;
            QMetaObject::invokeMethod(local, "moveEntry", Qt::QueuedConnection,
                                       Q_ARG(QString, base + "/source.txt"),
                                       Q_ARG(QString, base + "/moved.txt"),
                                       Q_ARG(int, 101));
            check("LocalBackend::moveEntry (file): entryMoved fired", waitFor(moved));
            check("LocalBackend::moveEntry (file): requestId echoed back", lastRequestId == 101);
            check("LocalBackend::moveEntry (file): old path is gone", !QFile::exists(base + "/source.txt"));
            check("LocalBackend::moveEntry (file): new path exists", QFile::exists(base + "/moved.txt"));
        }

        // A file move onto an EXISTING destination file — moveEntry()
        // (unlike renameEntry()) is expected to overwrite it, mirroring
        // uploadFile()'s established Overwrite convention (see
        // LocalBackend::moveEntry()'s own doc comment).
        {
            QFile dest(base + "/existing.txt");
            dest.open(QIODevice::WriteOnly);
            dest.write("old content");
            dest.close();

            moved = false; failed = false;
            QMetaObject::invokeMethod(local, "moveEntry", Qt::QueuedConnection,
                                       Q_ARG(QString, base + "/moved.txt"),
                                       Q_ARG(QString, base + "/existing.txt"),
                                       Q_ARG(int, 102));
            check("LocalBackend::moveEntry (overwrite): entryMoved fired", waitFor(moved));
            QFile readBack(base + "/existing.txt");
            readBack.open(QIODevice::ReadOnly);
            check("LocalBackend::moveEntry (overwrite): destination now has the SOURCE's content",
                  readBack.readAll() == "hello");
            check("LocalBackend::moveEntry (overwrite): source path is gone",
                  !QFile::exists(base + "/moved.txt"));
        }

        // A folder move.
        {
            QDir().mkpath(base + "/sourcefolder");
            QFile marker(base + "/sourcefolder/inside.txt");
            marker.open(QIODevice::WriteOnly);
            marker.write("x");
            marker.close();

            moved = false; failed = false;
            QMetaObject::invokeMethod(local, "moveEntry", Qt::QueuedConnection,
                                       Q_ARG(QString, base + "/sourcefolder"),
                                       Q_ARG(QString, base + "/destfolder"),
                                       Q_ARG(int, 103));
            check("LocalBackend::moveEntry (folder): entryMoved fired", waitFor(moved));
            check("LocalBackend::moveEntry (folder): old path is gone", !QDir(base + "/sourcefolder").exists());
            check("LocalBackend::moveEntry (folder): new path exists", QDir(base + "/destfolder").exists());
            check("LocalBackend::moveEntry (folder): its contents moved too",
                  QFile::exists(base + "/destfolder/inside.txt"));
        }

        local->deleteLater();
    }

    // ---------- Part 2: TransferManager::moveEligible() rejects two
    // backends with different connectionIdentity() — synchronous, no event
    // loop needed: moveEligible() is checked and moveEntry()/moveFolder()
    // return before dispatching anything async when it fails. ----------
    {
        auto *backendA = new FakeMoveBackend(QStringLiteral("fake://serverA"));
        auto *backendB = new FakeMoveBackend(QStringLiteral("fake://serverB"));
        auto *paneA = new FilePaneWidget(backendA);
        auto *paneB = new FilePaneWidget(backendB);
        auto *manager = new TransferManager(&app);

        check("moveEligible(): false for two different connectionIdentity() backends",
              !TransferManager::moveEligible(paneA, paneB));

        manager->moveEntry(paneA, paneB, QStringLiteral("somefile.bin"));
        check("moveEntry() on ineligible panes: no item was queued",
              manager->items().isEmpty());
        check("moveEntry() on ineligible panes: the backend's moveEntry() was never called",
              backendA->moveEntryCallCount == 0 && backendB->moveEntryCallCount == 0);
    }

    // ---------- Part 3: TransferManager::moveEntry()/moveFolder() with two
    // backends reporting the SAME connectionIdentity() — the eligible path.
    // Event-driven state machine below. ----------
    auto *sourceFake = new FakeMoveBackend(QStringLiteral("fake://shared"));
    auto *destFake = new FakeMoveBackend(QStringLiteral("fake://shared"));
    auto *sourcePane = new FilePaneWidget(sourceFake);
    auto *destPane = new FilePaneWidget(destFake);
    auto *manager = new TransferManager(&app);

    check("moveEligible(): true for two backends with the same connectionIdentity()",
          TransferManager::moveEligible(sourcePane, destPane));

    int transferSucceededCount = 0;
    QObject::connect(manager, &TransferManager::transferSucceeded, &app, [&]() {
        ++transferSucceededCount;
    });

    enum class Stage { WaitFileAdded, WaitFileDone, WaitFolderAdded, WaitFolderDone, AllDone };
    Stage stage = Stage::WaitFileAdded;
    int currentItemId = -1;

    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        currentItemId = item.id;
        if (stage == Stage::WaitFileAdded) {
            check("single-file move: TransferItem direction is Move", item.direction == TransferDirection::Move);
            check("single-file move: status starts InProgress (not Queued — Move bypasses the ordinary pipeline)",
                  item.status == TransferStatus::InProgress);
            stage = Stage::WaitFileDone;
        } else if (stage == Stage::WaitFolderAdded) {
            check("folder move: TransferItem direction is Move", item.direction == TransferDirection::Move);
            check("folder move: fileName is the folder's own name", item.fileName == QStringLiteral("myfolder"));
            stage = Stage::WaitFolderDone;
        }
    });

    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.id != currentItemId)
            return;

        switch (stage) {
        case Stage::WaitFileDone:
            if (item.status == TransferStatus::Done) {
                check("single-file move: destFake->moveEntry() was called exactly once",
                      destFake->moveEntryCallCount == 1);
                check("single-file move: sourceFake->moveEntry() was NEVER called (dispatch goes to destBackend only)",
                      sourceFake->moveEntryCallCount == 0);
                check("single-file move: old/new paths built from each pane's own current directory",
                      destFake->lastMoveOldPath == QStringLiteral("/fake/fakefile.bin")
                          && destFake->lastMoveNewPath == QStringLiteral("/fake/fakefile.bin"));
                // onEntryMoved() emits itemUpdated (handled synchronously,
                // right here) BEFORE it goes on to emit transferSucceeded()
                // — checking transferSucceededCount from inside THIS
                // handler would see it too early. Deferred via a 0ms timer
                // so the check runs after control returns to the event
                // loop, by which point that second emission has already
                // happened.
                QTimer::singleShot(0, &app, [&]() {
                    check("single-file move: transferSucceeded fired (drives the free dual-pane refresh)",
                          transferSucceededCount == 1);
                });

                stage = Stage::WaitFolderAdded;
                manager->moveFolder(sourcePane, destPane, QStringLiteral("myfolder"));
            }
            break;
        case Stage::WaitFolderDone:
            if (item.status == TransferStatus::Done) {
                check("folder move: destFake->moveEntry() was called exactly once MORE (total 2)",
                      destFake->moveEntryCallCount == 2);
                check("folder move: dispatched against the folder's OWN root path, not a per-file path",
                      destFake->lastMoveOldPath == QStringLiteral("/fake/myfolder")
                          && destFake->lastMoveNewPath == QStringLiteral("/fake/myfolder"));
                check("folder move: listDirectoryForEnumeration() was NEVER called on either backend "
                      "(FolderEnumerator genuinely skipped, not just unobserved)",
                      sourceFake->listDirectoryForEnumerationCallCount == 0
                          && destFake->listDirectoryForEnumerationCallCount == 0);

                stage = Stage::AllDone;
                // Same deferred-check reasoning as the single-file stage
                // above — transferSucceeded() for THIS move hasn't been
                // emitted yet at the point itemUpdated is handled.
                QTimer::singleShot(0, &app, [&]() {
                    check("folder move: transferSucceeded fired a second time", transferSucceededCount == 2);
                    qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
                    app.exit(allPass ? 0 : 1);
                });
            }
            break;
        default:
            break;
        }
    });

    manager->moveEntry(sourcePane, destPane, QStringLiteral("fakefile.bin"));

    // Safety net, not the primary mechanism — same reasoning as
    // remote_to_remote_test.cpp's own deadline timer.
    QTimer::singleShot(10000, &app, [&]() {
        if (stage == Stage::AllDone)
            return;
        check(QStringLiteral("test timed out waiting for a state transition (stuck at stage %1)")
                  .arg(static_cast<int>(stage)),
              false);
        qDebug() << "[test] AT LEAST ONE FAILURE";
        app.exit(1);
    });

    return app.exec();
}

#include "move_entry_test.moc"
