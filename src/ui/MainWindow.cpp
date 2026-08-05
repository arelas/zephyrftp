#include "MainWindow.h"
#include "FilePaneWidget.h"
#include "ConnectionDialog.h"
#include "SiteManagerDialog.h"
#include "PreferencesDialog.h"
#include "TransferQueueWidget.h"
#include "CommandsPaneWidget.h"
#include "HostKeyVerifier.h"
#include "CertificateVerifier.h"
#include "IconTheme.h"
#include "../AppSettings.h"
#include "../backends/LocalBackend.h"
#include "../backends/SftpBackend.h"
#include "../backends/FtpBackend.h"
#include "../backends/ConnectionRequest.h"
#include "../backends/SftpCredentials.h"
#include "../transfer/TransferManager.h"

// Set via target_compile_definitions() in CMakeLists.txt, from this
// project's project(VERSION ...) declaration — but defended here rather
// than assumed, since it's easy for a future CMake target that also
// happens to compile this file (a test harness constructing a real
// MainWindow, say) to forget to set it, which would otherwise be a hard
// build failure rather than a graceful "well, we don't know" fallback.
#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

#include <QSplitter>
#include <QToolBar>
#include <QMenuBar>
#include <QMenu>
#include <QStatusBar>
#include <QAction>
#include <QThread>
#include <QMessageBox>
#include <QDockWidget>
#include <QCloseEvent>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("ZephyrFTP v%1").arg(QStringLiteral(APP_VERSION)));
    resize(1100, 650);   // fallback default — overridden below if a previous session saved real geometry

    m_settings = new AppSettings(this);
    m_transferManager = new TransferManager(this);
    m_hostKeyVerifier = new HostKeyVerifier(this);
    m_certificateVerifier = new CertificateVerifier(this);

    // Ordered before buildMenuBar(): the View menu's "Transfers"/"Commands"
    // entries are each dock's own toggleViewAction(), so both docks have
    // to exist first.
    buildTransferQueue();
    buildCommandsPane();
    buildMenuBar();
    buildToolbar();
    buildLayout();

    // Wired here (after buildLayout() creates both panes and
    // buildCommandsPane() creates m_commandsPane) rather than inside
    // either build*() method — this is the one place both already exist.
    connect(m_leftPane, &FilePaneWidget::commandLogged, m_commandsPane, &CommandsPaneWidget::appendLine);
    connect(m_rightPane, &FilePaneWidget::commandLogged, m_commandsPane, &CommandsPaneWidget::appendLine);

    // Restored last, after every dock/toolbar exists for restoreState() to
    // apply to — an empty QByteArray (first run, or settings.json not
    // written yet) is a documented safe no-op for both calls, so no
    // isEmpty() guard is needed here.
    restoreGeometry(m_settings->windowGeometry());
    restoreState(m_settings->windowState());

    connect(m_transferManager, &TransferManager::transferSucceeded,
            this, &MainWindow::onTransferSucceeded);

    connect(m_transferManager, &TransferManager::folderTransferStarted, this, [this](const QString &name) {
        statusBar()->showMessage(tr("Preparing to transfer \"%1\"...").arg(name));
    });
    connect(m_transferManager, &TransferManager::folderTransferFinished,
            this, [this](const QString &name, int fileCount) {
        statusBar()->showMessage(
            fileCount > 0
                ? tr("\"%1\": %2 file(s) queued for transfer").arg(name).arg(fileCount)
                : tr("\"%1\": folder structure created (no files to transfer)").arg(name),
            5000);
    });
    connect(m_transferManager, &TransferManager::folderTransferFailed,
            this, [this](const QString &name, const QString &reason) {
        QMessageBox::warning(this, tr("Transfer"),
                              tr("Couldn't transfer \"%1\":\n%2").arg(name, reason));
    });
    connect(m_transferManager, &TransferManager::folderTransferSkipped, this, [this](const QString &name) {
        statusBar()->showMessage(tr("Skipped \"%1\" — a folder with that name already exists").arg(name), 5000);
    });

    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::buildMenuBar()
{
    // A real menu bar, not just the toolbar — the toolbar was never
    // meant to be the only way into every action (About specifically
    // has no natural toolbar icon; "info circle" would be one more icon
    // competing for space for something used rarely, not during normal
    // work).
    //
    // Connection menu mirrors the toolbar's Sites/Connect/Disconnect
    // buttons for keyboard/menu access — same actions, same slots, no
    // separate logic to keep in sync. Placed before Help, matching the
    // usual convention of app-specific menus preceding a trailing Help.
    QMenu *connectionMenu = menuBar()->addMenu(tr("&Connection"));

    QAction *sitesAction = connectionMenu->addAction(tr("&Sites..."));
    connect(sitesAction, &QAction::triggered, this, &MainWindow::onSiteManagerTriggered);

    QAction *connectAction = connectionMenu->addAction(tr("Co&nnect..."));
    connect(connectAction, &QAction::triggered, this, &MainWindow::onConnectTriggered);

    connectionMenu->addSeparator();

    QAction *disconnectAction = connectionMenu->addAction(tr("&Disconnect"));
    connect(disconnectAction, &QAction::triggered, this, &MainWindow::onDisconnectTriggered);

    // Standard File/Edit/View/.../Help menu convention — Edit is where a
    // Preferences entry conventionally lives, and there's no separate File
    // menu here for it to hide under instead.
    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    QAction *preferencesAction = editMenu->addAction(tr("&Preferences..."));
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::onPreferencesTriggered);

    // toggleViewAction() is a checkable QAction Qt keeps in sync with the
    // dock's own visibility automatically (both directions: toggling the
    // action shows/hides the dock, and the dock being hidden any other way
    // — a native close button on its floating window, for instance —
    // unchecks the action). This is the fix for a real reported dead end:
    // an undocked (floating) Transfers panel still gets a real, WM-drawn
    // close button on its own top-level window regardless of this dock's
    // own DockWidgetClosable feature, and closing it that way previously
    // left no way to get it back at all.
    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    QAction *transfersToggle = m_transfersDock->toggleViewAction();
    transfersToggle->setText(tr("&Transfers"));
    viewMenu->addAction(transfersToggle);

    // Same toggleViewAction() pattern as Transfers above — same fix for
    // the same dead end (a floating dock's WM-drawn close button has no
    // other way back without this).
    QAction *commandsToggle = m_commandsDock->toggleViewAction();
    commandsToggle->setText(tr("&Commands"));
    viewMenu->addAction(commandsToggle);

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction *aboutAction = helpMenu->addAction(tr("&About ZephyrFTP..."));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAboutTriggered);
}

void MainWindow::buildToolbar()
{
    auto *toolbar = addToolBar(tr("Main"));

    // Icon/color mapping straight from the design package's ICON-MAP.md —
    // connect=green (create/upload/success family), disconnect=red
    // (destructive), refresh=amber (in this map's "caution/in-progress"
    // bucket, since ICON-MAP explicitly lists Refresh as amber even
    // though it's not destructive — a quirk of the source spec, not a
    // typo introduced here). Sites comes first in the toolbar — it's the
    // primary way most people will actually connect, once they've saved
    // a site or two; Connect stays right after it for one-off connections.
    auto *sitesAction = toolbar->addAction(
        IconTheme::tintedIcon(":/icons/server-cog.svg", IconTheme::Blue), tr("Sites..."));
    connect(sitesAction, &QAction::triggered, this, &MainWindow::onSiteManagerTriggered);

    auto *connectAction = toolbar->addAction(
        IconTheme::tintedIcon(":/icons/plug.svg", IconTheme::Green), tr("Connect..."));
    connect(connectAction, &QAction::triggered, this, &MainWindow::onConnectTriggered);

    auto *disconnectAction = toolbar->addAction(
        IconTheme::tintedIcon(":/icons/plug-connected-x.svg", IconTheme::Red), tr("Disconnect"));
    connect(disconnectAction, &QAction::triggered, this, &MainWindow::onDisconnectTriggered);

    toolbar->addSeparator();

    // Previously a dead button (addAction with no connected slot) — giving
    // it an icon that implies it does something meant it actually needed
    // to, so this also wires it to refresh both panes' listings.
    auto *refreshAction = toolbar->addAction(
        IconTheme::tintedIcon(":/icons/refresh.svg", IconTheme::Amber), tr("Refresh"));
    connect(refreshAction, &QAction::triggered, this, &MainWindow::onRefreshTriggered);
}

void MainWindow::buildLayout()
{
    auto *splitter = new QSplitter(Qt::Horizontal, this);

    // Both panes start out on LocalBackend so the app is immediately
    // usable as a two-pane local file manager. The right pane's backend
    // gets swapped for an SftpBackend by onConnectTriggered() below.
    // m_settings passed to both so a live "show hidden files" toggle in
    // Preferences updates whichever pane(s) are showing at the time.
    m_leftPane = new FilePaneWidget(new LocalBackend(), splitter, m_settings);
    m_rightPane = new FilePaneWidget(new LocalBackend(), splitter, m_settings);

    splitter->addWidget(m_leftPane);
    splitter->addWidget(m_rightPane);
    splitter->setSizes({550, 550});

    setCentralWidget(splitter);

    connect(m_leftPane, &FilePaneWidget::fileActivated, this, &MainWindow::onLeftFileActivated);
    connect(m_rightPane, &FilePaneWidget::fileActivated, this, &MainWindow::onRightFileActivated);
    connect(m_leftPane, &FilePaneWidget::filesActivated, this, &MainWindow::onLeftFilesActivated);
    connect(m_rightPane, &FilePaneWidget::filesActivated, this, &MainWindow::onRightFilesActivated);
    connect(m_leftPane, &FilePaneWidget::filesDropped, this, &MainWindow::onFilesDropped);
    connect(m_rightPane, &FilePaneWidget::filesDropped, this, &MainWindow::onFilesDropped);
}

void MainWindow::buildTransferQueue()
{
    m_transfersDock = new QDockWidget(tr("Transfers"), this);
    // Named explicitly — QMainWindow::saveState()/restoreState() (not used
    // yet, but this avoids the ambiguous-object warning if that's ever
    // added) identify dock widgets by objectName, not the translatable
    // window title.
    m_transfersDock->setObjectName(QStringLiteral("TransfersDock"));
    // DockWidgetClosable added so the dock's own (docked-state) title bar
    // close button behaves the same, deliberate way a floating window's
    // WM-drawn close button already does — both now go through the same
    // toggleViewAction()-tracked visibility, recoverable from the View
    // menu either way.
    m_transfersDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable
                                 | QDockWidget::DockWidgetClosable);
    m_transfersDock->setWidget(new TransferQueueWidget(m_transferManager, m_transfersDock));
    addDockWidget(Qt::BottomDockWidgetArea, m_transfersDock);
}

void MainWindow::buildCommandsPane()
{
    m_commandsDock = new QDockWidget(tr("Commands"), this);
    m_commandsDock->setObjectName(QStringLiteral("CommandsDock"));
    // Same features/reasoning as m_transfersDock — see its own comment.
    m_commandsDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable
                                 | QDockWidget::DockWidgetClosable);
    m_commandsPane = new CommandsPaneWidget(m_commandsDock);
    m_commandsDock->setWidget(m_commandsPane);
    // Top, not Bottom (where Transfers goes) — this is what actually puts
    // it between the toolbar and the file panes (the central widget),
    // matching FileZilla's own message-log placement.
    addDockWidget(Qt::TopDockWidgetArea, m_commandsDock);

    // Without this, QPlainTextEdit's default sizeHint gives the dock a
    // tall first-run height that crowds out the file panes below it —
    // only matters for a brand new settings.json; restoreState() further
    // down overrides this on every later launch with whatever size (if
    // any) the person actually left it at.
    resizeDocks({m_commandsDock}, {120}, Qt::Vertical);
}

void MainWindow::onLeftFileActivated(const QString &name)
{
    // Double-click on the left (source) pane queues a transfer to the
    // right pane's current directory.
    m_transferManager->enqueue(m_leftPane, m_rightPane, name);
}

void MainWindow::onRightFileActivated(const QString &name)
{
    m_transferManager->enqueue(m_rightPane, m_leftPane, name);
}

void MainWindow::onLeftFilesActivated(const QList<RemoteEntry> &entries)
{
    enqueueEntries(m_leftPane, m_rightPane, entries);
}

void MainWindow::onRightFilesActivated(const QList<RemoteEntry> &entries)
{
    enqueueEntries(m_rightPane, m_leftPane, entries);
}

void MainWindow::onFilesDropped(FilePaneWidget *sourcePane, const QList<RemoteEntry> &entries)
{
    // sourcePane is carried explicitly in the signal (the drag's origin);
    // the destination is whichever pane actually received the drop, which
    // is the object that emitted this signal — filesDropped is forwarded
    // straight through from FileTreeView on the receiving side, so
    // sender() here is reliably the destination FilePaneWidget.
    auto *destPane = qobject_cast<FilePaneWidget *>(sender());
    if (!destPane || destPane == sourcePane)
        return;

    enqueueEntries(sourcePane, destPane, entries);
}

void MainWindow::enqueueEntries(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                                 const QList<RemoteEntry> &entries)
{
    for (const RemoteEntry &entry : entries) {
        if (entry.isDir)
            m_transferManager->enqueueFolder(sourcePane, destPane, entry.name);
        else
            m_transferManager->enqueue(sourcePane, destPane, entry.name);
    }
}

void MainWindow::onTransferSucceeded()
{
    // Cheap and correct beats clever here: just re-list whatever both
    // panes are currently showing. If neither pane's current directory
    // was the source or destination of the completed transfer this is a
    // no-op refresh, which is harmless.
    m_leftPane->navigateTo(m_leftPane->currentDirectory());
    m_rightPane->navigateTo(m_rightPane->currentDirectory());
}

void MainWindow::onAboutTriggered()
{
    // QMessageBox::about() rather than a custom dialog — this is
    // exactly the kind of simple, rarely-used, no-real-interaction
    // dialog it's built for; a hand-rolled QDialog subclass would just
    // be more code doing the same thing.
    QMessageBox::about(this, tr("About ZephyrFTP"),
        tr("<h3>ZephyrFTP</h3>"
           "<p>Version %1 — alpha software. Expect rough edges; see "
           "<a href=\"https://github.com/arelas/zephyrftp/blob/main/README.md#known-limitations\">"
           "Known limitations</a> in the README.</p>"
           "<p>A dual-pane SFTP client.</p>"
           "<p>GPL-3.0-or-later licensed. &copy; Bad Cluster.<br>"
           "<a href=\"https://github.com/arelas/zephyrftp\">github.com/arelas/zephyrftp</a></p>")
            .arg(QStringLiteral(APP_VERSION)));
}

void MainWindow::onPreferencesTriggered()
{
    PreferencesDialog dialog(m_settings, this);
    dialog.exec();
}

void MainWindow::onConnectTriggered()
{
    ConnectionDialog dialog(this);
    // setProtocol() already exists specifically for protocol-selection-test
    // to drive the dialog through all three states — reused here to apply
    // the user's own preferred default rather than always starting on SFTP.
    dialog.setProtocol(m_settings->defaultProtocol());
    if (dialog.exec() != QDialog::Accepted)
        return;

    const ConnectionRequest request = dialog.connectionRequest();

    if (request.host().isEmpty()) {
        QMessageBox::warning(this, tr("Connect"), tr("Host cannot be empty."));
        return;
    }
    // Key-auth validation only applies to SFTP — the dialog hides the auth
    // choice entirely for FTP/FTPS, so there's no key path to check there.
    if (request.protocol == Protocol::Sftp
        && request.sftp.authMethod == SftpAuthMethod::PublicKey
        && request.sftp.privateKeyPath.isEmpty()) {
        QMessageBox::warning(this, tr("Connect"), tr("Select a private key file."));
        return;
    }

    startConnection(request);
}

void MainWindow::onSiteManagerTriggered()
{
    SiteManagerDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    // SiteManagerDialog has already validated the host and prompted for
    // any password/passphrase by the time it accepts — nothing left to
    // check here, unlike onConnectTriggered above.
    startConnection(dialog.connectionRequestToConnect());
}

void MainWindow::startConnection(const ConnectionRequest &request)
{
    // No parent on the backend: it's about to be moved to a worker thread,
    // and Qt refuses to reparent an object across thread boundaries.
    // FilePaneWidget owns its lifetime manually from here via setBackend()'s
    // deleteLater + quit()/wait() teardown path instead of the usual
    // QObject parent-child chain.
    //
    // Both branches produce a RemoteBackend*, which is the entire point of
    // that interface — everything downstream of this switch (the pane, the
    // transfer manager, the queue widget) is protocol-agnostic and stays
    // that way. This function is the only place in the UI layer that knows
    // concrete backend types exist.
    RemoteBackend *backend = nullptr;
    switch (request.protocol) {
    case Protocol::Sftp:
        backend = new SftpBackend(request.sftp, m_hostKeyVerifier);
        break;
    case Protocol::Ftp:
    case Protocol::Ftps:
        // m_certificateVerifier is FTPS's equivalent of m_hostKeyVerifier:
        // an unverifiable server certificate (self-signed, unknown CA, ...)
        // goes through the same real trust-on-first-use prompt a changed
        // SSH host key would, rather than being silently accepted or
        // silently, permanently refused. Harmless to pass for plain FTP
        // too — FtpBackend only ever calls into it when ftpsMode is
        // Explicit. Plain FTP itself still authenticates the server not at
        // all, which is exactly what "unencrypted" means, and why the
        // connection dialog labels it that way rather than leaving the
        // user to infer it.
        backend = new FtpBackend(request.ftp, m_certificateVerifier);
        break;
    }

    if (!backend) {
        // Unreachable while Protocol has exactly the three values above,
        // but an unhandled future enum value silently connecting to
        // nothing would be far worse than a visible message.
        statusBar()->showMessage(tr("Unsupported protocol — cannot connect."), 5000);
        return;
    }

    auto *thread = new QThread(this);   // the QThread *controller* object is fine
                                         // to parent normally — it's backend that
                                         // can't be, since IT is what moves.
    backend->moveToThread(thread);

    // Real, reported bug: "Connecting to %1..." was never followed up —
    // nothing ever showed success or failure, so the status bar was stuck
    // on a permanently stale "Connecting..." even once the connection had
    // long since succeeded (or failed). connect() here, before
    // moveToThread() has taken effect, is still safe: Qt::AutoConnection
    // decides direct-vs-queued by the sender's thread affinity at *emit*
    // time, not at connect() time, so this correctly queues back to the
    // GUI thread once backend is actually running on its worker thread —
    // the same reasoning FilePaneWidget::setBackend() already relies on
    // for its own connections to this backend.
    const QString host = request.host();
    connect(backend, &RemoteBackend::connected, this, [this, host]() {
        statusBar()->showMessage(tr("Connected to %1").arg(host), 5000);
    });
    connect(backend, &RemoteBackend::connectionFailed, this, [this, host](const QString &reason) {
        statusBar()->showMessage(tr("Failed to connect to %1: %2").arg(host, reason), 8000);
    });

    statusBar()->showMessage(tr("Connecting to %1...").arg(request.host()));
    m_rightPane->setBackend(backend, thread);
}

void MainWindow::onDisconnectTriggered()
{
    // Swap back to a plain LocalBackend. setBackend() handles tearing down
    // whatever was there before — including, if it was an SftpBackend, the
    // thread quit()/wait()/delete sequence.
    m_rightPane->setBackend(new LocalBackend(), nullptr);
    statusBar()->showMessage(tr("Disconnected"), 3000);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Saved on every close rather than only on a "clean" exit — there's no
    // meaningfully different path here worth distinguishing, and this way
    // even a close during an active transfer/connection still remembers
    // layout for next time.
    m_settings->setWindowGeometry(saveGeometry());
    m_settings->setWindowState(saveState());

    // Same teardown as Disconnect (see MainWindow.h's doc comment on
    // why this needs to happen at all): if the right pane is still on a
    // thread-owning backend, this blocks briefly while that thread's
    // quit()/wait() completes — same tradeoff Disconnect already has
    // mid-transfer, not a new one introduced here.
    m_rightPane->setBackend(new LocalBackend(), nullptr);
    QMainWindow::closeEvent(event);
}

void MainWindow::onRefreshTriggered()
{
    m_leftPane->navigateTo(m_leftPane->currentDirectory());
    m_rightPane->navigateTo(m_rightPane->currentDirectory());
    statusBar()->showMessage(tr("Refreshed"), 2000);
}
