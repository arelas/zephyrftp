#pragma once

#include <QString>

class FilePaneWidget;

enum class TransferDirection {
    LocalToRemote,
    RemoteToLocal,
    LocalToLocal,
    Unsupported   // remote-to-remote — not implemented, see TransferManager
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

    FilePaneWidget *sourcePane = nullptr;
    FilePaneWidget *destPane = nullptr;
};
