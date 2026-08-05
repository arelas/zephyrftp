#pragma once

#include <QMainWindow>
#include "../backends/RemoteEntry.h"

class FilePaneWidget;
class TransferManager;
class HostKeyVerifier;
class CertificateVerifier;
class QDockWidget;
class AppSettings;
class CommandsPaneWidget;
struct ConnectionRequest;

// Top-level window: two FilePaneWidgets side by side (commander style),
// a docked transfer queue along the bottom, and a toolbar for
// connect/disconnect. Deliberately thin — transfer orchestration lives in
// TransferManager, backend I/O lives in the backends, layout/wiring lives
// here.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    // Without this, closing the window while connected to a server is a
    // hard crash: startConnection() parents the worker QThread to this
    // window (`new QThread(this)`), and closing/quitting destroys this
    // window through Qt's ordinary child-object cleanup — which reaches
    // that QThread while its thread is still running, and QThread's own
    // destructor calls qFatal() in exactly that case ("QThread:
    // Destroyed while thread is still running"). Confirmed via a real
    // coredump, not theorized: the crash's stack trace is
    // `~QWidget -> QObjectPrivate::deleteChildren -> ~QThread` end to
    // end. Swapping the right pane back to a plain LocalBackend reuses
    // FilePaneWidget::setBackend()'s already-correct teardown
    // (deleteLater() + thread->quit() + thread->wait()) rather than
    // duplicating that logic here.
    void closeEvent(QCloseEvent *event) override;

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
    void onAboutTriggered();
    void onPreferencesTriggered();

private:
    void buildLayout();
    void buildToolbar();
    void buildMenuBar();
    void buildTransferQueue();
    void buildCommandsPane();

    // Shared by both the plain "Connect..." dialog and the Site Manager's
    // Connect button — picks the backend matching the request's protocol,
    // spins it up on a worker QThread, and hands it to the right pane.
    // One tested path for "actually establish a connection" rather than
    // one per protocol per entry point.
    void startConnection(const ConnectionRequest &request);

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
    // Same lifetime and cross-thread-call pattern as m_hostKeyVerifier
    // above, but for FtpBackend's FTPS certificate trust-on-first-use
    // decisions instead of SSH host keys.
    CertificateVerifier *m_certificateVerifier = nullptr;
    // Built in buildTransferQueue(), before buildMenuBar() runs — the View
    // menu's "Transfers" entry is this dock's own toggleViewAction(), so
    // closing/floating-and-losing it (previously a dead end: the dock had
    // no close button, but a floating QDockWidget still gets a real,
    // WM-drawn close button on its own top-level window, which the app had
    // no way to undo) always has a way back.
    QDockWidget *m_transfersDock = nullptr;
    // Built in buildCommandsPane(), same reasoning/ordering as
    // m_transfersDock above (needs to exist before buildMenuBar() builds
    // the View menu's "Commands" entry from its toggleViewAction()).
    // Docked above the central widget by default — between the toolbar
    // and the file panes, a live read-only log of both panes' protocol
    // traffic (see RemoteBackend::commandLogged's doc comment), modeled
    // on FileZilla's message log.
    QDockWidget *m_commandsDock = nullptr;
    CommandsPaneWidget *m_commandsPane = nullptr;
    // Owned for the app's whole lifetime, constructed before both panes
    // (they take a pointer to it) and before buildMenuBar() (Preferences
    // needs it too). Persists to settings.json on every change — see
    // AppSettings's own doc comment for why this isn't QSettings.
    AppSettings *m_settings = nullptr;
};
