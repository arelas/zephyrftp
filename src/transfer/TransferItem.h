#pragma once

#include <QString>

class FilePaneWidget;

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
    Skipped
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

    FilePaneWidget *sourcePane = nullptr;
    FilePaneWidget *destPane = nullptr;
};
