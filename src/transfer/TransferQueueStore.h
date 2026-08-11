#pragma once

#include <QString>
#include <QList>
#include "TransferItem.h"
#include "../backends/ConnectionDescriptor.h"

// One entry of a persisted, not-yet-restored queue item — deliberately a
// separate, smaller struct from TransferItem, not TransferItem itself:
// most of TransferItem's fields (sourcePane/destPane, capturedDestBackend,
// tempFilePath, skipConflictCheckOnDispatch) are live-only and meaningless
// across a restart, and this struct's whole point is to hold only what
// genuinely needs to survive one.
//
// Scope, enforced by TransferManager::saveQueueForShutdown() before any
// entry here is ever built (see that method's own doc comment for the
// full reasoning): direction is always LocalToLocal, LocalToRemote, or
// RemoteToLocal — never RemoteToRemote, Move, EditDownload/EditUpload,
// or Unsupported. sourceConnection/destConnection use
// ConnectionDescriptor::isEmpty() to mean "this side is local, no
// reconnect needed" (exactly one of the two is non-empty for
// LocalToRemote/RemoteToLocal; both are empty for LocalToLocal).
//
// SECURITY: same non-negotiable ConnectionDescriptor itself already
// carries — no password/passphrase field exists here to write, matching
// SavedSite/sites.json's own established guarantee. See
// queue-persistence-test's raw-JSON key inspection.
struct PersistedTransferItem {
    QString fileName;
    QString sourcePath;
    QString destPath;
    TransferDirection direction = TransferDirection::LocalToLocal;
    qint64 bytesDone = 0;
    qint64 bytesTotal = 0;
    ConnectionDescriptor sourceConnection;
    ConnectionDescriptor destConnection;
};

// Loads/saves the persisted queue as JSON under
// QStandardPaths::AppConfigLocation, alongside sites.json/settings.json —
// same "one persistence convention, one file per concern" pattern this
// app already follows throughout (AppSettings' own doc comment). Written
// only at clean shutdown (MainWindow::closeEvent(), via
// TransferManager::saveQueueForShutdown()) — see this project's
// established, deliberate scope boundary on that (ARCHITECTURE.md's
// TransferQueueStore entry has the full reasoning): not continuous, so a
// crash still loses whatever was in flight, same guarantee window
// notification and dock-state already have.
class TransferQueueStore {
public:
    // Returns an empty list (not an error) if the file doesn't exist yet
    // — expected on first run and on every run where nothing was
    // Queued/Paused/InProgress at the last clean shutdown.
    static QList<PersistedTransferItem> load();

    // Returns false if the file couldn't be written (e.g. permissions).
    // Writing an empty list removes any previous file's stale contents
    // (a shutdown with nothing left to persist shouldn't leave a PREVIOUS
    // run's now-irrelevant queue sitting on disk to be wrongly restored
    // next launch).
    static bool save(const QList<PersistedTransferItem> &items);

private:
    static QString filePath();
};
