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
    Done,
    Failed
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
    qint64 bytesDone = 0;
    qint64 bytesTotal = 0;
    QString errorMessage;

    FilePaneWidget *sourcePane = nullptr;
    FilePaneWidget *destPane = nullptr;
};
