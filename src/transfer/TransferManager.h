#pragma once

#include <QObject>
#include <QList>
#include <QHash>
#include <QElapsedTimer>
#include <QPointer>
#include "TransferItem.h"
#include "FolderEnumerator.h"

class RemoteBackend;

// Owns the transfer queue and processes it one item at a time. Serial by
// design: SftpBackend holds a single libssh2 session, and running two
// transfers concurrently on the same session isn't safe without a lot more
// synchronization than this app needs yet. Parallelism (e.g. one transfer
// per direction) is a reasonable future step, not attempted here.
class TransferManager : public QObject {
    Q_OBJECT
public:
    explicit TransferManager(QObject *parent = nullptr);

    // Builds a TransferItem from the two panes + a filename (as reported by
    // FilePaneWidget::fileActivated), figures out direction from each
    // pane's backend, and appends it to the queue. Starts processing
    // immediately if nothing else is currently running.
    void enqueue(FilePaneWidget *sourcePane, FilePaneWidget *destPane, const QString &fileName);

    // Recursively transfers a whole folder: enumerates it via
    // FolderEnumerator (against the SOURCE backend), creates the mirrored
    // directory structure on the destination, then enqueues every file
    // found through the exact same enqueue() above — a folder transfer
    // is not a different code path from individual file transfers as far
    // as the visible queue, pause/resume/cancel, or speed tracking are
    // concerned; it's just enqueue() called many times with relative
    // paths instead of bare filenames (which already works correctly
    // unchanged, since joinPath(dir, "sub/dir/file") produces exactly
    // the right nested path). Remote-to-remote works the same way — each
    // discovered file stages through its own local temp file via
    // enqueue()'s RemoteToRemote handling, same as a standalone file.
    void enqueueFolder(FilePaneWidget *sourcePane, FilePaneWidget *destPane, const QString &folderName);

    // Moves a single file/folder-root server-side within ONE backend (both
    // panes on the same connection — see moveEligible()/
    // RemoteBackend::connectionIdentity()) instead of enqueue()'s
    // download+upload. Deliberately NOT routed through the ordinary
    // Queued/m_activeIndex pipeline: a rename is a single control-
    // connection round trip, not a data transfer with meaningful
    // progress/pause/cancel, so nothing is gained by serializing it
    // behind whatever enqueue()'d transfer happens to be running —
    // it's simply dispatched (and since it targets the SAME backend
    // object as any transfer already running against it, still only
    // actually executes after that transfer's own queued call returns,
    // courtesy of Qt's per-thread FIFO event queue — no explicit
    // coordination needed here). Still appends a TransferItem and emits
    // itemAdded/itemUpdated so it's visible in the same queue view.
    // Silently does nothing if the two panes aren't move-eligible —
    // MainWindow is expected to have already checked moveEligible() and
    // shown its own explanatory message before ever calling this, so
    // reaching here ineligible means a caller bug, not a user-facing
    // case worth its own error path.
    void moveEntry(FilePaneWidget *sourcePane, FilePaneWidget *destPane, const QString &fileName);

    // Same idea for a whole folder: skips FolderEnumerator entirely and
    // issues one backend moveEntry() call on the folder's root path —
    // the whole subtree relocates for free as a side effect of the
    // server renaming the parent directory. Reuses the same root-level
    // conflict check enqueueFolder() runs (via askConflict()) for a
    // consistent prompt, but unlike enqueueFolder()'s "Write Into"
    // (merge into an existing folder by transferring every file
    // individually), a single rename() call cannot merge into a
    // non-empty existing destination (POSIX rename(2) semantics, which
    // both SFTP's RENAME and FTP's RNFR/RNTO map onto: renaming onto a
    // non-empty directory fails). "Write Into" here therefore fails the
    // move with a clear message rather than attempting anything —
    // real merge machinery (copy the tree, then delete the source)
    // would be significant new scope this project's existing "no
    // recursive delete" precedent argues against, not a small addition
    // to this pass.
    void moveFolder(FilePaneWidget *sourcePane, FilePaneWidget *destPane, const QString &folderName);

    // Edit-in-place's two halves — see TransferDirection::EditDownload/
    // EditUpload's own doc comment and EditSessionManager (src/ui/
    // EditSessionManager.h/.cpp), the sole caller of both. Neither goes
    // through enqueue(): that method always requires two real
    // FilePaneWidget objects and always runs a destination conflict
    // check, neither of which fits here (an edit download's destination
    // is a fresh, guaranteed-unique temp path; an edit upload's
    // "conflict" is the file the user was just editing, not a real one).
    // Both return the new item's id so the caller can watch itemUpdated()
    // for that specific id.
    int startEditDownload(FilePaneWidget *sourcePane, const QString &fileName);
    int startEditUpload(FilePaneWidget *destPane, const QString &localTempPath,
                         const QString &remotePath, const QString &fileName);

    // True when both panes have a backend and both report the same,
    // non-empty connectionIdentity() — the precondition for both methods
    // above. Public so MainWindow can check it BEFORE ever calling
    // moveEntry()/moveFolder(), to show its own explanatory message for
    // the ineligible case rather than relying on this class's silent
    // no-op.
    static bool moveEligible(FilePaneWidget *sourcePane, FilePaneWidget *destPane);

    // Cancels by id. If the item is Queued (hasn't started), just marks it
    // Cancelled directly — nothing to interrupt. If it's the currently
    // InProgress item, calls the executing backend's requestCancel() and
    // remembers that the resulting transferFailed (if any) should be
    // reported as Cancelled rather than Failed — see m_activeItemCancelled.
    // No-op for ids that are already Done/Failed/Cancelled, or don't exist.
    void cancelItem(int id);

    // Resets a Failed, Cancelled, or Skipped item back to Queued (clearing
    // its error message and progress) and starts processing if idle. A
    // skipped item is retryable for the same reason a failed one is —
    // the person might change their mind about a conflict resolution
    // they made earlier, especially one applied automatically via
    // "apply to all" rather than chosen individually for this item. No-op
    // for ids in any other state, or that don't exist.
    void retryItem(int id);

    // Pauses the currently InProgress item — only meaningful for the
    // active item, since that's the only one actually mid-transfer.
    // No-op for a Queued/Paused/terminal item, or for one whose backend
    // doesn't support pausing (LocalBackend's requestPause() is a
    // documented no-op — see RemoteBackend.h). Unlike cancelItem(), no
    // flag is needed to disambiguate the result: transferPaused is its
    // own signal, unambiguous the moment it arrives, unlike
    // transferFailed which cancel and genuine errors both produce.
    void pauseItem(int id);

    // Re-queues a Paused item, preserving its bytesDone as the resume
    // offset (unlike retryItem(), which resets progress to zero — a
    // paused transfer has real partial progress worth keeping, a failed
    // one doesn't necessarily). No-op for ids not currently Paused.
    void resumeItem(int id);

    const QList<TransferItem> &items() const { return m_items; }

signals:
    void itemAdded(const TransferItem &item);
    void itemUpdated(const TransferItem &item);   // covers both progress and status changes
    void transferSucceeded();   // fired after any item completes — MainWindow uses this to refresh both panes

    // Folder-transfer status, separate from the per-file itemAdded/
    // itemUpdated above — a folder transfer isn't one queue item, it's
    // an enumeration phase followed by potentially many. folderTransferStarted
    // fires once enumeration begins (before any files are known yet, so
    // MainWindow can show a "Preparing..." message); folderTransferFinished
    // fires once every discovered file has been handed to enqueue()
    // (fileCount may be 0 for a folder containing only empty
    // subdirectories — still not a failure, just nothing for the visible
    // queue to do); folderTransferFailed fires if enumeration itself
    // couldn't complete (e.g. a permission error partway through the walk).
    // folderTransferSkipped fires when the destination already has a
    // folder with this name and the conflict was resolved as "skip" —
    // distinct from Finished(0 files), which means something different
    // (a real transfer that happened to find no files, e.g. a tree of
    // only empty subdirectories).
    void folderTransferStarted(const QString &folderName);
    void folderTransferFinished(const QString &folderName, int fileCount);
    void folderTransferFailed(const QString &folderName, const QString &reason);
    void folderTransferSkipped(const QString &folderName);

private slots:
    void onBackendProgress(const QString &fileName, qint64 bytesDone, qint64 bytesTotal);
    void onBackendFinished(const QString &fileName);
    void onBackendFailed(const QString &fileName, const QString &reason);
    void onBackendPaused(const QString &fileName, qint64 bytesDone);

    // Response to a checkExists() call — routes to whichever of the two
    // flows below issued the matching request id, since a file-conflict
    // check (mid-queue-processing) and a folder-conflict check
    // (enqueueFolder(), before enumeration even starts) can genuinely
    // overlap: askConflict()'s QMessageBox::exec() is modal but still
    // pumps the event loop internally, so a brand new drag-and-drop
    // could trigger enqueueFolder() while an earlier conflict dialog is
    // still on screen. Two separate pending-id trackers (rather than one
    // shared one) keep these from corrupting each other if that happens.
    void onDestinationExistsChecked(const QString &path, bool exists, bool isDir, int requestId);

    // Response to a moveEntry() backend call — requestId-correlated
    // against m_pendingMoveItemId (NOT m_activeIndex; move items never
    // become "the active item" — see moveEntry()'s own doc comment),
    // since unlike the single-file/single-folder conflict check above,
    // more than one move's backend call could plausibly be in flight
    // at once if the person fires off several "Move Selected" actions
    // in quick succession.
    void onEntryMoved(int requestId);
    void onEntryMoveFailed(const QString &reason, int requestId);

private:
    void startNext();
    void connectToBackend(RemoteBackend *backend);
    int indexById(int id) const;

    // Actually kicks off FolderEnumerator against the source folder —
    // split out from enqueueFolder() so it can be called either
    // immediately (no destination conflict) or after the person resolves
    // a "this folder already exists" prompt as Write Into.
    void startFolderEnumeration(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                                 const QString &folderName);

    // Second phase of enqueueFolder(), called once FolderEnumerator
    // finishes: creates every directory the walk found (dispatched via
    // QMetaObject::invokeMethod(..., Qt::QueuedConnection), which
    // preserves FIFO order per target object — so the destination
    // backend processes them strictly in FolderEnumerator's
    // parent-before-child order regardless of how long each individual
    // creation takes; no need to wait for each one's own completion
    // signal before issuing the next), then hands every file to the
    // ordinary enqueue() above.
    void startFolderFileTransfers(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                                   const QString &folderName, const QList<EnumeratedItem> &items);

    // Second half of startNext()'s old body — actually tells the backend
    // to start uploading/downloading. Split out specifically so the file-
    // conflict check (checkExists(), async) can sit in between "an item
    // was picked to run" and "the backend was actually told to run it".
    // Also re-entered directly (not through startNext()) by
    // onBackendFinished() when a RemoteToRemote item transitions from its
    // Downloading phase to its Uploading phase — see that method's own
    // comment.
    void dispatchActiveItem();

    // Local-disk staging for RemoteToRemote items — RemoteBackend has no
    // direct server-to-server primitive, so a temp file is genuinely the
    // only way to move a file between two remote backends.
    //
    // allocateTempFilePath() is called once per item, on its first
    // dispatch (not at enqueue() time — a cancelled-while-still-Queued
    // item should never claim a filename it won't use). Lives under
    // QStandardPaths::TempLocation in a dedicated subdirectory, named with
    // the item's own unique id to prevent collisions.
    QString allocateTempFilePath(const TransferItem &item) const;

    // Same staging directory/sweep-on-launch as allocateTempFilePath()
    // above, but "edit_"-prefixed so an edit-in-place download's temp
    // file is visually distinguishable from a RemoteToRemote one if
    // someone inspects zephyrftp-staging/ directly — and, unlike
    // allocateTempFilePath()'s callers, this one's result deliberately
    // outlives the download item's own Done status (the user is about
    // to start editing it); see EditSessionManager for the component
    // that actually deletes it once editing ends.
    QString allocateEditTempFilePath(const TransferItem &item) const;

    // No-op for every direction except RemoteToRemote (checked internally
    // via item.direction/tempFilePath). Called from onBackendFinished()
    // (phase-2 success) and onBackendFailed() (phase-1 or phase-2
    // failure/cancellation — both surface through the same path). Safe to
    // call even if the file was never created: QFile::remove() on a
    // nonexistent path is a no-op.
    void cleanupTempFile(TransferItem &item);

    // Connects a backend's existsChecked signal to
    // onDestinationExistsChecked() — safe to call every time a check is
    // issued, even repeatedly against the same backend, since
    // Qt::UniqueConnection makes it a silent no-op if that exact
    // connection already exists. Deliberately never disconnected (unlike
    // connectToBackend()'s progress/finished/failed wiring, which DOES
    // need to be torn down between transfers to avoid double-delivery):
    // existsChecked's requestId already disambiguates which call a given
    // response belongs to, so there's no double-delivery risk to avoid
    // in the first place, and tearing down here would risk losing a
    // response if two different backends both have checks in flight at
    // once (see onDestinationExistsChecked's own doc comment on why that
    // can genuinely happen).
    void ensureExistsCheckConnected(RemoteBackend *backend);

    // Same idea as ensureExistsCheckConnected() but for the
    // entryMoved/entryMoveFailed pair moveEntry()/moveFolder() below
    // rely on — connects both, Qt::UniqueConnection so repeat calls
    // against the same backend are harmless no-ops.
    void ensureMoveConnected(RemoteBackend *backend);

    // Second half of moveEntry()/moveFolder(): called once a
    // destination conflict check has come back clear (or been resolved
    // as Overwrite/Write Into) for a SINGLE root path — appends the
    // visible TransferItem, dispatches the actual backend moveEntry()
    // call, and stashes the request id -> item id mapping
    // onEntryMoved()/onEntryMoveFailed() resolve against. name is a
    // folder or file name relative to both panes' current directories;
    // identical for a file move and a whole-folder move, since a folder
    // move is just this same single call issued against the folder's
    // root path instead of individually walking its contents.
    void dispatchMoveEntry(FilePaneWidget *sourcePane, FilePaneWidget *destPane, const QString &name);

    // Shows the Overwrite/Skip (files) or Write Into/Skip (folders)
    // dialog. Returns true for Overwrite/Write Into, false for Skip;
    // applyToAll is set from the "do this for all remaining conflicts"
    // checkbox regardless of which button was clicked. Safe to call
    // directly (not through some cross-thread mechanism) — unlike
    // SftpBackend, TransferManager always lives on the GUI thread.
    bool askConflict(const QString &name, bool isDirectory, bool &applyToAll);

    QList<TransferItem> m_items;
    int m_nextId = 1;
    int m_activeIndex = -1;          // index into m_items currently running, -1 if idle
    // QPointer, not a raw pointer: the pane that owns this backend can be
    // disconnected/reconnected mid-session (FilePaneWidget::setBackend()
    // deleteLater()s the old backend), and connectToBackend() below reads
    // this to decide what to disconnect from before the new one — a raw
    // pointer would go dangling and crash on the next transfer.
    QPointer<RemoteBackend> m_currentBackend;   // whichever backend is executing the active item
    // Set by cancelItem() when it cancels the currently-InProgress item;
    // onBackendFailed() checks this to report Cancelled instead of Failed,
    // then clears it. There's no other way to distinguish "the user asked
    // for this to stop" from "it genuinely errored" once both surface as
    // the same transferFailed signal from the backend.
    bool m_activeItemCancelled = false;

    // Live speed sampling for the active item. Recomputed roughly every
    // 250ms in onBackendProgress() rather than on every single progress
    // signal (SFTP's read loop emits one per 32KB chunk, which on a fast
    // connection would be noisy and not meaningfully "live" anyway).
    QElapsedTimer m_speedSampleTimer;
    qint64 m_speedSampleBytesAtLastSample = 0;

    // Each raw 250ms sample above is a real, unlagged measurement, but
    // with nothing carried over between windows it visibly jumps around
    // with the transfer's natural burstiness (TCP window dynamics, disk
    // flush stalls, scheduler jitter) — confirmed by directly comparing
    // against other SFTP clients' noticeably calmer live readouts, which
    // turned out to be smoothing their samples rather than being more
    // accurate. An exponential moving average here trades a small amount
    // of lag for the same calmer display, without changing how the raw
    // 250ms sample itself is computed. Reset in dispatchActiveItem() so a
    // new transfer doesn't start smoothed from a previous one's speed.
    double m_smoothedSpeedBytesPerSec = 0.0;
    bool m_hasSpeedSample = false;

    // Conflict resolution — "remembered" choices reset back to Ask
    // whenever the queue fully drains (see startNext()'s "nothing left
    // to run" path), so a fresh batch of transfers gets fresh decisions
    // rather than silently inheriting a choice from an unrelated earlier
    // transfer. File and directory conflicts are tracked independently,
    // matching the two separate checkboxes/decisions the person actually
    // gets asked about.
    enum class ConflictResolution { Ask, AlwaysOverwrite, AlwaysSkip };
    ConflictResolution m_fileConflictResolution = ConflictResolution::Ask;
    ConflictResolution m_directoryConflictResolution = ConflictResolution::Ask;

    int m_nextConflictCheckId = 1;
    int m_pendingFileConflictCheckId = -1;

    // Stashed while a folder's root-conflict check (enqueueFolder(),
    // before enumeration starts) is in flight — a QHash keyed by
    // requestId, NOT a single shared scalar (this used to be three
    // scalars — id/sourcePane/destPane/name — and it was a real bug:
    // MainWindow's drag-drop/multi-select handling can call
    // enqueueFolder() for several folders in one synchronous loop before
    // any of their checkExists() calls resolve, so a shared scalar let
    // each new call silently clobber the previous one's stashed pane/
    // name before its response ever arrived, dropping every folder but
    // the last one in a multi-folder drag. Same fix as
    // m_pendingMoveConflictChecks below, which had already hit and fixed
    // the identical bug for Move — see that struct's own comment.
    struct PendingFolderConflictCheck {
        FilePaneWidget *sourcePane = nullptr;
        FilePaneWidget *destPane = nullptr;
        QString folderName;
    };
    QHash<int, PendingFolderConflictCheck> m_pendingFolderConflictChecks;

    // Stashed while a moveEntry()'s/moveFolder()'s own root-conflict
    // check is in flight — a QHash keyed by requestId, NOT a single
    // shared scalar (an earlier version of this used one, and it was a
    // real bug: MainWindow::moveEntries() can dispatch several entries
    // in one synchronous loop before any of their checkExists() calls
    // resolve, so a shared scalar let each new call silently clobber the
    // previous one's stashed pane/name before its response ever arrived,
    // dropping every item but the last one in a multi-select Move). Also
    // a separate id-space from m_pendingFileConflictCheckId/
    // m_pendingFolderConflictChecks above (rather than reusing either)
    // so a Move's conflict check can never collide with an ordinary
    // enqueue()/enqueueFolder() check in flight at the same moment.
    struct PendingMoveConflictCheck {
        FilePaneWidget *sourcePane = nullptr;
        FilePaneWidget *destPane = nullptr;
        QString name;
        bool isFolder = false;   // which askConflict() wording + which post-conflict path to use
    };
    QHash<int, PendingMoveConflictCheck> m_pendingMoveConflictChecks;

    // Move's OWN file/folder conflict-resolution state — deliberately
    // separate from m_fileConflictResolution/m_directoryConflictResolution
    // above, not shared. Those are only ever reset back to Ask in
    // startNext()'s "nothing left to run" branch, which Move never calls
    // (by design — see moveEntry()'s doc comment on why Move bypasses the
    // ordinary Queued/m_activeIndex pipeline entirely). Sharing them was a
    // real bug: a Move batch's "apply to all, Write Into" choice would
    // persist indefinitely and silently apply to a completely unrelated
    // ordinary transfer's conflict later in the session, with no prompt.
    // Reset via maybeResetMoveConflictResolution() once no Move activity
    // remains outstanding.
    ConflictResolution m_moveFileConflictResolution = ConflictResolution::Ask;
    ConflictResolution m_moveDirectoryConflictResolution = ConflictResolution::Ask;

    // Resets m_moveFileConflictResolution/m_moveDirectoryConflictResolution
    // back to Ask once every Move-related bookkeeping structure is empty
    // (no conflict check in flight, no backend call in flight) — the
    // closest Move equivalent to startNext()'s "queue fully drained"
    // reset, since Move has no single queue to drain. Safe to call
    // liberally; a no-op unless both are actually empty.
    void maybeResetMoveConflictResolution();

    // Maps a moveEntry() backend-call request id to the TransferItem::id
    // it belongs to, resolved in onEntryMoved()/onEntryMoveFailed(). Not
    // reusing m_activeIndex/m_currentBackend for this — move items are
    // deliberately never "the active item" (see moveEntry()'s doc
    // comment), so several could plausibly have calls in flight at once.
    int m_nextMoveRequestId = 1;
    QHash<int, int> m_pendingMoveItemId;
};
