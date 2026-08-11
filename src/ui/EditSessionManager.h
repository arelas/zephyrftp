#pragma once

#include <QObject>
#include <QHash>
#include <QPointer>
#include "../backends/RemoteEntry.h"

class AppSettings;
class TransferManager;
class FilePaneWidget;
class RemoteBackend;
class QFileSystemWatcher;
class QTimer;
struct TransferItem;

// Edit-in-place: right-click a remote file -> Edit downloads it to a
// local temp file, opens it in an external editor, and re-uploads it
// every time the local copy is saved.
//
// Talks to RemoteBackend exclusively THROUGH TransferManager
// (TransferManager::startEditDownload()/startEditUpload()), never by
// calling RemoteBackend::downloadFile()/uploadFile() directly.
// RemoteBackend's transferProgress/transferFinished/transferFailed/
// transferPaused signals are backend-instance-wide, not scoped per
// call, and TransferManager is already the sole thing connected to
// them, serializing exactly one active item at a time against a given
// backend. A second, independent listener on those same signals here
// would race whatever TransferManager is legitimately doing with the
// same backend at the same moment — see ARCHITECTURE.md's
// EditSessionManager entry for the full reasoning behind this design.
class EditSessionManager : public QObject {
    Q_OBJECT
public:
    EditSessionManager(TransferManager *transferManager, AppSettings *settings, QObject *parent = nullptr);

    // Called from FilePaneWidget::editRequested. If a session already
    // exists for this (pane, remote path), just re-launches the editor
    // on the existing temp file rather than downloading it again.
    void startEditing(FilePaneWidget *pane, const RemoteEntry &entry);

    // Tears down every live session whose pane matches — stops its
    // watcher/debounce timer and deletes its temp file. MainWindow
    // calls this right before it swaps a pane to a different backend
    // (Connect on an already-connected pane, or Disconnect): re-fetching
    // a live destPane->backend() at upload time (which TransferManager's
    // dispatch already does, same as it does for every other direction)
    // would otherwise silently redirect an edit's re-upload to whatever
    // NEW connection that pane has, if a save's debounce timer fired in
    // the narrow window before this ran — this is what actually
    // prevents that, not a passive check.
    void endSessionsForPane(FilePaneWidget *pane);

    // Swept from MainWindow::closeEvent() — best-effort; the app's
    // existing zephyrftp-staging/ sweep-on-next-launch is the backstop
    // if a crash skips this entirely.
    void endAllSessions();

signals:
    // MainWindow shows a brief statusBar() message on this — the
    // Transfers pane already shows the completed upload row, so nothing
    // stronger than that is needed for the success case (unlike
    // failure, which is a QMessageBox this class shows itself).
    void editUploadSucceeded(const QString &fileName);

private slots:
    void onTransferItemUpdated(const TransferItem &item);

private:
    struct Session {
        FilePaneWidget *pane = nullptr;
        QString remotePath;
        QString localTempPath;
        QString fileName;
        // Staleness check only — TransferManager::startEditUpload()'s
        // own dispatch re-fetches destPane->backend() live, same as
        // every other direction; this QPointer is a cheap extra guard
        // (same pattern TransferItem::capturedDestBackend already
        // uses), not the primary defense. endSessionsForPane() above is.
        QPointer<RemoteBackend> backend;
        QFileSystemWatcher *watcher = nullptr;
        QTimer *debounceTimer = nullptr;
    };

    QString sessionKey(FilePaneWidget *pane, const QString &remotePath) const;
    void launchEditor(const QString &localPath) const;
    void onDownloadFinished(const QString &key, const QString &tempPath);
    void onDownloadFailed(const QString &key, const QString &reason);
    void onFileChanged(const QString &key, const QString &path);
    void startUpload(const QString &key);
    void onUploadFailed(const QString &key, const QString &reason);
    void endSession(const QString &key);

    TransferManager *m_transferManager;
    AppSettings *m_settings;
    QHash<QString, Session> m_sessions;   // keyed by sessionKey()

    // Reverse lookups from a TransferManager item id to the session it
    // belongs to — NOT a single scalar per session, since a session can
    // have more than one upload in flight/queued at once (a second save
    // landing before the first save's upload has finished is a real
    // case, not hypothetical: TransferManager just queues it behind the
    // first, same as any other pair of items would be).
    QHash<int, QString> m_downloadItemToSessionKey;
    QHash<int, QString> m_uploadItemToSessionKey;
};
