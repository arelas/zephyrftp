#pragma once

#include <QObject>
#include <QList>
#include <QHash>
#include <QSet>
#include <QElapsedTimer>
#include <QPointer>
#include <QVarLengthArray>
#include "TransferItem.h"
#include "FolderEnumerator.h"
#include "TransferQueueStore.h"

class RemoteBackend;

// Owns the transfer queue. Concurrency is bounded not by a configurable
// limit but by how many distinct backend INSTANCES exist to dispatch
// against — in this app, at most the two panes' remote backends
// (SftpBackend/FtpBackend, each pinned to its own worker QThread and its
// own non-thread-safe session) plus however many LocalBackend instances
// happen to be executing. The actual safety invariant: at most one item
// may ever be dispatched against a given backend instance at a time
// (running two transfers concurrently on the same libssh2 session/FTP
// control connection isn't safe) — see m_active/ActiveTransfer below,
// which is how that invariant is enforced. Two items that need DIFFERENT
// backend instances (e.g. a left-pane upload and a simultaneous
// right-pane download) run genuinely concurrently; two items that need
// the SAME backend instance still serialize, same as before this
// concurrency support existed.
class TransferManager : public QObject {
    Q_OBJECT
public:
    explicit TransferManager(QObject *parent = nullptr);

    // Builds a TransferItem from the two panes + a filename (as reported by
    // FilePaneWidget::fileActivated), figures out direction from each
    // pane's backend, and appends it to the queue. Starts processing
    // immediately if nothing else is currently running.
    //
    // assumeOverwrite, when true, sets the new item's own
    // skipConflictCheckOnDispatch flag (the same one resumeItem() already
    // uses for a resumed transfer's own destination) so this item skips
    // startNext()'s destination-exists check and askConflict() entirely,
    // dispatching straight through — identical in effect to the user
    // having answered Overwrite, without a dialog. For a caller (e.g.
    // CompareSyncExecutor) that already knows from its own prior diff that
    // the destination exists and a deliberate overwrite is intended, this
    // avoids popping one askConflict() modal per file in a bulk batch.
    // Default false preserves every existing caller's behavior unchanged.
    //
    // Returns the new item's id — added for ChecksumVerifier (src/
    // transfer/ChecksumVerifier.h), which needs to know exactly which
    // item to watch via itemUpdated() rather than inferring it from
    // signal-ordering. Every pre-existing caller ignores the return
    // value, so this is a source-compatible change.
    int enqueue(FilePaneWidget *sourcePane, FilePaneWidget *destPane, const QString &fileName,
                bool assumeOverwrite = false);

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
    // Queued/m_active pipeline: a rename is a single control-
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

    // ChecksumVerifier's (src/transfer/ChecksumVerifier.h) sole entry
    // point into TransferManager: re-downloads a single remote file to a
    // fresh, guaranteed-unique local temp path so its bytes can be hashed
    // a second time, independently of whatever was captured during the
    // original transfer. Deliberately NOT built on top of enqueue() —
    // enqueue() derives its paths from pane->currentDirectory() + a bare
    // filename, which is wrong here: by the time someone clicks "Verify
    // Checksum" on a completed item, the pane may well have navigated
    // elsewhere, and re-deriving the path that way could silently verify
    // the wrong file (or fail to find it) instead of the exact path the
    // original transfer actually used. remotePath is therefore the
    // original item's own already-resolved sourcePath/destPath, passed
    // through as-is. Otherwise identical in shape to startEditDownload()
    // above (same "no destPane, conflict-check skipped, fresh temp
    // path" structure) — reuses TransferDirection::EditDownload rather
    // than introducing a new direction, since every dispatch/pool/claim
    // switch already handles that shape correctly; sets
    // TransferItem::isVerificationTask = true (before itemAdded fires)
    // to disambiguate from a real edit-in-place download, which is what
    // keeps this out of the visible transfer queue and out of
    // EditSessionManager's own routing (keyed by id, not direction alone
    // — a foreign id is a silent, safe no-op there).
    int startVerificationDownload(FilePaneWidget *remotePane, const QString &remotePath,
                                   const QString &fileName);

    // True when both panes have a backend and both report the same,
    // non-empty connectionIdentity() — the precondition for both methods
    // above. Public so MainWindow can check it BEFORE ever calling
    // moveEntry()/moveFolder(), to show its own explanatory message for
    // the ineligible case rather than relying on this class's silent
    // no-op.
    static bool moveEligible(FilePaneWidget *sourcePane, FilePaneWidget *destPane);

    // Cancels by id. If the item is Queued (hasn't started), just marks it
    // Cancelled directly — nothing to interrupt. If it's currently
    // InProgress, calls the executing backend's requestCancel() and
    // remembers that the resulting transferFailed (if any) should be
    // reported as Cancelled rather than Failed — see
    // ActiveTransfer::cancelled. No-op for ids that are already
    // Done/Failed/Cancelled, or don't exist.
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

    // Right-click "Clear Completed"/"Clear Failed" on a tab header
    // (TransferQueueWidget) — hides every id in ids from the visible
    // queue, but ONLY ones whose CURRENT status is already terminal
    // (Done, Failed, Cancelled, or Skipped); any other id is silently
    // skipped rather than acted on. That guard matters because ids are
    // gathered from a specific tab's own current row set at click time,
    // but this method runs after the context menu's own nested event
    // loop (menu.exec()) — the same real "state can change while a
    // modal is open" class of race this project has hit before (see
    // FilePaneWidget::showContextMenu()'s own comment on capturing
    // currentDirectory() before, not after, its menu.exec()) — so a
    // status-changed-in-the-meantime id (e.g. a Failed item's own
    // Retry, chosen from a DIFFERENT window/action between the click and
    // the choice) never gets hidden while genuinely back in flight.
    // Deliberately does NOT remove anything from m_items itself — see
    // TransferItem::clearedFromQueue's own doc comment for why (pending
    // conflict-check maps hold raw m_items indices across a real async
    // round trip; physically compacting the list would silently corrupt
    // those). Emits itemRemoved(id) for each id actually cleared, so
    // TransferQueueWidget can remove exactly those rows without a full
    // rebuild.
    void clearItems(const QList<int> &ids);

    const QList<TransferItem> &items() const { return m_items; }

    // Queue persistence — see TransferQueueStore's own doc comment and
    // ARCHITECTURE.md's TransferQueueStore entry for the full design and
    // its deliberate scope boundaries (clean-shutdown-only,
    // RemoteToRemote/Move excluded, no auto-reconnect).
    //
    // Filters m_items down to direction ∈ {LocalToLocal, LocalToRemote,
    // RemoteToLocal} and status ∈ {Queued, Paused, InProgress} (an
    // InProgress item is written out exactly like a Paused one — its
    // current bytesDone becomes the resume offset, the same mechanism
    // resumeItem() already provides), builds each surviving item's
    // connection descriptors from its pane(s), and writes them via
    // TransferQueueStore::save(). Called from MainWindow::closeEvent(),
    // alongside the existing window-state save.
    void saveQueueForShutdown() const;

    // Loads TransferQueueStore and restores each entry: a LocalToLocal
    // item dispatches immediately against localExecutorPane (both panes
    // always start on LocalBackend, and sourcePath/destPath are already-
    // resolved absolute paths, so which FilePaneWidget object stands in
    // as executor doesn't matter). A LocalToRemote/RemoteToLocal item's
    // local side gets localExecutorPane immediately too; its remote side
    // stays unset and the item's status becomes PendingReconnect until
    // tryReclaimPendingItems() finds a matching connection. Called once
    // from MainWindow's constructor, right after buildLayout() creates
    // both panes.
    void restorePersistedQueue(FilePaneWidget *localExecutorPane);

    // Called whenever a pane's connection succeeds (MainWindow's
    // startConnection(), via its existing RemoteBackend::connected
    // handler) — scans every PendingReconnect item's stored
    // pendingConnection against pane's own connectionDescriptor()
    // (matched by savedSiteId when both have one, else by
    // protocol+host+port+username). On a match, assigns the waiting
    // side (inferred from the item's direction — see
    // TransferItem::pendingConnection's own doc comment), flips status
    // back to Queued/Paused, and calls startNext(). A single call can
    // claim more than one pending item if several share the same
    // connection.
    void tryReclaimPendingItems(FilePaneWidget *pane);

signals:
    void itemAdded(const TransferItem &item);
    void itemUpdated(const TransferItem &item);   // covers both progress and status changes
    // Fired once per id clearItems() actually hides — see that method's
    // own doc comment. Distinct from itemUpdated: this means "stop
    // showing this row at all," not "re-render it," so TransferQueueWidget
    // routes it to TransferQueueTable::removeItem() instead of updateItem().
    void itemRemoved(int id);
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
    // Fired repeatedly during the enumeration phase (see
    // FolderEnumerator::itemsDiscovered's own doc comment) — a real,
    // live-reported UX gap: without this, MainWindow's "Preparing to
    // transfer..." status message never changed between
    // folderTransferStarted and folderTransferFinished, which for a
    // large/deep tree over SFTP/FTP (a real network round trip per
    // directory, strictly serial) could take upwards of ten seconds with
    // zero visible feedback — indistinguishable from a genuine freeze
    // even though the walk was working the whole time.
    void folderTransferProgress(const QString &folderName, int itemsFound);
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
    // against m_pendingMoveItemId (NOT m_active; move items never
    // become an ActiveTransfer entry — see moveEntry()'s own doc comment),
    // since unlike the single-file/single-folder conflict check above,
    // more than one move's backend call could plausibly be in flight
    // at once if the person fires off several "Move Selected" actions
    // in quick succession.
    void onEntryMoved(int requestId);
    void onEntryMoveFailed(const QString &reason, int requestId);

private:
    void startNext();

    // Cheap (bounded by m_active.size(), NOT the queue's size) pre-check
    // used by enqueue() instead of calling startNext() unconditionally
    // for every newly-Queued item — a real O(N²) bug found from a live
    // report, distinct from (and layered on top of) m_scanFloor above:
    // a tight loop of enqueue() calls (any bulk drag-drop or folder
    // transfer) against an already-busy backend meant EVERY call paid
    // for a full startNext() scan that could only ever conclude "still
    // busy" — m_scanFloor doesn't help here, since every one of those
    // items genuinely IS still Queued (not yet resolved), just
    // unable to dispatch. If every backend `item` would need is already
    // claimed AND something is already running, skip the scan for THIS
    // item entirely rather than deferring it — safe because the only
    // event that can make it dispatchable (a backend being released)
    // already re-triggers a full startNext() on its own
    // (releaseActiveTransfer()'s caller), so nothing is ever
    // permanently missed, only the redundant immediate re-scan.
    void startNextIfLikelyToDispatch(const TransferItem &item);

    // Connects a backend's transferProgress/transferFinished/transferFailed/
    // transferPaused signals to this class's four onBackend*() slots —
    // Qt::UniqueConnection, so calling this repeatedly against the same
    // backend (every time an item is dispatched to it) is a harmless
    // no-op, same pattern as ensureExistsCheckConnected()/
    // ensureMoveConnected() below. Deliberately never disconnected:
    // unlike the old single-active-backend design (which had to
    // disconnect the previous backend before connecting the next one, to
    // avoid double-delivery through a single shared m_currentBackend),
    // routing is now done by matching sender() against m_active's
    // currentExecutor fields, so a stale connection left on an idle
    // backend simply has nothing to route when it fires.
    void ensureTransferSignalsConnected(RemoteBackend *backend);

    // The backend instance(s) a Queued item's dispatch needs, computed
    // WITHOUT mutating the item (no tempFilePath allocation, no
    // capturedDestBackend capture — those side effects stay in
    // dispatchActiveItem(), which runs only once startNext() actually
    // commits to this item). Normally a single entry (the executor
    // dispatchActiveItem()'s own switch would pick); RemoteToRemote
    // returns BOTH source and destination backends, even though only one
    // is the executor for phase 1 — see ActiveTransfer::claimedBackends'
    // own doc comment for why both need claiming up front. Returns an
    // empty array only for a genuine internal-error case (should not
    // happen for anything startNext() dispatches).
    QVarLengthArray<RemoteBackend *, 2> requiredBackendsForDispatch(const TransferItem &item) const;

    // Which pane a pool pick should be attempted against for this item,
    // if any — destPane for the upload-shaped directions, sourcePane for
    // the download-shaped ones, nullptr for everything else
    // (RemoteToRemote's two-backend claim and Move are out of scope; see
    // TransferItem::capturedTransferBackend's own doc comment). Shared
    // by startNext()'s actual pool-pick attempt and
    // startNextIfLikelyToDispatch()'s "is a real scan even worth it"
    // pre-check, rather than duplicating this same direction switch in
    // both places — a real, code-review-flagged risk elsewhere in this
    // file already (see requiredBackendsForDispatch()'s own doc comment
    // on the two-switch duplication that used to exist here).
    FilePaneWidget *poolPaneForItem(const TransferItem &item) const;

    // True if any ActiveTransfer entry has already claimed this backend
    // (see ActiveTransfer::claimedBackends) — the busy check startNext()
    // uses to decide whether a Queued item may start now.
    bool isBackendClaimed(RemoteBackend *backend) const;

    // Index into m_active whose currentExecutor matches backend, or -1.
    // Used by the four onBackend*() slots to route a signal (received via
    // sender()) back to the right in-flight item — safe because
    // isBackendClaimed()'s busy check guarantees at most one ActiveTransfer
    // is ever dispatched against a given backend at a time.
    int activeIndexForExecutor(RemoteBackend *backend) const;

    // Index into m_active for the entry tracking itemIndex, or -1.
    int activeIndexForItem(int itemIndex) const;

    // The single point every "this ActiveTransfer entry is no longer
    // running" path goes through — releases m_reservedDestinationKeys'
    // entry (if any) this ActiveTransfer held, THEN removes it from
    // m_active, so a reservation can never outlive the entry that
    // acquired it regardless of which of the several exit paths
    // (finished/failed/cancelled/paused/no-backend-to-dispatch-to) got
    // it there. A no-op (does nothing, including no bounds-check crash)
    // for an already-out-of-range index, matching m_active.removeAt()'s
    // own callers' existing "activeIdx >= 0" guard style.
    void releaseActiveTransfer(int activeIdx);

    int indexById(int id) const;

    // Actually kicks off FolderEnumerator against the source folder —
    // split out from enqueueFolder() so it can be called either
    // immediately (no destination conflict) or after the person resolves
    // a "this folder already exists" prompt as Write Into. rootAlreadyExists
    // distinguishes those two callers for startFolderFileTransfers() below
    // — see that method's own comment on why it matters.
    void startFolderEnumeration(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                                 const QString &folderName, bool rootAlreadyExists);

    // Second phase of enqueueFolder(), called once FolderEnumerator
    // finishes: creates every directory the walk found (dispatched via
    // QMetaObject::invokeMethod(..., Qt::QueuedConnection), which
    // preserves FIFO order per target object — so the destination
    // backend processes them strictly in FolderEnumerator's
    // parent-before-child order regardless of how long each individual
    // creation takes; no need to wait for each one's own completion
    // signal before issuing the next), then hands every file to the
    // ordinary enqueue() above. rootAlreadyExists skips the createDirectory()
    // call for the folder's own root specifically (the first item
    // FolderEnumerator always emits) when the caller already knows it's
    // there (a "Write Into" resolution) — issuing it anyway would always
    // fail with "already exists", and while TransferManager itself
    // doesn't listen for that failure, FilePaneWidget's own
    // fileOperationFailed handling is unconditional and would pop a real,
    // confusing "Create folder failed" error dialog for something that
    // isn't actually an error at all. A fresh (non-conflict) folder
    // transfer still needs this call — nothing else creates the root.
    void startFolderFileTransfers(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                                   const QString &folderName, const QList<EnumeratedItem> &items,
                                   bool rootAlreadyExists);

    // Second half of startNext()'s old body — actually tells the backend
    // to start uploading/downloading. Split out specifically so the file-
    // conflict check (checkExists(), async) can sit in between "an item
    // was picked to run" and "the backend was actually told to run it".
    // Also re-entered directly (not through startNext()) by
    // onBackendFinished() when a RemoteToRemote item transitions from its
    // Downloading phase to its Uploading phase — see that method's own
    // comment. itemIndex must already have a matching m_active entry
    // (created by startNext() before this is first called for the item).
    void dispatchActiveItem(int itemIndex);

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

    // Same idea again, "verify_"-prefixed, for startVerificationDownload()
    // above. Unlike the edit-temp file, this one is short-lived by
    // design — ChecksumVerifier deletes it itself right after hashing it
    // (Done) or as soon as it fails/is cancelled, rather than outliving
    // its own item's terminal status.
    QString allocateVerificationTempFilePath(const TransferItem &item) const;

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
    // connection already exists. Deliberately never disconnected, same
    // reasoning as ensureTransferSignalsConnected() above: existsChecked's
    // requestId already disambiguates which call a given response belongs
    // to, so there's no double-delivery risk to avoid in the first place,
    // and tearing down here would risk losing a response if two different
    // backends both have checks in flight at once (see
    // onDestinationExistsChecked's own doc comment on why that can
    // genuinely happen).
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

    // id -> index into m_items. Safe as a plain incrementally-maintained
    // map specifically because m_items is append-only and its indices are
    // stable for an item's whole lifetime (same invariant ActiveTransfer::
    // itemIndex above already relies on) — every insertion just adds one
    // new entry, nothing already in the map ever needs updating or
    // removing. Populated at every one of the handful of `m_items.append()`
    // call sites. indexById() used to do a linear scan of the WHOLE
    // (never-shrinking) list instead — a real bug found from a live
    // report: cancelItem()/pauseItem()/resumeItem()/retryItem() are one-
    // off user actions, but onEntryMoved()/onEntryMoveFailed() fire once
    // PER FILE for a bulk Move, turning that O(N) scan into O(N²) across
    // a large batch and contributing to a real reported hang.
    QHash<int, int> m_indexById;

    // Lower bound below which no item in m_items is Queued anymore —
    // lets startNext() skip re-scanning an ever-growing prefix of already-
    // resolved items on every single call instead of always starting from
    // index 0. A second real O(N²) source found from the same live hang
    // report: startNext() runs roughly once per dispatch AND once per
    // completion, and m_items only ever grows, so scanning the whole list
    // every time made a large batch's total scanning cost grow with the
    // square of its size. Advanced lazily at the end of startNext() (skip
    // past any leading run of non-Queued items); pulled back down by
    // retryItem()/resumeItem() when either revives an item at an index
    // below the current floor, since m_scanFloor's own invariant ("nothing
    // below this index is Queued") would otherwise be silently violated.
    int m_scanFloor = 0;

    // One entry per item currently claiming a backend — everything from
    // "startNext() committed to this item" (before its conflict check
    // even goes out) through its terminal status. At most one entry may
    // ever have a given backend pointer anywhere in claimedBackends at a
    // time; that invariant (enforced by isBackendClaimed(), checked
    // before any new entry is created) is what makes concurrent dispatch
    // safe — see TransferManager's own class-level doc comment.
    struct ActiveTransfer {
        int itemIndex = -1;   // index into m_items — stable, m_items is append-only, never erased

        // Backend(s) claimed for this item's WHOLE lifetime, not just
        // whichever phase is currently executing. Every direction but
        // RemoteToRemote claims exactly one (its executor).
        // TransferDirection::RemoteToRemote claims BOTH source and
        // destination backends up front, at the very first dispatch
        // (phase == Downloading), even though only one is the current
        // executor at a time (source while Downloading, destination
        // while Uploading) — this closes a real race: without an upfront
        // claim on the destination backend too, a different queued item
        // could grab it in the gap between phase 1 finishing and phase 2
        // dispatching, and phase 2 would then try to invokeMethod() a
        // backend some other ActiveTransfer already has claimed.
        QVarLengthArray<QPointer<RemoteBackend>, 2> claimedBackends;

        // Whichever claimed backend is the one actually executing right
        // now — what onBackend*()'s sender()-based routing (see
        // activeIndexForExecutor()) matches against, and what
        // cancelItem()/pauseItem() call requestCancel()/requestPause()
        // on. QPointer, not a raw pointer, for the same reason the old
        // single m_currentBackend field was: the pane owning this
        // backend can be disconnected/reconnected mid-transfer
        // (FilePaneWidget::setBackend() deleteLater()s the old backend),
        // and a raw pointer would go dangling.
        QPointer<RemoteBackend> currentExecutor;

        // Set by cancelItem() when it cancels this entry's item;
        // onBackendFailed() checks this to report Cancelled instead of
        // Failed, then clears it — same role the old single
        // m_activeItemCancelled scalar had, now one per concurrently-
        // running item so a cancel on one can't mislabel another's
        // genuine failure.
        bool cancelled = false;

        // Live speed sampling for this item specifically — was a single
        // set of class-level scalars (m_speedSampleTimer/
        // m_speedSampleBytesAtLastSample/m_smoothedSpeedBytesPerSec/
        // m_hasSpeedSample) back when only one item could ever be
        // running; now per-entry so two concurrently-progressing items'
        // speeds can't blend into one number. See onBackendProgress()
        // for the recompute-every-~250ms logic and the exponential-
        // moving-average smoothing rationale, unchanged from before.
        QElapsedTimer speedSampleTimer;
        qint64 speedSampleBytesAtLastSample = 0;
        double smoothedSpeedBytesPerSec = 0.0;
        bool hasSpeedSample = false;

        // The key this entry holds in m_reservedDestinationKeys, if any
        // (empty for an item with no destPane, i.e. EditDownload) —
        // stored per-entry so releaseActiveTransfer() can release
        // exactly what THIS entry reserved, from any of its several exit
        // paths, without re-deriving it (the item's own destPane/destPath
        // are still readable at release time too, but keeping this here
        // means release doesn't depend on the item still existing in a
        // particular state). See m_reservedDestinationKeys' own doc
        // comment for what this actually protects against.
        QString reservedDestinationKey;
    };
    QList<ActiveTransfer> m_active;

    // (connectionIdentity, destPath) pairs — joined into one string key,
    // see destinationReservationKey() in the .cpp — currently claimed by
    // some ActiveTransfer entry, for that entry's WHOLE lifetime (from
    // the moment it's claimed in startNext(), through completion/
    // failure/cancellation/pause). Closes a real race isBackendClaimed()
    // alone can't: that only serializes per-backend-*instance*, but two
    // DIFFERENT backend instances connected to the SAME remote server
    // (routine — both panes are often connected to the same host) could
    // otherwise both see a destination checkExists()==false and silently
    // clobber each other. A code review found this real, if narrow, gap
    // — see ARCHITECTURE.md's TransferManager entry for the full story.
    // Deliberately scoped to the ordinary per-file enqueue()/startNext()
    // pipeline only — Move (moveEntry()/moveFolder()) and a folder
    // transfer's own root-level conflict check both already bypass
    // m_active/isBackendClaimed() entirely by design (see moveEntry()'s
    // own doc comment), so neither participates here either; a folder
    // transfer's individual FILES still do, since those ride through
    // this exact same enqueue()/startNext() pipeline like any other file.
    QSet<QString> m_reservedDestinationKeys;

    // Conflict resolution — "remembered" choices reset back to Ask
    // whenever the queue fully drains AND no folder/file root-conflict
    // check is still in flight (see startNext()'s "nothing left to run"
    // path — m_pendingFolderConflictChecks/m_pendingFileConflictChecks
    // must be empty too, not just m_active, or a fast-finishing folder
    // could reset this before a later dropped folder's own still-pending
    // root check gets to read it), so a fresh batch of transfers gets
    // fresh decisions rather than silently inheriting a choice from an
    // unrelated earlier transfer. File and directory conflicts are
    // tracked independently, matching the two separate checkboxes/
    // decisions the person actually gets asked about.
    enum class ConflictResolution { Ask, AlwaysOverwrite, AlwaysSkip };
    ConflictResolution m_fileConflictResolution = ConflictResolution::Ask;
    ConflictResolution m_directoryConflictResolution = ConflictResolution::Ask;

    int m_nextConflictCheckId = 1;

    // Stashed while an ordinary (non-folder, non-Move) file's own
    // destination-conflict check is in flight — a QHash keyed by
    // requestId, NOT a single shared scalar (that was fine when only one
    // item could ever be dispatching at a time; now two items on two
    // different free backends can each have their own file-conflict
    // check in flight from the same startNext() scan). Same pattern
    // m_pendingFolderConflictChecks/m_pendingMoveConflictChecks below
    // already use, and for the identical reason their own doc comments
    // describe.
    QHash<int, int> m_pendingFileConflictChecks;   // requestId -> item index in m_items

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
    // a separate id-space from m_pendingFileConflictChecks/
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
    // ordinary Queued/m_active pipeline entirely). Sharing them was a
    // real bug: a Move batch's "apply to all, Write Into" choice would
    // persist indefinitely and silently apply to a completely unrelated
    // ordinary transfer's conflict later in the session, with no prompt.
    // Reset via maybeResetMoveConflictResolution() once no Move activity
    // remains outstanding.
    ConflictResolution m_moveFileConflictResolution = ConflictResolution::Ask;
    ConflictResolution m_moveDirectoryConflictResolution = ConflictResolution::Ask;

    // Guards askConflict()'s QMessageBox::exec() against a real,
    // reported bug: exec() pumps the WHOLE app's event queue, not just
    // events for its own dialog, so a SECOND, unrelated checkExists()
    // reply already queued at the time it opens (e.g. a second top-level
    // folder dropped in the same batch — enqueueFolder() is called for
    // each dropped folder in one synchronous loop, see
    // m_pendingFolderConflictChecks' own comment) gets delivered and
    // processed WHILE the first dialog is still open. Un-guarded, that
    // second reply saw the resolution still at Ask (the first dialog
    // hadn't been answered yet) and opened its OWN independent dialog,
    // stacked on top of the first — "Write Into/Overwrite asked for
    // every top-level dir" instead of the first answer (even "apply to
    // all") being honored for the rest of the batch, since a dialog that
    // had already opened before that answer was given has no way to see
    // it. Set for the duration of every askConflict() call.
    bool m_conflictDialogInProgress = false;

    // Stashed here instead of being processed immediately whenever
    // onDestinationExistsChecked() is reentered while
    // m_conflictDialogInProgress is set — replayed in original arrival
    // order once the in-progress dialog closes, via
    // drainDeferredExistsChecks(). A replayed call that ALSO needs its
    // own dialog (a genuinely different conflict, not covered by
    // whatever "apply to all" was just chosen) re-enters askConflict()
    // and re-sets the guard for its own duration — so at most one dialog
    // is ever open at a time, sequential rather than stacked, exactly
    // like every other conflict in this codebase already resolves one
    // at a time.
    struct DeferredExistsCheck {
        QString path;
        bool exists = false;
        bool isDir = false;
        int requestId = 0;
    };
    QList<DeferredExistsCheck> m_deferredExistsChecks;

    // See m_deferredExistsChecks' own comment above for what this
    // replays and why. Called once askConflict()'s dialog closes.
    void drainDeferredExistsChecks();

    // Resets m_moveFileConflictResolution/m_moveDirectoryConflictResolution
    // back to Ask once every Move-related bookkeeping structure is empty
    // (no conflict check in flight, no backend call in flight) — the
    // closest Move equivalent to startNext()'s "queue fully drained"
    // reset, since Move has no single queue to drain. Safe to call
    // liberally; a no-op unless both are actually empty.
    void maybeResetMoveConflictResolution();

    // Maps a moveEntry() backend-call request id to the TransferItem::id
    // it belongs to, resolved in onEntryMoved()/onEntryMoveFailed(). Not
    // reusing m_active for this — move items never become an
    // ActiveTransfer entry (see moveEntry()'s doc comment), so several
    // could plausibly have calls in flight at once.
    int m_nextMoveRequestId = 1;
    QHash<int, int> m_pendingMoveItemId;
};
