#include "MainWindow.h"
#include "FilePaneWidget.h"
#include "ConnectionDialog.h"
#include "SiteManagerDialog.h"
#include "PreferencesDialog.h"
#include "TransferQueueWidget.h"
#include "CommandsPaneWidget.h"
#include "EditSessionManager.h"
#include "HostKeyVerifier.h"
#include "CertificateVerifier.h"
#include "IconTheme.h"
#include "../AppSettings.h"
#include "../backends/LocalBackend.h"
#include "../backends/SftpBackend.h"
#include "../backends/FtpBackend.h"
#include "../backends/ConnectionRequest.h"
#include "../backends/ConnectionDescriptor.h"
#include "../backends/SftpCredentials.h"
#include "../backends/QuickConnectParser.h"
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
#include <QLineEdit>
#include <QInputDialog>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("ZephyrFTP v%1").arg(QStringLiteral(APP_VERSION)));
    resize(1100, 780);   // fallback default — overridden below if a previous session saved real geometry

    m_settings = new AppSettings(this);
    m_transferManager = new TransferManager(this);
    m_hostKeyVerifier = new HostKeyVerifier(this);
    m_certificateVerifier = new CertificateVerifier(this);
    m_editSessionManager = new EditSessionManager(m_transferManager, m_settings, this);
    connect(m_editSessionManager, &EditSessionManager::editUploadSucceeded, this, [this](const QString &fileName) {
        statusBar()->showMessage(tr("\"%1\" saved to server").arg(fileName), 5000);
    });

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

    // Right after buildLayout() creates both panes — a restored
    // LocalToLocal item dispatches against m_leftPane immediately
    // (paths are already-resolved absolute strings, so which pane
    // stands in as executor doesn't matter); a restored LocalToRemote/
    // RemoteToLocal item becomes PendingReconnect until a matching
    // connection shows up (see TransferManager::restorePersistedQueue()'s
    // own doc comment).
    m_transferManager->restorePersistedQueue(m_leftPane);

    // Restored last, after every dock/toolbar exists for restoreState() to
    // apply to — an empty QByteArray (first run, or settings.json not
    // written yet) is a documented safe no-op for both calls, so no
    // isEmpty() guard is needed here. restoreState() alone is also what
    // makes each dock's own toggleViewAction() (the View menu's
    // "Transfers"/"Commands" entries) the actual source of truth for
    // whether it reopens on the next launch — whatever visibility state
    // saveState() captured in closeEvent() is exactly what comes back
    // here, no separate "show on start" preference needed on top of it.
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
    // Constructed here, not in buildToolbar() where it actually lives —
    // see this member's own doc comment (MainWindow.h) for why: the View
    // menu's toggle below needs it to already exist, and buildMenuBar()
    // runs before buildToolbar() in the constructor.
    m_quickConnectEdit = new QLineEdit(this);
    m_quickConnectEdit->setPlaceholderText(tr("user@host — Enter to connect"));
    m_quickConnectEdit->setToolTip(
        tr("Quick connect: [protocol://][user@]host[:port]\n"
           "protocol is sftp, ftp, or ftps — defaults to your Preferences default protocol.\n"
           "Password auth only; for a private key, use Connect... instead."));
    m_quickConnectEdit->setFixedWidth(220);
    m_quickConnectEdit->setVisible(m_settings->quickConnectFieldVisible());
    connect(m_quickConnectEdit, &QLineEdit::returnPressed,
            this, &MainWindow::onQuickConnectReturnPressed);

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

    // NOT a toggleViewAction() — m_quickConnectEdit is a plain toolbar
    // widget, not a QDockWidget, so there's no Qt-managed checkable
    // action to reuse (confirmed no such widget existed anywhere in this
    // codebase before this). A manually-managed checkable QAction instead,
    // wired directly to the field's own setVisible() and persisted via
    // AppSettings — View-menu only, not duplicated onto the toolbar the
    // way Transfers/Commands are (a toolbar icon toggling a toolbar
    // FIELD's own visibility read as more confusing than useful, and
    // every plausible icon here would visually collide with the
    // existing green plug.svg Connect... action right next to it).
    QAction *quickConnectToggle = viewMenu->addAction(tr("&Quick Connect Field"));
    quickConnectToggle->setCheckable(true);
    quickConnectToggle->setChecked(m_settings->quickConnectFieldVisible());
    connect(quickConnectToggle, &QAction::toggled, this, [this](bool visible) {
        m_quickConnectEdit->setVisible(visible);
        m_settings->setQuickConnectFieldVisible(visible);
    });

    // Same shape as the quick-connect toggle above — one shared toggle
    // controls BOTH panes' filter-field visibility at once (each pane's
    // own filter TEXT stays independent, since the two panes usually
    // show different directories). Unlike m_quickConnectEdit, m_leftPane/
    // m_rightPane don't exist yet here (buildLayout() creates them AFTER
    // buildMenuBar() runs) — no ordering hazard though, since this
    // lambda only dereferences them at toggle time (a later, real user
    // action), never at construction time; each pane sets its OWN
    // initial filter-field visibility directly from AppSettings in its
    // own constructor instead of waiting for this toggle to do it.
    QAction *filenameFilterToggle = viewMenu->addAction(tr("Filename &Filter"));
    filenameFilterToggle->setCheckable(true);
    filenameFilterToggle->setChecked(m_settings->filenameFilterVisible());
    connect(filenameFilterToggle, &QAction::toggled, this, [this](bool visible) {
        m_leftPane->setFilterFieldVisible(visible);
        m_rightPane->setFilterFieldVisible(visible);
        m_settings->setFilenameFilterVisible(visible);
    });

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

    // Constructed back in buildMenuBar() (see m_quickConnectEdit's own
    // doc comment in MainWindow.h for why) — addWidget() reparents it
    // into this toolbar; its visibility was already set from
    // AppSettings there too, so nothing further to sync here.
    toolbar->addWidget(m_quickConnectEdit);

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

    toolbar->addSeparator();

    // The SAME QAction the View menu already uses (toggleViewAction() —
    // see buildMenuBar()'s own comment on why this is a checkable action
    // Qt keeps in sync with the dock's actual visibility both directions),
    // not a second one — adding it here just gives that existing action a
    // second, icon-bearing home, with no separate state to keep in sync.
    // Gray/neutral, same reasoning Edit's context-menu icon uses: a
    // visibility toggle has no inherent semantic color the way
    // connect/disconnect/refresh do. QToolButton:checked in theme.qss is
    // what actually makes "currently shown" visible here.
    QAction *transfersToggle = m_transfersDock->toggleViewAction();
    transfersToggle->setIcon(IconTheme::tintedIcon(":/icons/arrows-left-right.svg", IconTheme::Gray));
    toolbar->addAction(transfersToggle);

    QAction *commandsToggle = m_commandsDock->toggleViewAction();
    commandsToggle->setIcon(IconTheme::tintedIcon(":/icons/terminal-2.svg", IconTheme::Gray));
    toolbar->addAction(commandsToggle);

    toolbar->addSeparator();

    // Preferences/About: separate QActions from buildMenuBar()'s own
    // (unlike the two toggles above, there's no shared checked-state to
    // keep in sync for a plain one-shot dialog trigger — connecting two
    // actions to the same slot is simpler than threading one shared
    // QAction between the two build*() methods for no behavioral gain).
    auto *preferencesAction = toolbar->addAction(
        IconTheme::tintedIcon(":/icons/adjustments.svg", IconTheme::Gray), tr("Preferences..."));
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::onPreferencesTriggered);

    // Blue — "primary / navigation / download / info" per IconTheme's own
    // palette comment; About is the one place "info" applies.
    auto *aboutAction = toolbar->addAction(
        IconTheme::tintedIcon(":/icons/info-circle.svg", IconTheme::Blue), tr("About ZephyrFTP..."));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAboutTriggered);
}

void MainWindow::buildLayout()
{
    auto *splitter = new QSplitter(Qt::Horizontal, this);

    // Both panes start out on LocalBackend so the app is immediately
    // usable as a two-pane local file manager. Either pane's backend can
    // later be swapped for a remote one — via its own path-bar icon menu
    // (onPaneConnectRequested and friends below), or, for the right pane
    // specifically, the global toolbar's Connect/Sites (onConnectTriggered).
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
    connect(m_leftPane, &FilePaneWidget::moveRequested, this, &MainWindow::onLeftMoveRequested);
    connect(m_rightPane, &FilePaneWidget::moveRequested, this, &MainWindow::onRightMoveRequested);

    // Either pane's own path-bar icon menu can request a connect/site-
    // manager/disconnect on ITSELF — both panes wired to the same three
    // slots, which take the requesting pane directly from the signal.
    connect(m_leftPane, &FilePaneWidget::connectRequested, this, &MainWindow::onPaneConnectRequested);
    connect(m_rightPane, &FilePaneWidget::connectRequested, this, &MainWindow::onPaneConnectRequested);
    connect(m_leftPane, &FilePaneWidget::siteManagerRequested, this, &MainWindow::onPaneSiteManagerRequested);
    connect(m_rightPane, &FilePaneWidget::siteManagerRequested, this, &MainWindow::onPaneSiteManagerRequested);
    connect(m_leftPane, &FilePaneWidget::disconnectRequested, this, &MainWindow::onPaneDisconnectRequested);
    connect(m_rightPane, &FilePaneWidget::disconnectRequested, this, &MainWindow::onPaneDisconnectRequested);

    connect(m_leftPane, &FilePaneWidget::editRequested, this, &MainWindow::onPaneEditRequested);
    connect(m_rightPane, &FilePaneWidget::editRequested, this, &MainWindow::onPaneEditRequested);
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

    // Without this, the table's own sizeHint claims more first-run height
    // than the pane actually needs. ~200px matches buildCommandsPane()'s
    // own target below (title bar + content), so on a brand new
    // settings.json both docks start out the same height rather than one
    // dwarfing the other.
    resizeDocks({m_transfersDock}, {200}, Qt::Vertical);
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
    // any) the person actually left it at. 200px matches buildTransferQueue()'s
    // own target above, so Commands and Transfers start out the same height.
    resizeDocks({m_commandsDock}, {200}, Qt::Vertical);
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

void MainWindow::onLeftMoveRequested(const QList<RemoteEntry> &entries)
{
    moveEntries(m_leftPane, m_rightPane, entries);
}

void MainWindow::onRightMoveRequested(const QList<RemoteEntry> &entries)
{
    moveEntries(m_rightPane, m_leftPane, entries);
}

void MainWindow::moveEntries(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                              const QList<RemoteEntry> &entries)
{
    if (!TransferManager::moveEligible(sourcePane, destPane)) {
        QMessageBox::information(this, tr("Move"),
            tr("Move requires both panes to be on the same server, or both on your computer."));
        return;
    }

    for (const RemoteEntry &entry : entries) {
        if (entry.isDir)
            m_transferManager->moveFolder(sourcePane, destPane, entry.name);
        else
            m_transferManager->moveEntry(sourcePane, destPane, entry.name);
    }
}

void MainWindow::refreshBothPanes()
{
    // Cheap and correct beats clever here: just re-list whatever both
    // panes are currently showing. If a given pane's current directory
    // isn't the source or destination of whatever triggered this, it's a
    // no-op refresh, which is harmless. Shared by onTransferSucceeded()
    // and onRefreshTriggered() — a real duplication found by code review.
    m_leftPane->navigateTo(m_leftPane->currentDirectory());
    m_rightPane->navigateTo(m_rightPane->currentDirectory());
}

void MainWindow::onTransferSucceeded()
{
    refreshBothPanes();
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
    // Toolbar/Connection-menu path — always the right pane, preserving
    // existing behavior (see startConnection()'s own comment on why this
    // stays a fixed shortcut rather than gaining a "which pane" concept).
    connectViaDialog(m_rightPane);
}

void MainWindow::onQuickConnectReturnPressed()
{
    const QuickConnectResult result =
        parseQuickConnectString(m_quickConnectEdit->text(), m_settings->defaultProtocol());
    if (!result.ok) {
        statusBar()->showMessage(result.errorMessage, 5000);
        return;
    }

    // Password-only auth — matching SiteManagerDialog::onConnectClicked()'s
    // own QInputDialog::getText(Password) pattern exactly, but with no
    // CredentialStore prefill: this is a one-off, not a saved site.
    // Private-key auth still requires the full Connect dialog — a
    // deliberate scope boundary, not an oversight (no reasonable way to
    // fit a key path into a one-line quick-connect string).
    bool ok = false;
    const QString password = QInputDialog::getText(
        this, tr("Connect"), tr("Password for %1@%2:").arg(result.username, result.host),
        QLineEdit::Password, QString(), &ok);
    if (!ok)
        return;

    ConnectionRequest request;
    request.protocol = result.protocol;
    if (result.protocol == Protocol::Sftp) {
        request.sftp.host = result.host;
        request.sftp.port = result.port;
        request.sftp.username = result.username;
        request.sftp.authMethod = SftpAuthMethod::Password;
        request.sftp.password = password;
    } else {
        request.ftp.host = result.host;
        request.ftp.port = result.port;
        request.ftp.username = result.username;
        request.ftp.password = password;
        request.ftp.ftpsMode = protocolToFtpsMode(result.protocol);
    }

    // Same fixed-right-pane shortcut connectViaDialog() already uses —
    // see startConnection()'s own comment.
    startConnection(request, m_rightPane);
    m_quickConnectEdit->clear();
}

void MainWindow::onSiteManagerTriggered()
{
    siteManagerViaDialog(m_rightPane);
}

void MainWindow::onPaneConnectRequested(FilePaneWidget *pane)
{
    connectViaDialog(pane);
}

void MainWindow::onPaneSiteManagerRequested(FilePaneWidget *pane)
{
    siteManagerViaDialog(pane);
}

void MainWindow::onPaneDisconnectRequested(FilePaneWidget *pane)
{
    disconnectPane(pane);
}

void MainWindow::onPaneEditRequested(FilePaneWidget *pane, const RemoteEntry &entry)
{
    m_editSessionManager->startEditing(pane, entry);
}

void MainWindow::connectViaDialog(FilePaneWidget *targetPane)
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
    // choice entirely for FTP/FTPS, so missingRequiredPrivateKeyPath() is
    // always false there. Same check SiteManagerDialog::onConnectClicked()
    // makes for its own Connect button — shared via ConnectionRequest
    // itself rather than duplicated here.
    if (request.missingRequiredPrivateKeyPath()) {
        QMessageBox::warning(this, tr("Connect"), tr("Select a private key file."));
        return;
    }

    startConnection(request, targetPane);
}

void MainWindow::siteManagerViaDialog(FilePaneWidget *targetPane)
{
    SiteManagerDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    // SiteManagerDialog has already validated the host and prompted for
    // any password/passphrase by the time it accepts — nothing left to
    // check here, unlike connectViaDialog() above.
    startConnection(dialog.connectionRequestToConnect(), targetPane);
}

bool MainWindow::stillConnecting(FilePaneWidget *targetPane)
{
    if (!targetPane->isConnecting())
        return false;
    statusBar()->showMessage(
        tr("Still connecting on this pane — wait for that to finish or fail first."), 5000);
    return true;
}

void MainWindow::startConnection(const ConnectionRequest &request, FilePaneWidget *targetPane)
{
    // A real bug this guards against: targetPane->setBackend() (called at
    // the end of this function) tears down whatever backend the pane
    // already has with a blocking QThread::quit()+wait() — but quit()
    // cannot interrupt a worker thread currently stuck inside a blocking
    // connect()/SSH-handshake syscall (see FilePaneWidget::isConnecting()'s
    // own doc comment). Clicking Connect again on a pane whose previous
    // connection attempt hasn't resolved yet (a slow or packet-dropping
    // host) would otherwise freeze the entire GUI until that syscall times
    // out on its own. Refusing here — rather than fixing the underlying
    // blocking I/O, a much larger change — is the same tradeoff
    // disconnectPane() makes for the identical hazard on the Disconnect path.
    if (stillConnecting(targetPane))
        return;

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
    case Protocol::Sftp: {
        // request is a const&, so the global proxy config (see
        // AppSettings::resolvedProxyConfig()) is attached to a local
        // mutable copy of its credentials rather than request.sftp
        // itself — the only place proxy configuration enters the
        // connection-construction path, since it's global rather than
        // per-site (ConnectionRequest/ConnectionDialog/SavedSite are
        // otherwise untouched by proxy support).
        SftpCredentials creds = request.sftp;
        creds.proxy = m_settings->resolvedProxyConfig();
        creds.bandwidthLimitKBps = m_settings->bandwidthLimitKBps();
        backend = new SftpBackend(creds, m_hostKeyVerifier);
        break;
    }
    case Protocol::Ftp:
    case Protocol::Ftps: {
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
        FtpCredentials creds = request.ftp;
        creds.proxy = m_settings->resolvedProxyConfig();
        creds.bandwidthLimitKBps = m_settings->bandwidthLimitKBps();
        backend = new FtpBackend(creds, m_certificateVerifier);
        break;
    }
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
    connect(backend, &RemoteBackend::connected, this, [this, host, targetPane]() {
        statusBar()->showMessage(tr("Connected to %1").arg(host), 5000);
        // Any restored queue item still PendingReconnect and waiting for
        // exactly this connection picks back up automatically from here
        // — see TransferManager::tryReclaimPendingItems()'s own doc
        // comment. A harmless no-op when there's nothing waiting.
        m_transferManager->tryReclaimPendingItems(targetPane);
    });
    connect(backend, &RemoteBackend::connectionFailed, this, [this, host](const QString &reason) {
        statusBar()->showMessage(tr("Failed to connect to %1: %2").arg(host, reason), 8000);
    });

    statusBar()->showMessage(tr("Connecting to %1...").arg(request.host()));
    // Before the swap below, not after — see EditSessionManager::
    // endSessionsForPane()'s own doc comment on why this needs to run
    // BEFORE setBackend() takes effect, not as an afterthought: a
    // pending edit session's re-upload dispatch re-fetches
    // targetPane->backend() live, same as any other transfer, so a
    // session left dangling past this point could otherwise silently
    // redirect an edit's save to whatever NEW connection this pane ends
    // up with (reachable here too, not just Disconnect — clicking
    // Connect again on an already-connected pane is a real path, not
    // just a hypothetical one).
    m_editSessionManager->endSessionsForPane(targetPane);
    targetPane->setBackend(backend, thread);

    // After setBackend(), not before — setBackend() itself resets the
    // pane's connectionDescriptor() to empty on every swap (see its own
    // comment in FilePaneWidget.cpp), so this has to be the last word.
    // Non-secret fields only — see ConnectionDescriptor's own doc comment.
    ConnectionDescriptor descriptor;
    descriptor.savedSiteId = request.sourceSiteId;
    descriptor.protocol = request.protocol;
    descriptor.host = request.host();
    descriptor.port = request.port();
    descriptor.username = request.username();
    targetPane->setConnectionDescriptor(descriptor);
}

void MainWindow::onDisconnectTriggered()
{
    // Toolbar/Connection-menu path — always the right pane, same fixed-
    // shortcut reasoning as onConnectTriggered() above.
    disconnectPane(m_rightPane);
}

void MainWindow::disconnectPane(FilePaneWidget *targetPane)
{
    // Same hazard startConnection() guards against, for the same reason:
    // setBackend() below tears down the pane's current backend with a
    // blocking QThread::quit()+wait(), which can't interrupt a worker
    // thread still stuck inside a blocking connect()/SSH-handshake
    // syscall — refusing here is deliberately preferred to attempting to
    // interrupt that blocking I/O. closeEvent() checks isConnecting()
    // itself before ever calling this now (see its own doc comment on
    // why disconnectPane() silently no-op'ing here once let the window
    // close anyway with a worker thread still running), so this guard
    // should never actually trigger on that path anymore — it stays here
    // regardless, since the toolbar's Disconnect and a pane's own
    // path-bar menu both still call this directly, without going through
    // closeEvent() at all.
    if (stillConnecting(targetPane))
        return;

    // Same reasoning as startConnection()'s identical call — see
    // EditSessionManager::endSessionsForPane()'s own doc comment.
    m_editSessionManager->endSessionsForPane(targetPane);

    // Swap back to a plain LocalBackend. setBackend() handles tearing down
    // whatever was there before — including, if it was an SftpBackend, the
    // thread quit()/wait()/delete sequence.
    targetPane->setBackend(new LocalBackend(), nullptr);
    statusBar()->showMessage(tr("Disconnected"), 3000);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // A real bug found by code review: disconnectPane() below silently
    // no-ops (see its own doc comment) whenever a pane is still
    // mid-connect, rather than tearing down that pane's worker QThread —
    // but this function used to ignore that outcome and always accept
    // the close regardless, by falling through to
    // QMainWindow::closeEvent() unconditionally. Since main.cpp relies
    // on the default quitOnLastWindowClosed, that let the whole app exit
    // while a worker QThread parented to this window was still running —
    // reintroducing the exact qFatal("QThread: Destroyed while thread is
    // still running") crash this whole mechanism exists to prevent (see
    // this class's own header doc comment). Refusing the close outright
    // here — rather than attempting to interrupt the blocking connect()/
    // SSH-handshake syscall — is the same "wait for it to finish or fail
    // first" tradeoff startConnection()/disconnectPane() already make for
    // the identical hazard via the shared stillConnecting() helper, just
    // extended to cover window close too — checked for BOTH panes,
    // short-circuiting on whichever is found connecting first (if both
    // are, only that one's status message shows; either way the close is
    // refused, which is the only thing that actually matters here).
    if (stillConnecting(m_leftPane) || stillConnecting(m_rightPane)) {
        event->ignore();
        return;
    }

    // Saved on every close rather than only on a "clean" exit — there's no
    // meaningfully different path here worth distinguishing, and this way
    // even a close during an active transfer/connection still remembers
    // layout for next time.
    m_settings->setWindowGeometry(saveGeometry());
    m_settings->setWindowState(saveState());

    // Must run BEFORE disconnectPane() below — it reads each surviving
    // item's sourcePane/destPane connectionDescriptor(), which
    // disconnectPane()'s own setBackend(new LocalBackend()) call would
    // otherwise have already reset to empty by the time this ran. See
    // TransferManager::saveQueueForShutdown()'s own doc comment for what
    // actually gets persisted and what deliberately doesn't.
    m_transferManager->saveQueueForShutdown();

    // Belt-and-suspenders alongside disconnectPane()'s own
    // endSessionsForPane() calls below — every live session's pane is
    // always m_leftPane or m_rightPane in practice, so this is
    // logically redundant with what's about to happen, but cheap and
    // explicit rather than relying solely on that inference holding.
    m_editSessionManager->endAllSessions();

    // Same teardown as Disconnect (see MainWindow.h's doc comment on why
    // this needs to happen at all) — for BOTH panes now, since either can
    // hold a thread-owning backend, not just the right one. Neither call
    // can hit the isConnecting() no-op path above (already checked, and
    // nothing between here and there can start a new connection), so
    // each one genuinely tears its pane down rather than silently no-op'ing.
    // The transient "Disconnected" status-bar message this also produces
    // is harmless and ignorable during shutdown.
    disconnectPane(m_leftPane);
    disconnectPane(m_rightPane);
    QMainWindow::closeEvent(event);
}

void MainWindow::onRefreshTriggered()
{
    refreshBothPanes();
    statusBar()->showMessage(tr("Refreshed"), 2000);
}
