// Tests the server-side Move feature end to end at the TransferManager
// level: TransferManager::moveEligible()'s identity-equality guard,
// moveEntry()/moveFolder()'s request-id-correlated dispatch to a backend's
// moveEntry() (NOT downloadFile()/uploadFile() — the whole point of Move is
// skipping the data-transfer path entirely), that a whole-folder move
// issues exactly one backend call rather than walking the tree via
// FolderEnumerator, and — the one path ARCHITECTURE.md used to flag as
// manual-only — that a folder move onto an EXISTING destination folder
// resolved as "Write Into" fails cleanly (a single rename can't merge)
// rather than attempting a doomed rename or silently dispatching nothing.
// Also confirms LocalBackend::moveEntry() performs a real QDir::rename()
// against real temp files/directories, including the
// pre-existing-file-destination overwrite case — mirrors
// file_operations_test.cpp's "test the backend directly against a real
// temp directory" approach for that part.
//
// Three scenarios below are regression tests for real bugs a code review
// found in TransferManager's Move implementation after it had already
// shipped: (1) multi-select Move silently dropped every entry but the
// last one — the conflict-check stage stashed per-call state in shared
// scalar members instead of a per-request map, so MainWindow::
// moveEntries()'s synchronous per-entry loop clobbered each call's state
// before its async response ever arrived; (2) a Move's "apply to all"
// conflict-resolution choice leaked into a completely unrelated later
// transfer, since it shared state with the ordinary transfer pipeline
// but Move never triggers that pipeline's reset; (3) retryItem() had no
// guard for a Failed Move item, so calling it (currently prevented only
// by the UI, not the model) would silently misdispatch through a
// pipeline with no case for Move. See each fix's own comment in
// TransferManager.{h,cpp} for the full detail.
//
// Event-driven throughout, not fixed-delay — see remote_to_remote_test.cpp's
// own header comment for why a fixed wall-clock delay isn't safe against
// arbitrary system load. Every backend call here resolves in at most one
// queued-connection hop (no multi-tick timers), so each stage simply reacts
// to the specific signal that proves that hop actually happened. The one
// exception is the real conflict QMessageBox the "Write Into fails"
// scenario drives — conflict-resolution-test.cpp's technique (a timer
// scheduled to fire during the dialog's still-blocking exec() call, which
// pumps the event loop internally) applied here as a continuous poller
// rather than a single fixed-delay shot, since this test doesn't know in
// advance exactly when the dialog will appear.
#include <QApplication>
#include <QAbstractButton>
#include <QCheckBox>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QMetaObject>
#include <QTimer>
#include <functional>
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
    void setPermissions(const QString &, int) override {}

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
    // Without this, closing the real conflict QMessageBox the
    // WaitFolderConflictAdded scenario drives below (the only actually-
    // visible window anywhere in this test) triggers Qt's default
    // "quit when the last window closes" behavior, ending the event
    // loop before that scenario's own checks get a chance to run — same
    // reasoning conflict_resolution_test.cpp's own setup has.
    app.setQuitOnLastWindowClosed(false);

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

    // ---------- Part 3: multi-select Move dispatches EVERY selected
    // entry, not just the last one. Regression test for a real bug found
    // by code review: an earlier version stashed a moveEntry()/
    // moveFolder() conflict check's source/dest pane + name + isFolder
    // in single shared scalar members rather than a per-request map.
    // MainWindow::moveEntries() calls moveEntry()/moveFolder() in a
    // plain synchronous loop for a multi-select "Move Selected" — so
    // each call in that loop silently clobbered the previous call's
    // stashed state before its own async checkExists() response ever
    // arrived, dropping every item but the last one with no error shown
    // at all. Reproduced here by calling moveEntry() twice in a row,
    // synchronously, before returning control to the event loop — the
    // exact pattern MainWindow::moveEntries()'s selection loop
    // produces. ----------
    {
        auto *srcFake = new FakeMoveBackend(QStringLiteral("fake://multiselect"));
        auto *dstFake = new FakeMoveBackend(QStringLiteral("fake://multiselect"));
        auto *srcPane = new FilePaneWidget(srcFake);
        auto *dstPane = new FilePaneWidget(dstFake);
        auto *mgr = new TransferManager(&app);

        int doneCount = 0;
        QObject::connect(mgr, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
            if (item.status == TransferStatus::Done)
                ++doneCount;
        });

        // Synchronous, back to back — no event-loop turn in between.
        mgr->moveEntry(srcPane, dstPane, QStringLiteral("multi1.bin"));
        mgr->moveEntry(srcPane, dstPane, QStringLiteral("multi2.bin"));

        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeoutTimer.start(5000);
        while (doneCount < 2 && timeoutTimer.isActive())
            loop.processEvents(QEventLoop::AllEvents, 50);

        check("multi-select move: BOTH entries got a real TransferItem (not just the last one)",
              mgr->items().size() == 2);
        check("multi-select move: BOTH reached Done", doneCount == 2);
        check("multi-select move: the destination backend's moveEntry() was called twice",
              dstFake->moveEntryCallCount == 2);
    }

    // ---------- Part 4: a Move's "apply to all" conflict-resolution
    // choice must NOT leak into a separate, later Move conflict.
    // Regression test for a real bug found by code review:
    // m_fileConflictResolution/m_directoryConflictResolution (the
    // ordinary enqueue()/enqueueFolder() pipeline's state) used to be
    // shared with Move too, but are only ever reset in startNext()'s
    // "queue fully drained" branch — which Move never calls (by design,
    // since Move bypasses that pipeline entirely). A Move batch's
    // "apply to all, Write Into" choice would persist indefinitely, and
    // any LATER, completely unrelated Move (or ordinary transfer)
    // conflict would silently reuse it with no prompt at all. Proven
    // here behaviorally, not by inspecting internal state: a real
    // QMessageBox must appear again for a second, independent folder
    // conflict — under the old bug it would silently reuse the first
    // decision instead, and no dialog would ever be found. ----------
    {
        auto *srcFake = new FakeMoveBackend(QStringLiteral("fake://isolation"));
        auto *dstFake = new FakeMoveBackend(QStringLiteral("fake://isolation"));
        dstFake->existsAtCheck = true;   // every checkExists() in this block reports a conflict
        auto *srcPane = new FilePaneWidget(srcFake);
        auto *dstPane = new FilePaneWidget(dstFake);
        auto *mgr = new TransferManager(&app);

        bool dialogAppeared = false;
        auto driveOneDialog = [&](bool checkApplyToAll) {
            dialogAppeared = false;
            QEventLoop loop;
            QTimer poll;
            int elapsedMs = 0;
            QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    if (QCheckBox *checkbox = box->checkBox())
                        checkbox->setChecked(checkApplyToAll);
                    for (QAbstractButton *button : box->buttons()) {
                        if (button->text().contains(QStringLiteral("Write Into"), Qt::CaseInsensitive)) {
                            dialogAppeared = true;
                            button->click();
                            loop.quit();
                            return;
                        }
                    }
                }
                elapsedMs += 20;
                if (elapsedMs > 3000)
                    loop.quit();   // no dialog appeared within the window — a genuinely different, checkable outcome
            });
            poll.start(20);
            loop.exec();
        };

        mgr->moveFolder(srcPane, dstPane, QStringLiteral("isolation_a"));
        driveOneDialog(/*checkApplyToAll=*/true);
        check("conflict-resolution isolation: dialog appeared for the FIRST Move conflict", dialogAppeared);

        mgr->moveFolder(srcPane, dstPane, QStringLiteral("isolation_b"));
        driveOneDialog(/*checkApplyToAll=*/false);
        check("conflict-resolution isolation: a SEPARATE Move conflict still prompts "
              "(no leaked \"apply to all\" from the earlier, unrelated Move)",
              dialogAppeared);
    }

    // ---------- Part 5: TransferManager::moveEntry()/moveFolder() with two
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

    enum class Stage {
        WaitFileAdded, WaitFileDone, WaitFolderAdded, WaitFolderDone,
        WaitFolderConflictAdded, AllDone
    };
    Stage stage = Stage::WaitFileAdded;
    int currentItemId = -1;

    // Continuous poller (not a single fixed-delay shot — see this file's
    // header comment) for the real QMessageBox the WaitFolderConflictAdded
    // scenario below drives. Gated on the stage so it's a harmless no-op
    // during every earlier scenario, none of which ever open a dialog
    // (destFake->existsAtCheck stays false until that scenario sets it).
    std::function<void()> pollForConflictDialog;
    pollForConflictDialog = [&]() {
        if (stage == Stage::WaitFolderConflictAdded) {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                for (QAbstractButton *button : box->buttons()) {
                    if (button->text().contains(QStringLiteral("Write Into"), Qt::CaseInsensitive)) {
                        button->click();
                        break;
                    }
                }
            }
        }
        QTimer::singleShot(20, &app, pollForConflictDialog);
    };
    pollForConflictDialog();

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
        } else if (stage == Stage::WaitFolderConflictAdded) {
            // Unlike every scenario above, this one has no itemUpdated
            // step at all: TransferManager::onDestinationExistsChecked()'s
            // "Write Into fails" branch sets status = Failed BEFORE
            // appending the item, so itemAdded itself already carries the
            // terminal state — there's nothing further to dispatch or
            // wait for.
            check("folder move onto an existing destination + Write Into: "
                  "TransferItem direction is Move", item.direction == TransferDirection::Move);
            check("folder move onto an existing destination + Write Into: "
                  "fileName is the folder's own name", item.fileName == QStringLiteral("conflictfolder"));
            check("folder move onto an existing destination + Write Into: "
                  "status is Failed, not left dispatched/InProgress", item.status == TransferStatus::Failed);
            check("folder move onto an existing destination + Write Into: "
                  "error message explains the merge limitation, not a generic failure",
                  item.errorMessage.contains(QStringLiteral("merge"), Qt::CaseInsensitive));
            check("folder move onto an existing destination + Write Into: "
                  "no backend moveEntry() call was ever made (still 2, unchanged since the prior scenario)",
                  destFake->moveEntryCallCount == 2);

            stage = Stage::AllDone;
            // Deferred the same way the earlier scenarios' final checks
            // are — harmless here (nothing else is pending for this
            // item), but keeps the "verify after control returns to the
            // event loop" convention consistent throughout this file.
            QTimer::singleShot(0, &app, [&]() {
                check("folder move onto an existing destination + Write Into: "
                      "transferSucceeded did NOT fire again (still 2 — this was a failure, not a success)",
                      transferSucceededCount == 2);

                // Regression test for a real bug found by code review:
                // retryItem() had no guard for TransferDirection::Move,
                // so calling it on this exact Failed item would silently
                // misdispatch through the ordinary startNext()/
                // dispatchActiveItem() pipeline (which has no case for
                // Move) instead of safely no-op'ing — the same invariant
                // TransferQueueWidget already enforces by disabling
                // Retry for Move items, now also enforced here at the
                // model layer that actually owns it.
                const int itemCountBeforeRetry = manager->items().size();
                manager->retryItem(currentItemId);
                check("retryItem() on a Failed Move item is a safe no-op (status still Failed)",
                      manager->items().last().status == TransferStatus::Failed);
                check("retryItem() on a Failed Move item dispatched nothing new",
                      destFake->moveEntryCallCount == 2
                          && manager->items().size() == itemCountBeforeRetry);

                qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
                app.exit(allPass ? 0 : 1);
            });
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
                // Same deferred-check reasoning as the single-file stage
                // above — transferSucceeded() for THIS move hasn't been
                // emitted yet at the point itemUpdated is handled.
                QTimer::singleShot(0, &app, [&]() {
                    check("folder move: transferSucceeded fired a second time", transferSucceededCount == 2);
                });

                // Next: a folder move onto a destination that ALREADY
                // EXISTS, resolved as "Write Into" — the one path
                // ARCHITECTURE.md used to flag as manual-only. Driven via
                // a real QMessageBox (pollForConflictDialog(), already
                // running) rather than simulated.
                destFake->existsAtCheck = true;
                stage = Stage::WaitFolderConflictAdded;
                manager->moveFolder(sourcePane, destPane, QStringLiteral("conflictfolder"));
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
