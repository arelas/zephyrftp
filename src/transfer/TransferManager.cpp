#include "TransferManager.h"
#include "../ui/FilePaneWidget.h"
#include "../backends/RemoteBackend.h"

#include <QMetaObject>
#include <QMessageBox>
#include <QCheckBox>
#include <QPushButton>
#include <QFileInfo>
#include <QWidget>
#include <QStandardPaths>
#include <QDir>
#include <QFile>

namespace {
QString joinPath(const QString &dir, const QString &name)
{
    return dir.endsWith('/') ? dir + name : dir + '/' + name;
}

// Shared by the constructor's startup sweep and allocateTempFilePath()
// below — previously duplicated verbatim in both places, a real risk if
// this ever needs to change (easy to update one call site and miss the
// other, silently breaking either the cleanup sweep or the staging path
// itself). NOT per-instance or lock-guarded: two app instances running
// at once would both sweep and allocate into this same directory, and
// the second instance's startup sweep would delete the first instance's
// in-flight staging files out from under it. A documented, accepted gap
// — this app has no other multi-instance support either (e.g. sites.json
// writes aren't coordinated across instances) — not attempted here.
QString stagingDirPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/zephyrftp-staging");
}
}

TransferManager::TransferManager(QObject *parent)
    : QObject(parent)
{
    // Best-effort cleanup of a PREVIOUS run's leftover staging files — if
    // the app was closed (or crashed) while a RemoteToRemote item was
    // mid-flight, that item's temp file has no other chance to be deleted
    // (closeEvent() tears down both panes' backends without
    // TransferManager ever getting a completion/failure signal for
    // whatever was still running). The whole zephyrftp-staging/ directory
    // is disposable and per-app, so clearing it wholesale on startup is
    // safe — nothing in it is ever meant to survive a restart, and this
    // instance hasn't allocated anything into it yet. Does NOT address a
    // leak within the SAME run (closing mid-transfer still loses that run's
    // temp file until the next launch) — a documented, accepted gap, not
    // attempted here; see ARCHITECTURE.md's Known Gaps. Synchronous, on
    // the GUI thread, before the main window ever paints — fine in the
    // common case (an empty or small leftover directory), but a genuinely
    // large leftover (e.g. a crash mid-transfer of a huge file) would
    // stall startup for as long as the delete takes; not addressed here
    // either, see ARCHITECTURE.md's Known Gaps.
    QDir(stagingDirPath()).removeRecursively();
}

void TransferManager::enqueue(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                               const QString &fileName)
{
    TransferItem item;
    item.id = m_nextId++;
    item.fileName = fileName;
    item.sourcePane = sourcePane;
    item.destPane = destPane;
    item.sourcePath = joinPath(sourcePane->currentDirectory(), fileName);
    item.destPath = joinPath(destPane->currentDirectory(), fileName);

    const bool srcLocal = sourcePane->backend()->isLocalFilesystem();
    const bool dstLocal = destPane->backend()->isLocalFilesystem();

    if (srcLocal && dstLocal) {
        item.direction = TransferDirection::LocalToLocal;
    } else if (srcLocal && !dstLocal) {
        item.direction = TransferDirection::LocalToRemote;
    } else if (!srcLocal && dstLocal) {
        item.direction = TransferDirection::RemoteToLocal;
    } else {
        // Remote-to-remote: neither backend can do a direct server-to-
        // server transfer, so this stages through a local temp file
        // instead — download from the source first, then upload to the
        // destination (see dispatchActiveItem()/onBackendFinished()).
        // item.status stays the default Queued, same as every other
        // direction, so it flows into the ordinary startNext() call below.
        // tempFilePath is deliberately left unset here — allocated on
        // first dispatch instead, so an item cancelled while still Queued
        // never claims a filename it won't use.
        item.direction = TransferDirection::RemoteToRemote;
        item.phase = TransferPhase::Downloading;
    }

    m_items.append(item);
    emit itemAdded(m_items.last());

    if (item.status == TransferStatus::Queued)
        startNext();
}

void TransferManager::enqueueFolder(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                                     const QString &folderName)
{
    RemoteBackend *dstBackend = destPane->backend();

    // No remote-to-remote guard here (unlike before) — FolderEnumerator
    // only ever talks to the source backend through
    // listDirectoryForEnumeration(), with zero knowledge of
    // isLocalFilesystem(), so a remote source enumerates exactly the same
    // way a local one does; every discovered file then stages through its
    // own local temp file via the ordinary enqueue() call in
    // startFolderFileTransfers() below.

    // Check for a destination conflict on the folder's own root BEFORE
    // paying for a full recursive enumeration of the source — if this is
    // going to be skipped, there's no point walking the source tree
    // first. Nested subdirectories that happen to already exist on the
    // destination are NOT checked individually; "Write Into" here means
    // exactly that — merge into whatever's already there, the same way
    // most file managers handle copying a folder onto an existing one.
    // Only files actually get their own per-item conflict prompt (see
    // startNext()), since asking about every nested subdirectory
    // individually would be far more prompting than this is worth.
    const QString destFolderPath = joinPath(destPane->currentDirectory(), folderName);
    ensureExistsCheckConnected(dstBackend);
    const int requestId = m_nextConflictCheckId++;
    m_pendingFolderConflictChecks.insert(requestId, {sourcePane, destPane, folderName});

    QMetaObject::invokeMethod(dstBackend, "checkExists", Qt::QueuedConnection,
                               Q_ARG(QString, destFolderPath), Q_ARG(int, requestId));
}

bool TransferManager::moveEligible(FilePaneWidget *sourcePane, FilePaneWidget *destPane)
{
    if (!sourcePane || !destPane)
        return false;

    RemoteBackend *srcBackend = sourcePane->backend();
    RemoteBackend *destBackend = destPane->backend();
    if (!srcBackend || !destBackend)
        return false;

    const QString srcIdentity = srcBackend->connectionIdentity();
    return !srcIdentity.isEmpty() && srcIdentity == destBackend->connectionIdentity();
}

void TransferManager::moveEntry(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                                 const QString &fileName)
{
    if (!moveEligible(sourcePane, destPane))
        return;

    RemoteBackend *destBackend = destPane->backend();
    const QString destPath = joinPath(destPane->currentDirectory(), fileName);

    ensureExistsCheckConnected(destBackend);
    const int requestId = m_nextConflictCheckId++;
    m_pendingMoveConflictChecks.insert(requestId, {sourcePane, destPane, fileName, false});

    QMetaObject::invokeMethod(destBackend, "checkExists", Qt::QueuedConnection,
                               Q_ARG(QString, destPath), Q_ARG(int, requestId));
}

void TransferManager::moveFolder(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                                  const QString &folderName)
{
    if (!moveEligible(sourcePane, destPane))
        return;

    RemoteBackend *destBackend = destPane->backend();
    const QString destPath = joinPath(destPane->currentDirectory(), folderName);

    ensureExistsCheckConnected(destBackend);
    const int requestId = m_nextConflictCheckId++;
    m_pendingMoveConflictChecks.insert(requestId, {sourcePane, destPane, folderName, true});

    QMetaObject::invokeMethod(destBackend, "checkExists", Qt::QueuedConnection,
                               Q_ARG(QString, destPath), Q_ARG(int, requestId));
}

int TransferManager::startEditDownload(FilePaneWidget *sourcePane, const QString &fileName)
{
    TransferItem item;
    item.id = m_nextId++;
    item.fileName = fileName;
    item.sourcePane = sourcePane;
    item.destPane = nullptr;   // no destination pane — see TransferDirection::EditDownload's own doc comment
    item.direction = TransferDirection::EditDownload;
    item.sourcePath = joinPath(sourcePane->currentDirectory(), fileName);
    item.destPath = allocateEditTempFilePath(item);
    // A fresh, guaranteed-unique temp path can never conflict with
    // anything — the same reasoning resumeItem() already established
    // for this flag (see TransferItem::skipConflictCheckOnDispatch's own
    // doc comment), reused as-is rather than reimplemented.
    item.skipConflictCheckOnDispatch = true;

    m_items.append(item);
    emit itemAdded(m_items.last());

    if (item.status == TransferStatus::Queued)
        startNext();
    return item.id;
}

int TransferManager::startEditUpload(FilePaneWidget *destPane, const QString &localTempPath,
                                      const QString &remotePath, const QString &fileName)
{
    TransferItem item;
    item.id = m_nextId++;
    item.fileName = fileName;
    item.sourcePane = nullptr;   // no source pane — the source is a fixed local temp path, not a pane selection
    item.destPane = destPane;
    item.direction = TransferDirection::EditUpload;
    item.sourcePath = localTempPath;
    item.destPath = remotePath;
    // Overwriting the file the user was just editing isn't a real
    // conflict — same reasoning as startEditDownload() above.
    item.skipConflictCheckOnDispatch = true;

    m_items.append(item);
    emit itemAdded(m_items.last());

    if (item.status == TransferStatus::Queued)
        startNext();
    return item.id;
}

void TransferManager::saveQueueForShutdown() const
{
    QList<PersistedTransferItem> toSave;
    for (const TransferItem &item : m_items) {
        // Scope boundary, deliberate — see this method's own header doc
        // comment (TransferManager.h) and ARCHITECTURE.md's
        // TransferQueueStore entry for the full reasoning on why
        // RemoteToRemote/Move/EditDownload/EditUpload/Unsupported never
        // reach here, and why a terminal item doesn't either.
        if (item.direction != TransferDirection::LocalToLocal
            && item.direction != TransferDirection::LocalToRemote
            && item.direction != TransferDirection::RemoteToLocal)
            continue;
        if (item.status != TransferStatus::Queued && item.status != TransferStatus::Paused
            && item.status != TransferStatus::InProgress)
            continue;

        PersistedTransferItem persisted;
        persisted.fileName = item.fileName;
        persisted.sourcePath = item.sourcePath;
        persisted.destPath = item.destPath;
        persisted.direction = item.direction;
        persisted.bytesDone = item.bytesDone;
        persisted.bytesTotal = item.bytesTotal;
        if (item.sourcePane)
            persisted.sourceConnection = item.sourcePane->connectionDescriptor();
        if (item.destPane)
            persisted.destConnection = item.destPane->connectionDescriptor();

        // Defensive: a LocalToRemote/RemoteToLocal item's remote side
        // should never actually have an empty descriptor (that pane
        // wouldn't have produced this direction in the first place) —
        // but an item with nothing to reconnect to on restore would be
        // permanently, silently stuck PendingReconnect, so it's skipped
        // here rather than persisted broken.
        if (item.direction == TransferDirection::LocalToRemote && persisted.destConnection.isEmpty())
            continue;
        if (item.direction == TransferDirection::RemoteToLocal && persisted.sourceConnection.isEmpty())
            continue;

        toSave.append(persisted);
    }
    TransferQueueStore::save(toSave);
}

void TransferManager::restorePersistedQueue(FilePaneWidget *localExecutorPane)
{
    const QList<PersistedTransferItem> persisted = TransferQueueStore::load();
    if (persisted.isEmpty())
        return;

    for (const PersistedTransferItem &entry : persisted) {
        TransferItem item;
        item.id = m_nextId++;
        item.fileName = entry.fileName;
        item.sourcePath = entry.sourcePath;
        item.destPath = entry.destPath;
        item.direction = entry.direction;

        switch (entry.direction) {
        case TransferDirection::LocalToLocal:
            // A local copy has no meaningful byte-offset resume
            // (LocalBackend::downloadFile()/uploadFile() ignore
            // resumeOffset entirely — QFile::copy() is always a fresh,
            // atomic, whole-file operation) — carrying over a stale
            // bytesDone/bytesTotal here would just show misleading
            // progress for a moment before the real copy resets it.
            item.sourcePane = localExecutorPane;
            item.destPane = localExecutorPane;
            item.status = TransferStatus::Queued;
            break;
        case TransferDirection::LocalToRemote:
            item.sourcePane = localExecutorPane;
            item.bytesDone = entry.bytesDone;
            item.bytesTotal = entry.bytesTotal;
            item.status = TransferStatus::PendingReconnect;
            item.pendingConnection = entry.destConnection;
            break;
        case TransferDirection::RemoteToLocal:
            item.destPane = localExecutorPane;
            item.bytesDone = entry.bytesDone;
            item.bytesTotal = entry.bytesTotal;
            item.status = TransferStatus::PendingReconnect;
            item.pendingConnection = entry.sourceConnection;
            break;
        default:
            continue;   // TransferQueueStore only ever persists these three directions
        }

        m_items.append(item);
        emit itemAdded(m_items.last());
    }

    startNext();   // picks up any restored LocalToLocal items; a no-op if there are none
}

void TransferManager::tryReclaimPendingItems(FilePaneWidget *pane)
{
    const ConnectionDescriptor paneDescriptor = pane->connectionDescriptor();
    if (paneDescriptor.isEmpty())
        return;   // pane has no real connection (back to LocalBackend, or never connected) -- nothing to reclaim against

    bool claimedAny = false;
    for (TransferItem &item : m_items) {
        if (item.status != TransferStatus::PendingReconnect)
            continue;

        // Matched on the same objective fields RemoteBackend::
        // connectionIdentity() itself is built from — protocol/host/
        // port/username — not on savedSiteId: a reconnect via the plain
        // ConnectionDialog (no site involved) to the exact same server
        // an item's pendingConnection DOES carry a savedSiteId for would
        // otherwise never match, even though it's genuinely the same
        // server. savedSiteId is carried for display purposes only
        // (a friendly name instead of a bare host — see
        // TransferQueueWidget::statusText()), never for matching.
        const ConnectionDescriptor &pending = item.pendingConnection;
        const bool matches = pending.protocol == paneDescriptor.protocol
            && pending.host == paneDescriptor.host
            && pending.port == paneDescriptor.port
            && pending.username == paneDescriptor.username;
        if (!matches)
            continue;

        if (item.direction == TransferDirection::RemoteToLocal)
            item.sourcePane = pane;
        else if (item.direction == TransferDirection::LocalToRemote)
            item.destPane = pane;
        else
            continue;   // unreachable -- only these two directions ever reach PendingReconnect

        item.status = TransferStatus::Queued;
        // Same reasoning resumeItem() already established for this flag
        // (TransferItem::skipConflictCheckOnDispatch's own doc comment):
        // a nonzero bytesDone means the destination already legitimately
        // has this item's own earlier partial content, not a real
        // conflict to prompt about. A restored item that was still at
        // byte 0 gets the normal fresh conflict check, same as any
        // ordinary newly-Queued item.
        item.skipConflictCheckOnDispatch = item.bytesDone > 0;
        claimedAny = true;
        emit itemUpdated(item);
    }

    if (claimedAny)
        startNext();
}

void TransferManager::dispatchMoveEntry(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                                         const QString &name)
{
    RemoteBackend *destBackend = destPane->backend();
    const QString sourcePath = joinPath(sourcePane->currentDirectory(), name);
    const QString destPath = joinPath(destPane->currentDirectory(), name);

    TransferItem item;
    item.id = m_nextId++;
    item.fileName = name;
    item.sourcePane = sourcePane;
    item.destPane = destPane;
    item.sourcePath = sourcePath;
    item.destPath = destPath;
    item.direction = TransferDirection::Move;
    item.status = TransferStatus::InProgress;
    m_items.append(item);
    const int itemId = item.id;
    emit itemAdded(m_items.last());

    ensureMoveConnected(destBackend);
    const int requestId = m_nextMoveRequestId++;
    m_pendingMoveItemId.insert(requestId, itemId);

    QMetaObject::invokeMethod(destBackend, "moveEntry", Qt::QueuedConnection,
                               Q_ARG(QString, sourcePath), Q_ARG(QString, destPath),
                               Q_ARG(int, requestId));
}

void TransferManager::startFolderEnumeration(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                                              const QString &folderName)
{
    RemoteBackend *srcBackend = sourcePane->backend();
    const QString rootPath = joinPath(sourcePane->currentDirectory(), folderName);
    auto *enumerator = new FolderEnumerator(srcBackend, rootPath, folderName, this);

    emit folderTransferStarted(folderName);

    connect(enumerator, &FolderEnumerator::finished, this,
            [this, enumerator, sourcePane, destPane, folderName](const QList<EnumeratedItem> &items) {
        enumerator->deleteLater();
        startFolderFileTransfers(sourcePane, destPane, folderName, items);
    });
    connect(enumerator, &FolderEnumerator::failed, this,
            [this, enumerator, folderName](const QString &reason) {
        enumerator->deleteLater();
        emit folderTransferFailed(folderName, reason);
    });

    enumerator->start();
}

void TransferManager::startFolderFileTransfers(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                                                const QString &folderName, const QList<EnumeratedItem> &items)
{
    RemoteBackend *dstBackend = destPane->backend();

    // Create every directory first, in FolderEnumerator's guaranteed
    // parent-before-child order. Not waited on individually — see this
    // method's doc comment in the header for why the queued-connection
    // FIFO ordering alone is sufficient here. "Already exists" (e.g. the
    // destination happens to already have some of this structure —
    // expected now that Write Into is an explicit, deliberate choice) is
    // not treated as an error for this bulk path — createDirectory()'s
    // fileOperationFailed signal for that case simply isn't listened to
    // here at all, unlike the single right-click "New Folder" action.
    for (const EnumeratedItem &item : items) {
        if (!item.isDir)
            continue;
        const QString destDirPath = joinPath(destPane->currentDirectory(), item.relativePath);
        QMetaObject::invokeMethod(dstBackend, "createDirectory", Qt::QueuedConnection,
                                   Q_ARG(QString, destDirPath));
    }

    // Every file goes through the ordinary enqueue() — its existing
    // joinPath(pane->currentDirectory(), fileName) logic already builds
    // the correct nested path when fileName is actually a relative path
    // like "photos/subdir/photo.jpg", so no separate file-transfer
    // mechanism is needed for folders at all. Each file still gets its
    // own overwrite/skip conflict check individually when its turn comes
    // up in startNext() — Write Into only resolved the TOP-LEVEL
    // folder's own conflict, not every file that might already exist
    // inside it.
    int fileCount = 0;
    for (const EnumeratedItem &item : items) {
        if (item.isDir)
            continue;
        enqueue(sourcePane, destPane, item.relativePath);
        fileCount++;
    }

    // fileCount == 0 is a real, valid outcome (a folder tree containing
    // only empty subdirectories) — not a failure, the directories were
    // still created above, there's just nothing left for the visible
    // transfer queue to do.
    emit folderTransferFinished(folderName, fileCount);
}

void TransferManager::startNext()
{
    // Scans the WHOLE queue on every call, not just the first Queued item
    // found — a busy backend no longer blocks an unrelated item on a
    // free backend from starting in the same pass. An item is claimed
    // (an ActiveTransfer entry appended) the moment this loop commits to
    // it, BEFORE its async conflict check even goes out — same ordering
    // the old single m_activeIndex used, just scoped per-item now,
    // closing the same double-dispatch race: without claiming first, two
    // Queued items for the same backend could both pass the busy check
    // in one scan before either's checkExists() resolves.
    for (int i = 0; i < m_items.size(); ++i) {
        TransferItem &item = m_items[i];
        if (item.status != TransferStatus::Queued)
            continue;

        const QVarLengthArray<RemoteBackend *, 2> required = requiredBackendsForDispatch(item);
        bool anyBusy = false;
        for (RemoteBackend *backend : required) {
            if (isBackendClaimed(backend)) {
                anyBusy = true;
                break;
            }
        }
        if (anyBusy)
            continue;   // try the next Queued item — a different backend might be free

        ActiveTransfer active;
        active.itemIndex = i;
        for (RemoteBackend *backend : required)
            active.claimedBackends.append(backend);
        m_active.append(active);

        // A resumed item's destination conflict check already happened
        // (or was deliberately skipped for a good reason) the first time
        // it was dispatched — see TransferItem::skipConflictCheckOnDispatch's
        // own doc comment for why re-checking here would be actively
        // wrong, not just redundant.
        if (item.skipConflictCheckOnDispatch) {
            item.skipConflictCheckOnDispatch = false;
            dispatchActiveItem(i);
            continue;   // keep scanning — another free backend might have a Queued item too
        }

        // Check the destination for a conflict before doing anything
        // visible (no InProgress status yet, nothing dispatched to a
        // backend) — dispatchActiveItem() only runs once this comes back
        // clean, or the conflict is resolved as Overwrite.
        RemoteBackend *destBackend = item.destPane->backend();
        ensureExistsCheckConnected(destBackend);
        const int requestId = m_nextConflictCheckId++;
        m_pendingFileConflictChecks.insert(requestId, i);
        QMetaObject::invokeMethod(destBackend, "checkExists", Qt::QueuedConnection,
                                   Q_ARG(QString, item.destPath), Q_ARG(int, requestId));
        // keep scanning rather than returning — a different Queued item
        // on a different, still-free backend can start in this same pass
    }

    // Nothing left to run anywhere — a fresh batch of transfers should
    // get fresh conflict decisions rather than silently inheriting a
    // choice from an unrelated earlier transfer. m_active must be empty
    // too, not just "no Queued items": an item can be claimed and mid
    // conflict-check (no ActiveTransfer removed yet) without being
    // Queued anymore.
    if (m_active.isEmpty()) {
        m_fileConflictResolution = ConflictResolution::Ask;
        m_directoryConflictResolution = ConflictResolution::Ask;
    }
}

void TransferManager::dispatchActiveItem(int itemIndex)
{
    if (itemIndex < 0 || itemIndex >= m_items.size())
        return;

    const int activeIdx = activeIndexForItem(itemIndex);
    if (activeIdx < 0)
        return;   // defensive — startNext() always creates the ActiveTransfer entry before calling this
    ActiveTransfer &active = m_active[activeIdx];

    TransferItem &item = m_items[itemIndex];
    item.status = TransferStatus::InProgress;
    item.speedBytesPerSec = 0;
    emit itemUpdated(item);

    active.speedSampleTimer.start();
    active.speedSampleBytesAtLastSample = item.bytesDone;   // nonzero when resuming a Paused item
    active.smoothedSpeedBytesPerSec = 0.0;
    active.hasSpeedSample = false;

    // A plain deref for every direction except EditDownload/EditUpload
    // (see TransferDirection's own doc comment) — those two are the
    // first where only one of sourcePane/destPane is ever set, so both
    // lookups below have to tolerate a null pane rather than assume one
    // like every earlier direction could.
    RemoteBackend *srcBackend = item.sourcePane ? item.sourcePane->backend() : nullptr;
    RemoteBackend *dstBackend = item.destPane ? item.destPane->backend() : nullptr;

    RemoteBackend *executor = nullptr;
    const char *methodName = nullptr;
    QString argA, argB;

    switch (item.direction) {
    case TransferDirection::LocalToRemote:
        // uploadFile(localPath, remotePath) — runs on the remote side's backend
        executor = dstBackend;
        methodName = "uploadFile";
        argA = item.sourcePath;
        argB = item.destPath;
        break;
    case TransferDirection::RemoteToLocal:
        // downloadFile(remotePath, localPath) — runs on the remote side's backend
        executor = srcBackend;
        methodName = "downloadFile";
        argA = item.sourcePath;
        argB = item.destPath;
        break;
    case TransferDirection::LocalToLocal:
        // Either backend is a plain LocalBackend here; uploadFile's
        // (localPath, remotePath) signature just becomes (src, dst)
        // for LocalBackend's QFile::copy-based implementation.
        executor = dstBackend;
        methodName = "uploadFile";
        argA = item.sourcePath;
        argB = item.destPath;
        break;
    case TransferDirection::RemoteToRemote:
        // Staged through a local temp file — allocated once, on first
        // dispatch (empty check below), reused unchanged across the
        // phase transition. Downloading: source -> temp. Uploading:
        // temp -> destination (dispatched again from onBackendFinished()
        // once phase 1 completes, not from here a second time).
        if (item.tempFilePath.isEmpty()) {
            item.tempFilePath = allocateTempFilePath(item);
            // Captured once, here, at phase 1's own first dispatch — NOT
            // re-fetched from item.destPane->backend() at phase 2's
            // dispatch below. See TransferItem::capturedDestBackend's own
            // doc comment for why: a live re-fetch would silently
            // redirect an already-running upload if the destination
            // pane's backend is swapped while phase 1 is still
            // downloading.
            item.capturedDestBackend = dstBackend;
        }
        if (item.phase == TransferPhase::Downloading) {
            executor = srcBackend;
            methodName = "downloadFile";
            argA = item.sourcePath;
            argB = item.tempFilePath;
        } else {   // Uploading
            executor = item.capturedDestBackend;
            methodName = "uploadFile";
            argA = item.tempFilePath;
            argB = item.destPath;
        }
        break;
    case TransferDirection::EditDownload:
        // remote source -> local temp file. No destPane involved — this
        // is opening a file for editing, not a pane-to-pane copy.
        executor = srcBackend;
        methodName = "downloadFile";
        argA = item.sourcePath;
        argB = item.destPath;
        break;
    case TransferDirection::EditUpload:
        // local temp file -> the original remote path. No sourcePane
        // involved — this is the save-triggered re-upload half of
        // edit-in-place, dispatched by EditSessionManager, not a
        // pane-to-pane copy either.
        executor = dstBackend;
        methodName = "uploadFile";
        argA = item.sourcePath;
        argB = item.destPath;
        break;
    case TransferDirection::Unsupported:
        // Shouldn't reach here — no direction is ever set to Unsupported
        // anymore (enqueue() now handles remote-to-remote via
        // RemoteToRemote above); reserved for a genuine future dispatch
        // failure.
        break;
    }

    if (!executor) {
        item.status = TransferStatus::Failed;
        // A specific, honest message for the one case this is actually
        // expected to happen in practice: item.capturedDestBackend went
        // null because the destination pane's connection changed while
        // phase 1 was still downloading (see TransferItem's own doc
        // comment) — everything else reaching here would be a genuine
        // internal bug.
        item.errorMessage = (item.direction == TransferDirection::RemoteToRemote
                              && item.phase == TransferPhase::Uploading)
            ? tr("The destination pane's connection changed while this transfer was staging — "
                 "cancelled rather than uploading to the wrong place.")
            : tr("Internal error: no backend to execute this transfer");
        cleanupTempFile(item);   // no-op unless phase 1 already downloaded to a temp file (the RemoteToRemote case above)
        emit itemUpdated(item);
        m_active.removeAt(activeIdx);
        startNext();   // try the next queued item instead of getting stuck
        return;
    }

    active.currentExecutor = executor;
    ensureTransferSignalsConnected(executor);
    // item.bytesDone doubles as the resume offset — 0 for a fresh
    // item, nonzero when re-starting a previously Paused one (see
    // resumeItem(), which deliberately does NOT reset bytesDone the
    // way retryItem() resets it to 0).
    QMetaObject::invokeMethod(executor, methodName, Qt::QueuedConnection,
                               Q_ARG(QString, argA), Q_ARG(QString, argB),
                               Q_ARG(qint64, item.bytesDone));
}

QString TransferManager::allocateTempFilePath(const TransferItem &item) const
{
    // A dedicated subdirectory rather than writing loose files straight
    // into the OS temp dir — keeps this app's own staging files visually
    // grouped (useful for debugging a stuck/crashed transfer) and easy to
    // sweep as a unit. Disposable: nothing here needs to survive a restart.
    const QString stagingDir = stagingDirPath();
    QDir().mkpath(stagingDir);

    // The item's own unique id prevents any collision between
    // concurrently-queued RemoteToRemote items — including two that are
    // now genuinely executing at the same time, on two different backend
    // pairs; the original filename stays visible alongside it for anyone
    // inspecting the staging directory mid-transfer or after a crash.
    return stagingDir + QStringLiteral("/%1_%2")
        .arg(item.id)
        .arg(QFileInfo(item.fileName).fileName());
}

QString TransferManager::allocateEditTempFilePath(const TransferItem &item) const
{
    const QString stagingDir = stagingDirPath();
    QDir().mkpath(stagingDir);

    // "edit_" prefix visually distinguishes this from a RemoteToRemote
    // staging file in the same shared directory — both get swept
    // identically by the constructor's startup sweep above, but this
    // one deliberately outlives its own item's Done status (the user is
    // about to start editing it); see allocateTempFilePath()'s own
    // comment for the id-collision reasoning, which applies identically
    // here.
    return stagingDir + QStringLiteral("/edit_%1_%2")
        .arg(item.id)
        .arg(QFileInfo(item.fileName).fileName());
}

void TransferManager::cleanupTempFile(TransferItem &item)
{
    if (item.direction != TransferDirection::RemoteToRemote || item.tempFilePath.isEmpty())
        return;

    QFile::remove(item.tempFilePath);   // safe no-op if it was never actually created
    item.tempFilePath.clear();
}

void TransferManager::cancelItem(int id)
{
    const int idx = indexById(id);
    if (idx < 0)
        return;

    TransferItem &item = m_items[idx];

    const int activeIdx = activeIndexForItem(idx);
    if (activeIdx >= 0) {
        ActiveTransfer &active = m_active[activeIdx];
        if (active.currentExecutor) {
            // Currently running — ask the executing backend to stop, and
            // remember to report the resulting transferFailed as Cancelled
            // rather than Failed once it arrives.
            active.cancelled = true;
            active.currentExecutor->requestCancel();
        } else {
            // A real bug: currentExecutor (a QPointer) can go null if the
            // active item's backend is destroyed/swapped mid-transfer —
            // e.g. Disconnect on a pane with a transfer running against
            // it. There's no backend left to ask to stop, and no
            // transferFailed/transferPaused signal is ever coming to
            // resolve this item — without handling this case, the item
            // AND its ActiveTransfer entry would stay stuck InProgress
            // forever, permanently blocking any other Queued item that
            // needs one of its claimedBackends. Resolve it directly
            // instead of waiting for a response that can't arrive, the
            // same terminal state onBackendFailed() would have produced
            // for a cancel.
            item.status = TransferStatus::Cancelled;
            item.speedBytesPerSec = 0;
            cleanupTempFile(item);
            emit itemUpdated(item);
            m_active.removeAt(activeIdx);
            startNext();
        }
        return;
    }

    if (item.status == TransferStatus::Queued || item.status == TransferStatus::Paused
        || item.status == TransferStatus::PendingReconnect) {
        // None of the three is actively running — Queued never started,
        // Paused already stopped and has no ActiveTransfer entry anymore
        // (removed when it paused), PendingReconnect has no backend at
        // all yet — so there's nothing to interrupt, just mark it done.
        item.status = TransferStatus::Cancelled;
        emit itemUpdated(item);
    }
    // Done/Failed/Cancelled items: no-op, nothing meaningful to cancel.
}

void TransferManager::retryItem(int id)
{
    const int idx = indexById(id);
    if (idx < 0)
        return;

    TransferItem &item = m_items[idx];
    if (item.status != TransferStatus::Failed && item.status != TransferStatus::Cancelled
        && item.status != TransferStatus::Skipped)
        return;

    // A Move item never becomes an ActiveTransfer entry and never
    // reaches this Queued/m_active pipeline in the first place (see
    // moveEntry()'s own doc comment) — re-queuing one here would hit
    // dispatchActiveItem(), which has no case for TransferDirection::Move
    // and would fail it again with a misleading generic "no backend to
    // execute this transfer" error. TransferQueueWidget already disables
    // the Retry action for Move items for exactly this reason; this
    // guard enforces the same invariant at the model layer too, rather
    // than relying solely on the UI never calling this for one.
    if (item.direction == TransferDirection::Move)
        return;

    item.status = TransferStatus::Queued;
    item.bytesDone = 0;
    item.bytesTotal = 0;
    item.errorMessage.clear();
    // A real bug: resumeItem() sets skipConflictCheckOnDispatch so a
    // resumed transfer doesn't get a spurious conflict prompt against its
    // own partial content (see TransferItem.h's own comment on the flag).
    // If that item is then cancelled while still Queued (never dispatched,
    // so the flag never gets consumed/cleared) and retried instead of
    // resumed, retryItem() genuinely restarts from byte 0 — exactly the
    // "fresh, worth-asking-about conflict" case the flag exists to skip
    // ONLY for a resume. Without this, a stale skipConflictCheckOnDispatch
    // from an earlier pause/resume could silently bypass this retry's
    // conflict check and overwrite whatever now exists at the destination.
    item.skipConflictCheckOnDispatch = false;
    // A RemoteToRemote item must restart from phase 1, not resume wherever
    // it failed — its temp file was already deleted by onBackendFailed()'s
    // cleanupTempFile() call, so re-entering at phase == Uploading would
    // try to upload a file that no longer exists. Resetting phase back to
    // Downloading and clearing tempFilePath makes dispatchActiveItem()
    // allocate a fresh temp file and start over, consistent with how every
    // other direction's retry also restarts fully from byte 0.
    if (item.direction == TransferDirection::RemoteToRemote) {
        item.phase = TransferPhase::Downloading;
        item.tempFilePath.clear();
    }
    emit itemUpdated(item);

    startNext();
}

void TransferManager::pauseItem(int id)
{
    const int idx = indexById(id);
    if (idx < 0)
        return;
    const int activeIdx = activeIndexForItem(idx);
    if (activeIdx < 0)
        return;   // only a currently-claimed (running or mid-conflict-check) item can be paused

    const ActiveTransfer &active = m_active[activeIdx];
    if (active.currentExecutor)
        active.currentExecutor->requestPause();
    // No status change here — onBackendPaused() makes that transition
    // once the backend actually confirms it stopped. Setting Paused
    // eagerly here would show a status the transfer hasn't reached yet.
}

void TransferManager::resumeItem(int id)
{
    const int idx = indexById(id);
    if (idx < 0)
        return;

    TransferItem &item = m_items[idx];
    if (item.status != TransferStatus::Paused)
        return;

    // Deliberately NOT resetting bytesDone — that's the resume offset
    // startNext() will pass through to the backend. This is the one
    // place this differs from retryItem().
    item.status = TransferStatus::Queued;
    // See TransferItem::skipConflictCheckOnDispatch's own doc comment —
    // this item's destination already legitimately has its own partial
    // content sitting there; re-checking on resume would find that and
    // show a real (spurious) conflict prompt. retryItem() does NOT set
    // this: a retry genuinely restarts from byte 0 (bytesDone reset to
    // 0, and for RemoteToRemote, phase/tempFilePath reset too), so
    // whatever's at the destination really is a fresh, worth-asking-
    // about conflict there, same as any other new dispatch.
    item.skipConflictCheckOnDispatch = true;
    emit itemUpdated(item);

    startNext();
}

void TransferManager::ensureTransferSignalsConnected(RemoteBackend *backend)
{
    // Qt::UniqueConnection — safe to call every time an item is
    // dispatched to this backend, even repeatedly across many transfers
    // over the backend's lifetime. Routing no longer depends on "whichever
    // backend is currently THE one" (there can be several at once now);
    // the four onBackend*() slots below resolve sender() against
    // m_active's currentExecutor fields instead, so a connection left on
    // an idle backend simply has nothing to route when (if ever) it fires.
    connect(backend, &RemoteBackend::transferProgress, this, &TransferManager::onBackendProgress,
            Qt::UniqueConnection);
    connect(backend, &RemoteBackend::transferFinished, this, &TransferManager::onBackendFinished,
            Qt::UniqueConnection);
    connect(backend, &RemoteBackend::transferFailed, this, &TransferManager::onBackendFailed,
            Qt::UniqueConnection);
    connect(backend, &RemoteBackend::transferPaused, this, &TransferManager::onBackendPaused,
            Qt::UniqueConnection);
}

QVarLengthArray<RemoteBackend *, 2> TransferManager::requiredBackendsForDispatch(const TransferItem &item) const
{
    QVarLengthArray<RemoteBackend *, 2> result;

    RemoteBackend *srcBackend = item.sourcePane ? item.sourcePane->backend() : nullptr;
    RemoteBackend *dstBackend = item.destPane ? item.destPane->backend() : nullptr;

    switch (item.direction) {
    case TransferDirection::LocalToRemote:
    case TransferDirection::LocalToLocal:
    case TransferDirection::EditUpload:
        if (dstBackend)
            result.append(dstBackend);
        break;
    case TransferDirection::RemoteToLocal:
    case TransferDirection::EditDownload:
        if (srcBackend)
            result.append(srcBackend);
        break;
    case TransferDirection::RemoteToRemote:
        // Both backends claimed up front, for the item's whole two-phase
        // lifetime — see ActiveTransfer::claimedBackends' own doc comment
        // for why. A freshly-Queued (or retried) RemoteToRemote item is
        // always phase == Downloading; the phase-2 re-dispatch from
        // onBackendFinished() doesn't go through startNext()/this helper
        // at all, so phase is never Uploading here.
        if (srcBackend)
            result.append(srcBackend);
        if (dstBackend && dstBackend != srcBackend)
            result.append(dstBackend);
        break;
    case TransferDirection::Move:
    case TransferDirection::Unsupported:
        // Move never reaches startNext() (see moveEntry()'s doc comment);
        // Unsupported is never assigned. Defensive only.
        break;
    }

    return result;
}

bool TransferManager::isBackendClaimed(RemoteBackend *backend) const
{
    if (!backend)
        return false;
    for (const ActiveTransfer &active : m_active) {
        if (active.claimedBackends.contains(backend))
            return true;
    }
    return false;
}

int TransferManager::activeIndexForExecutor(RemoteBackend *backend) const
{
    if (!backend)
        return -1;
    for (int i = 0; i < m_active.size(); ++i) {
        if (m_active[i].currentExecutor == backend)
            return i;
    }
    return -1;
}

int TransferManager::activeIndexForItem(int itemIndex) const
{
    for (int i = 0; i < m_active.size(); ++i) {
        if (m_active[i].itemIndex == itemIndex)
            return i;
    }
    return -1;
}

void TransferManager::ensureExistsCheckConnected(RemoteBackend *backend)
{
    connect(backend, &RemoteBackend::existsChecked, this, &TransferManager::onDestinationExistsChecked,
            Qt::UniqueConnection);
}

void TransferManager::ensureMoveConnected(RemoteBackend *backend)
{
    connect(backend, &RemoteBackend::entryMoved, this, &TransferManager::onEntryMoved,
            Qt::UniqueConnection);
    connect(backend, &RemoteBackend::entryMoveFailed, this, &TransferManager::onEntryMoveFailed,
            Qt::UniqueConnection);
}

bool TransferManager::askConflict(const QString &name, bool isDirectory, bool &applyToAll)
{
    QMessageBox box(qobject_cast<QWidget *>(parent()));
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(isDirectory ? tr("Folder Already Exists") : tr("File Already Exists"));
    box.setText(isDirectory
        ? tr("A folder named \"%1\" already exists at the destination.").arg(name)
        : tr("A file named \"%1\" already exists at the destination.").arg(name));

    QPushButton *proceedButton = box.addButton(
        isDirectory ? tr("Write Into") : tr("Overwrite"), QMessageBox::AcceptRole);
    QPushButton *skipButton = box.addButton(tr("Skip"), QMessageBox::RejectRole);
    box.setDefaultButton(skipButton);   // the safer choice if Enter is pressed without reading

    auto *checkbox = new QCheckBox(
        isDirectory ? tr("Do this for all remaining folder conflicts in this transfer")
                    : tr("Do this for all remaining file conflicts in this transfer"));
    box.setCheckBox(checkbox);

    box.exec();

    applyToAll = checkbox->isChecked();
    return box.clickedButton() == proceedButton;
}

void TransferManager::onDestinationExistsChecked(const QString &path, bool exists, bool isDir, int requestId)
{
    Q_UNUSED(path);
    Q_UNUSED(isDir);   // a genuine type mismatch (e.g. uploading a file where a folder of that
                       // name already exists) isn't specifically detected here — it surfaces as
                       // a normal backend error when the transfer is actually attempted, same as
                       // it always has, rather than trying to guess the right resolution for a
                       // case Overwrite/Skip doesn't really describe anyway

    const auto folderConflictIt = m_pendingFolderConflictChecks.find(requestId);
    if (folderConflictIt != m_pendingFolderConflictChecks.end()) {
        const PendingFolderConflictCheck pending = folderConflictIt.value();
        m_pendingFolderConflictChecks.erase(folderConflictIt);
        FilePaneWidget *sourcePane = pending.sourcePane;
        FilePaneWidget *destPane = pending.destPane;
        const QString folderName = pending.folderName;

        if (!exists) {
            startFolderEnumeration(sourcePane, destPane, folderName);
            return;
        }

        bool proceed;
        if (m_directoryConflictResolution == ConflictResolution::AlwaysOverwrite) {
            proceed = true;
        } else if (m_directoryConflictResolution == ConflictResolution::AlwaysSkip) {
            proceed = false;
        } else {
            bool applyToAll = false;
            proceed = askConflict(folderName, /*isDirectory=*/true, applyToAll);
            if (applyToAll) {
                m_directoryConflictResolution =
                    proceed ? ConflictResolution::AlwaysOverwrite : ConflictResolution::AlwaysSkip;
            }
        }

        if (proceed)
            startFolderEnumeration(sourcePane, destPane, folderName);
        else
            emit folderTransferSkipped(folderName);
        return;
    }

    const auto moveConflictIt = m_pendingMoveConflictChecks.find(requestId);
    if (moveConflictIt != m_pendingMoveConflictChecks.end()) {
        const PendingMoveConflictCheck pending = moveConflictIt.value();
        m_pendingMoveConflictChecks.erase(moveConflictIt);
        FilePaneWidget *sourcePane = pending.sourcePane;
        FilePaneWidget *destPane = pending.destPane;
        const QString name = pending.name;
        const bool isFolder = pending.isFolder;

        if (!exists) {
            dispatchMoveEntry(sourcePane, destPane, name);
            return;
        }

        bool proceed;
        // Move's OWN resolution state (m_moveFileConflictResolution/
        // m_moveDirectoryConflictResolution) — NOT the ordinary transfer
        // pipeline's m_fileConflictResolution/m_directoryConflictResolution;
        // see TransferManager.h's doc comment on why sharing them was a
        // real bug (a Move's "apply to all" choice leaking into an
        // unrelated later ordinary transfer with no prompt).
        ConflictResolution &resolution =
            isFolder ? m_moveDirectoryConflictResolution : m_moveFileConflictResolution;
        if (resolution == ConflictResolution::AlwaysOverwrite) {
            proceed = true;
        } else if (resolution == ConflictResolution::AlwaysSkip) {
            proceed = false;
        } else {
            bool applyToAll = false;
            proceed = askConflict(name, isFolder, applyToAll);
            if (applyToAll)
                resolution = proceed ? ConflictResolution::AlwaysOverwrite : ConflictResolution::AlwaysSkip;
        }

        if (!proceed) {
            // A move that's skipped at the conflict prompt never gets a
            // queue row at all — there's nothing to show for it, unlike
            // an ordinary transfer's Skipped status, which exists
            // because that item was already visible mid-batch. Folders
            // still get folderTransferSkipped for consistency with
            // enqueueFolder()'s own skip notification.
            if (isFolder)
                emit folderTransferSkipped(name);
            maybeResetMoveConflictResolution();
            return;
        }

        if (isFolder) {
            // "Write Into" was chosen, but a single rename() cannot
            // merge into a non-empty existing directory (POSIX
            // ENOTEMPTY semantics — see moveFolder()'s doc comment).
            // Report a clear failure through the same visible queue the
            // successful case uses, rather than attempting a doomed
            // rename or silently doing nothing.
            TransferItem item;
            item.id = m_nextId++;
            item.fileName = name;
            item.sourcePane = sourcePane;
            item.destPane = destPane;
            item.sourcePath = joinPath(sourcePane->currentDirectory(), name);
            item.destPath = joinPath(destPane->currentDirectory(), name);
            item.direction = TransferDirection::Move;
            item.status = TransferStatus::Failed;
            item.errorMessage = tr("Can't move into \"%1\": a folder with that name already exists "
                                    "at the destination, and Move can't merge folders.").arg(name);
            m_items.append(item);
            emit itemAdded(m_items.last());
            maybeResetMoveConflictResolution();
            return;
        }

        dispatchMoveEntry(sourcePane, destPane, name);
        return;
    }

    const auto fileConflictIt = m_pendingFileConflictChecks.find(requestId);
    if (fileConflictIt != m_pendingFileConflictChecks.end()) {
        const int itemIndex = fileConflictIt.value();
        m_pendingFileConflictChecks.erase(fileConflictIt);
        if (itemIndex < 0 || itemIndex >= m_items.size())
            return;   // defensive, shouldn't happen
        if (activeIndexForItem(itemIndex) < 0)
            return;   // item was cancelled while this check was in flight — its ActiveTransfer is already gone

        TransferItem &item = m_items[itemIndex];

        if (!exists) {
            dispatchActiveItem(itemIndex);
            return;
        }

        bool proceed;
        if (m_fileConflictResolution == ConflictResolution::AlwaysOverwrite) {
            proceed = true;
        } else if (m_fileConflictResolution == ConflictResolution::AlwaysSkip) {
            proceed = false;
        } else {
            bool applyToAll = false;
            // askConflict()'s QMessageBox::exec() pumps the event loop —
            // another concurrently-active item can finish/fail/pause and
            // be removed from m_active while this is up, which would
            // shift indices for anything captured beforehand. Nothing
            // here is captured across this call for that reason;
            // activeIndexForItem(itemIndex) below is looked up fresh.
            proceed = askConflict(QFileInfo(item.destPath).fileName(), /*isDirectory=*/false, applyToAll);
            if (applyToAll) {
                m_fileConflictResolution =
                    proceed ? ConflictResolution::AlwaysOverwrite : ConflictResolution::AlwaysSkip;
            }
        }

        if (proceed) {
            dispatchActiveItem(itemIndex);
        } else {
            item.status = TransferStatus::Skipped;
            item.speedBytesPerSec = 0;
            emit itemUpdated(item);
            const int activeIdx = activeIndexForItem(itemIndex);
            if (activeIdx >= 0)
                m_active.removeAt(activeIdx);
            startNext();
        }
        return;
    }

    // requestId matches neither pending check — a stale/unrelated
    // response (e.g. from a check that's since been superseded). Nothing
    // to do; not an error, just ignored.
}

void TransferManager::onBackendProgress(const QString &fileName, qint64 bytesDone, qint64 bytesTotal)
{
    Q_UNUSED(fileName);   // routed by sender() below, not by name
    auto *backend = qobject_cast<RemoteBackend *>(sender());
    const int activeIdx = activeIndexForExecutor(backend);
    if (activeIdx < 0)
        return;   // no ActiveTransfer currently executing on this backend — stale/unrelated, ignore

    ActiveTransfer &active = m_active[activeIdx];
    TransferItem &item = m_items[active.itemIndex];
    item.bytesDone = bytesDone;
    item.bytesTotal = bytesTotal;

    // Recompute speed roughly every 250ms rather than on every single
    // progress signal — SFTP's read/write loop emits one per 32KB chunk,
    // which on a fast connection would be far too frequent to be a
    // meaningful "live" number rather than noise.
    const qint64 elapsedMs = active.speedSampleTimer.isValid() ? active.speedSampleTimer.elapsed() : 0;
    if (elapsedMs >= 250) {
        const qint64 bytesSinceLastSample = bytesDone - active.speedSampleBytesAtLastSample;
        const double rawSpeed = (bytesSinceLastSample * 1000.0) / elapsedMs;

        // Exponential moving average across samples — see
        // ActiveTransfer::smoothedSpeedBytesPerSec's own doc comment for
        // why. alpha = 0.3 is a fairly standard middle ground (e.g. in
        // the same range curl and several download managers use for
        // their own live rate display): responsive enough to reflect a
        // real speed change within a couple of seconds, calm enough that
        // single-window noise doesn't dominate what's shown. The very
        // first sample after a (re)start has nothing to blend with, so
        // it's taken as-is rather than artificially damped toward zero.
        if (!active.hasSpeedSample) {
            active.smoothedSpeedBytesPerSec = rawSpeed;
            active.hasSpeedSample = true;
        } else {
            constexpr double kSmoothingAlpha = 0.3;
            active.smoothedSpeedBytesPerSec = kSmoothingAlpha * rawSpeed
                + (1.0 - kSmoothingAlpha) * active.smoothedSpeedBytesPerSec;
        }
        item.speedBytesPerSec = static_cast<qint64>(active.smoothedSpeedBytesPerSec);
        active.speedSampleBytesAtLastSample = bytesDone;
        active.speedSampleTimer.restart();
    }

    emit itemUpdated(item);
}

void TransferManager::onBackendFinished(const QString &fileName)
{
    Q_UNUSED(fileName);
    auto *backend = qobject_cast<RemoteBackend *>(sender());
    const int activeIdx = activeIndexForExecutor(backend);
    if (activeIdx < 0)
        return;

    const int itemIndex = m_active[activeIdx].itemIndex;
    TransferItem &item = m_items[itemIndex];

    // A RemoteToRemote item's phase-1 (download-to-temp) completion isn't
    // the item finishing — it's the cue to start phase 2 (upload-from-
    // temp). Re-enters dispatchActiveItem() directly (not through
    // startNext()/checkExists() — the destination conflict check already
    // happened once for this item, no reason to ask again) after
    // re-pointing this ActiveTransfer's currentExecutor at the
    // destination backend. bytesDone/bytesTotal are zeroed here (not
    // inside dispatchActiveItem() itself) since that's the signal
    // dispatchActiveItem()'s own speed-sample reset reads from. The
    // SAME ActiveTransfer entry carries over — it already claimed both
    // backends up front (see ActiveTransfer::claimedBackends' own doc
    // comment), so no new busy check is needed here.
    if (item.direction == TransferDirection::RemoteToRemote
        && item.phase == TransferPhase::Downloading) {
        item.phase = TransferPhase::Uploading;
        item.bytesDone = 0;
        item.bytesTotal = 0;
        item.speedBytesPerSec = 0;
        emit itemUpdated(item);   // lets the UI show "Uploading" before phase 2's first progress tick

        // item.capturedDestBackend, NOT item.destPane->backend() —
        // see TransferItem's own doc comment on why. If it's gone null
        // (the destination pane's backend was swapped mid-download),
        // skip connecting to anything: dispatchActiveItem()'s own
        // executor-null check below reports it as a clear failure
        // instead of connecting signals to a backend that's about to be
        // silently wrong.
        if (item.capturedDestBackend)
            ensureTransferSignalsConnected(item.capturedDestBackend);
        dispatchActiveItem(itemIndex);
        return;
    }

    item.status = TransferStatus::Done;
    item.bytesDone = item.bytesTotal > 0 ? item.bytesTotal : item.bytesDone;
    item.speedBytesPerSec = 0;   // not meaningful once finished — avoid showing a stale number
    cleanupTempFile(item);   // no-op for every direction except a just-finished RemoteToRemote upload
    emit itemUpdated(item);
    emit transferSucceeded();

    m_active.removeAt(activeIdx);
    startNext();
}

void TransferManager::onBackendFailed(const QString &fileName, const QString &reason)
{
    Q_UNUSED(fileName);
    auto *backend = qobject_cast<RemoteBackend *>(sender());
    const int activeIdx = activeIndexForExecutor(backend);
    if (activeIdx < 0)
        return;

    ActiveTransfer &active = m_active[activeIdx];
    TransferItem &item = m_items[active.itemIndex];
    item.status = active.cancelled ? TransferStatus::Cancelled : TransferStatus::Failed;
    item.errorMessage = active.cancelled ? QString() : reason;
    item.speedBytesPerSec = 0;
    // Covers a RemoteToRemote item's phase-1 failure, phase-2 failure
    // (deleting the already-downloaded temp file too), and cancellation
    // (which surfaces through this same path) — one call handles all
    // three, no separate handling needed. No-op for every other direction.
    cleanupTempFile(item);
    emit itemUpdated(item);

    m_active.removeAt(activeIdx);
    startNext();
}

void TransferManager::onBackendPaused(const QString &fileName, qint64 bytesDone)
{
    Q_UNUSED(fileName);
    auto *backend = qobject_cast<RemoteBackend *>(sender());
    const int activeIdx = activeIndexForExecutor(backend);
    if (activeIdx < 0)
        return;

    ActiveTransfer &active = m_active[activeIdx];
    TransferItem &item = m_items[active.itemIndex];
    item.status = TransferStatus::Paused;
    item.bytesDone = bytesDone;   // becomes the resume offset the next time this item runs
    item.speedBytesPerSec = 0;
    // A cancel racing a near-simultaneous pause (cancelItem() sets
    // active.cancelled and calls requestCancel(), but the backend reports
    // transferPaused instead of transferFailed before the cancel takes
    // effect) resolves this item to Paused either way — active.cancelled
    // is discarded along with the rest of this entry below, rather than
    // surviving to mislabel some later, unrelated item's genuine failure
    // as Cancelled. A real bug in the old single-class-scalar design
    // (m_activeItemCancelled never got reset on this path) that this
    // per-entry design closes structurally: there's no shared flag left
    // to leak from one item into the next.
    emit itemUpdated(item);

    m_active.removeAt(activeIdx);
    startNext();
}

void TransferManager::onEntryMoved(int requestId)
{
    if (!m_pendingMoveItemId.contains(requestId))
        return;   // stale/unrelated response

    const int itemId = m_pendingMoveItemId.take(requestId);
    maybeResetMoveConflictResolution();
    const int idx = indexById(itemId);
    if (idx < 0)
        return;

    TransferItem &item = m_items[idx];
    item.status = TransferStatus::Done;
    emit itemUpdated(item);
    emit transferSucceeded();
}

void TransferManager::onEntryMoveFailed(const QString &reason, int requestId)
{
    if (!m_pendingMoveItemId.contains(requestId))
        return;

    const int itemId = m_pendingMoveItemId.take(requestId);
    maybeResetMoveConflictResolution();
    const int idx = indexById(itemId);
    if (idx < 0)
        return;

    TransferItem &item = m_items[idx];
    item.status = TransferStatus::Failed;
    item.errorMessage = reason;
    emit itemUpdated(item);
}

void TransferManager::maybeResetMoveConflictResolution()
{
    if (m_pendingMoveConflictChecks.isEmpty() && m_pendingMoveItemId.isEmpty()) {
        m_moveFileConflictResolution = ConflictResolution::Ask;
        m_moveDirectoryConflictResolution = ConflictResolution::Ask;
    }
}

int TransferManager::indexById(int id) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id == id)
            return i;
    }
    return -1;
}
