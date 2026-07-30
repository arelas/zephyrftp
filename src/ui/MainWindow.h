#pragma once

#include <QMainWindow>
#include "../backends/RemoteEntry.h"

class FilePaneWidget;
class TransferManager;
class HostKeyVerifier;
struct SftpCredentials;

// Top-level window: two FilePaneWidgets side by side (commander style),
// a docked transfer queue along the bottom, and a toolbar for
// connect/disconnect. Deliberately thin — transfer orchestration lives in
// TransferManager, backend I/O lives in the backends, layout/wiring lives
// here.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onLeftFileActivated(const QString &name);
    void onRightFileActivated(const QString &name);
    void onLeftFilesActivated(const QList<RemoteEntry> &entries);
    void onRightFilesActivated(const QList<RemoteEntry> &entries);
    void onFilesDropped(FilePaneWidget *sourcePane, const QList<RemoteEntry> &entries);
    void onConnectTriggered();
    void onSiteManagerTriggered();
    void onDisconnectTriggered();
    void onRefreshTriggered();
    void onTransferSucceeded();

private:
    void buildLayout();
    void buildToolbar();
    void buildTransferQueue();

    // Shared by both the plain "Connect..." dialog and the Site Manager's
    // Connect button — spins up an SftpBackend + worker QThread for the
    // given credentials and hands it to the right pane. One tested path
    // for "actually establish a connection" rather than two copies of it.
    void startConnection(const SftpCredentials &credentials);

    // Shared by onLeftFilesActivated/onRightFilesActivated/onFilesDropped
    // — all three end up with the same "here's a selection of files and/or
    // folders, transfer them from sourcePane to destPane" request, just
    // triggered differently (context menu vs. drag-and-drop). Routes each
    // entry to TransferManager::enqueue() (files) or enqueueFolder()
    // (directories) — a mixed selection is valid and handled item-by-item.
    void enqueueEntries(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                         const QList<RemoteEntry> &entries);

    FilePaneWidget *m_leftPane = nullptr;   // local, by default
    FilePaneWidget *m_rightPane = nullptr;  // remote, once connected
    TransferManager *m_transferManager = nullptr;
    // Lives on the GUI thread for the app's whole lifetime; SftpBackend
    // instances (on their own worker threads) call into this one via a
    // blocking cross-thread call to get a real host-key trust decision.
    HostKeyVerifier *m_hostKeyVerifier = nullptr;
};
