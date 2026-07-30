#pragma once

#include <QObject>
#include <QList>
#include <QElapsedTimer>
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
    // the right nested path). Remote-to-remote has the same "not
    // supported yet" limitation as single-file transfers.
    void enqueueFolder(FilePaneWidget *sourcePane, FilePaneWidget *destPane, const QString &folderName);

    // Cancels by id. If the item is Queued (hasn't started), just marks it
    // Cancelled directly — nothing to interrupt. If it's the currently
    // InProgress item, calls the executing backend's requestCancel() and
    // remembers that the resulting transferFailed (if any) should be
    // reported as Cancelled rather than Failed — see m_activeItemCancelled.
    // No-op for ids that are already Done/Failed/Cancelled, or don't exist.
    void cancelItem(int id);

    // Resets a Failed or Cancelled item back to Queued (clearing its error
    // message and progress) and starts processing if idle. No-op for ids
    // in any other state, or that don't exist.
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
    void folderTransferStarted(const QString &folderName);
    void folderTransferFinished(const QString &folderName, int fileCount);
    void folderTransferFailed(const QString &folderName, const QString &reason);

private slots:
    void onBackendProgress(const QString &fileName, qint64 bytesDone, qint64 bytesTotal);
    void onBackendFinished(const QString &fileName);
    void onBackendFailed(const QString &fileName, const QString &reason);
    void onBackendPaused(const QString &fileName, qint64 bytesDone);

private:
    void startNext();
    void connectToBackend(RemoteBackend *backend);
    int indexById(int id) const;

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

    QList<TransferItem> m_items;
    int m_nextId = 1;
    int m_activeIndex = -1;          // index into m_items currently running, -1 if idle
    RemoteBackend *m_currentBackend = nullptr;   // whichever backend is executing the active item
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
};
