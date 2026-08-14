#pragma once

#include <QString>
#include <QPointer>
#include "../backends/ConnectionDescriptor.h"

class FilePaneWidget;
class RemoteBackend;

enum class TransferDirection {
    LocalToRemote,
    RemoteToLocal,
    LocalToLocal,
    // Staged through a local temp file (download from source, then upload
    // to destination) — RemoteBackend has no direct server-to-server
    // primitive, so this is the only mechanism possible. See
    // TransferManager::dispatchActiveItem()/onBackendFinished() for the
    // two-phase dispatch, and TransferPhase below for which half of the
    // staging an in-progress item is currently in. Cancel-only for now —
    // pause/resume isn't offered for this direction (see
    // TransferQueueWidget's pauseCapableDirection).
    RemoteToRemote,
    // A server-side rename between two panes on the SAME filesystem/server
    // (RemoteBackend::connectionIdentity() matched) — zero data transfer,
    // a single control-connection round trip. See
    // TransferManager::moveEntry()/moveFolder(). Deliberately a distinct
    // direction from the others, not a special case of LocalToLocal/
    // RemoteToRemote — those always mean "copy" (the source is left in
    // place, confirmed as this app's existing, established behavior even
    // for local-to-local); Move always means the source is gone
    // afterward, a genuinely different operation a person opts into
    // explicitly (the "Move Selected" context-menu action), never a
    // silent substitution for an ordinary Transfer/drag.
    Move,
    // Edit-in-place's two halves — see EditSessionManager (src/ui/
    // EditSessionManager.h/.cpp) and TransferManager::startEditDownload()/
    // startEditUpload(). Unlike every direction above, these are the
    // first where only ONE of sourcePane/destPane is ever set: a
    // download has no destPane (the destination is a fixed local temp
    // path, not another pane's current directory), and an upload has no
    // sourcePane (the source is that same temp path, not a pane
    // selection). dispatchActiveItem() guards its usual
    // sourcePane->backend()/destPane->backend() derefs accordingly.
    EditDownload,   // remote source -> local temp file, for opening in an external editor
    EditUpload,     // local temp file -> original remote path, after a save is detected
    Unsupported   // reserved for a genuine future dispatch failure — no
                  // longer reachable for remote-to-remote specifically
};

// Which half of a RemoteToRemote item's local-temp-file staging is
// currently active. Meaningless (None) for every other direction — kept as
// a separate axis from TransferStatus rather than overloading it, since
// status (Queued/InProgress/...) and phase are genuinely orthogonal: a
// RemoteToRemote item mid-download and mid-upload are both simply
// InProgress at the status level.
enum class TransferPhase {
    None,
    Downloading,   // phase 1: source -> tempFilePath
    Uploading      // phase 2: tempFilePath -> destination
};

enum class TransferStatus {
    Queued,
    InProgress,
    Paused,
    Done,
    Failed,
    Cancelled,
    // Never attempted at all — a destination conflict was resolved as
    // "skip" (either directly, or via a remembered "apply to all"
    // choice from an earlier conflict in the same queue). Deliberately
    // distinct from Cancelled (a person explicitly stopped something
    // already running) and Failed (something went wrong) — this is
    // neither; it's the queue correctly doing what it was told.
    Skipped,
    // Restored from a previous session (see TransferQueueStore/
    // TransferManager::restorePersistedQueue()) but its remote side
    // isn't connected yet — waiting for the user to reconnect a
    // matching pane themselves (TransferManager::tryReclaimPendingItems()),
    // never auto-reconnected. Deliberately distinct from Paused: Paused
    // means a live connection exists and this is simply not the
    // currently-running item; PendingReconnect means no connection
    // exists at all yet. Only ever the direction's remote side (see
    // TransferItem::pendingConnection's own doc comment) — the local
    // side of a restored LocalToRemote/RemoteToLocal item is assigned a
    // real pane immediately on restore, since LocalBackend needs no
    // reconnection.
    PendingReconnect
};

// One row in the transfer queue. Holds the source/dest pane pointers
// directly rather than going through an abstraction — this struct is
// internal to the app, not a public API, and the panes already outlive
// any transfer queued against them in every case that matters here.
//
// KNOWN GAP: if a pane's backend is swapped (Connect/Disconnect) while one
// of its transfers is still queued (not yet started), the queued item will
// execute against whatever backend is attached when its turn comes up —
// not necessarily the one that was active when it was enqueued. Fine for a
// single-user interactive tool, worth flagging if this ever needs to be
// more bulletproof.
struct TransferItem {
    int id = 0;
    QString fileName;
    QString sourcePath;
    QString destPath;
    TransferDirection direction = TransferDirection::Unsupported;
    TransferStatus status = TransferStatus::Queued;
    qint64 bytesDone = 0;   // also serves as the resume offset when status == Paused
    qint64 bytesTotal = 0;
    // Live transfer rate, sampled roughly every 250ms while InProgress
    // (see TransferManager::onBackendProgress) — not a per-chunk
    // instantaneous rate, which would be too noisy to read. 0 outside of
    // an active transfer; not meaningful/updated for Paused or Done.
    qint64 speedBytesPerSec = 0;
    QString errorMessage;

    // Only meaningful for direction == RemoteToRemote — see TransferPhase's
    // own doc comment and TransferManager::allocateTempFilePath()/
    // cleanupTempFile(). Both stay at their defaults for every other
    // direction.
    TransferPhase phase = TransferPhase::None;
    QString tempFilePath;

    // Only meaningful for direction == RemoteToRemote — captured ONCE, at
    // phase 1's own first dispatch (same moment tempFilePath is
    // allocated), and used as-is for phase 2's upload rather than
    // re-fetching destPane->backend() live at that later point. A
    // QPointer, not a raw pointer, specifically so a pane's backend being
    // swapped (Connect/Disconnect via that pane's own path-bar menu, or
    // the toolbar) mid-transfer is something this can detect (the
    // pointer goes null) rather than something that silently redirects
    // an already-running upload to whichever backend happens to be
    // attached by the time phase 2 starts — a real gap only possible
    // once either pane could connect independently mid-session. See
    // TransferManager::dispatchActiveItem()'s RemoteToRemote case and
    // onBackendFinished()'s phase transition.
    QPointer<RemoteBackend> capturedDestBackend;

    // Set by TransferManager::resumeItem(), consumed (and reset to
    // false) by startNext() at the moment this item is actually
    // dispatched. Skips startNext()'s usual destination-conflict
    // checkExists() call for this one dispatch — resuming a paused
    // transfer isn't a fresh start the way a freshly enqueue()'d or
    // retryItem()'d one is; the destination file already legitimately
    // has bytesDone bytes in it (this item's own earlier progress), so
    // re-checking would find "something's already there" and prompt a
    // real Overwrite/Skip conflict for what is, in fact, this exact
    // transfer's own in-progress partial content — not a real conflict
    // at all. A real bug found via live-server verification: every
    // fake-backend test's checkExists() stub unconditionally reports
    // "doesn't exist", so this was structurally impossible to catch
    // without a real backend actually tracking real destination state.
    //
    // Also settable via TransferManager::enqueue()'s own assumeOverwrite
    // parameter — same flag, same consumption point, for a caller (e.g.
    // CompareSyncExecutor) that already knows a destination conflict
    // exists from its own prior work and wants it treated as a settled
    // Overwrite rather than prompting per file.
    bool skipConflictCheckOnDispatch = false;

    FilePaneWidget *sourcePane = nullptr;
    FilePaneWidget *destPane = nullptr;

    // Only meaningful for status == PendingReconnect — the connection
    // this item's remote side is waiting for.
    // TransferManager::tryReclaimPendingItems() matches a newly-
    // connected pane's own ConnectionDescriptor against this one; which
    // pointer (sourcePane for RemoteToLocal, destPane for LocalToRemote)
    // gets filled in on a match is inferred from `direction` alone, not
    // a separate flag — RemoteToRemote/Move/LocalToLocal/EditDownload/
    // EditUpload never reach PendingReconnect in the first place (see
    // TransferManager::saveQueueForShutdown()'s own doc comment on why
    // persistence is scoped to LocalToLocal/LocalToRemote/RemoteToLocal
    // only), so `direction` is always unambiguous here.
    ConnectionDescriptor pendingConnection;
};
