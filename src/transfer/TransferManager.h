#pragma once

#include <QObject>
#include <QList>
#include "TransferItem.h"

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

    const QList<TransferItem> &items() const { return m_items; }

signals:
    void itemAdded(const TransferItem &item);
    void itemUpdated(const TransferItem &item);   // covers both progress and status changes
    void transferSucceeded();   // fired after any item completes — MainWindow uses this to refresh both panes

private slots:
    void onBackendProgress(const QString &fileName, qint64 bytesDone, qint64 bytesTotal);
    void onBackendFinished(const QString &fileName);
    void onBackendFailed(const QString &fileName, const QString &reason);

private:
    void startNext();
    void connectToBackend(RemoteBackend *backend);
    int indexById(int id) const;

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
};
