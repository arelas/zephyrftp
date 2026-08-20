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
#include "CompareDialog.h"
#include "ProtocolCombo.h"
#include "../AppSettings.h"
#include "../UpdateChecker.h"
#include "../backends/LocalBackend.h"
#include "../backends/SftpBackend.h"
#include "../backends/FtpBackend.h"
#include "../backends/ConnectionRequest.h"
#include "../backends/ConnectionDescriptor.h"
#include "../backends/ConnectionHistory.h"
#include "../backends/SftpCredentials.h"
#include "../backends/Protocol.h"
#include "../transfer/TransferManager.h"
#include "../PathUtils.h"

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
#include <QSignalBlocker>
#include <QCloseEvent>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QInputDialog>
#include <QKeyEvent>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QApplication>

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

    // Live Dark/Light switching — see onThemeChanged()'s own doc comment.
    // Wired here for the same reason as the two connections just above:
    // the slot touches m_leftPane/m_rightPane/m_transferQueueWidget, so
    // everything it needs already has to exist.
    connect(m_settings, &AppSettings::themeChanged, this, &MainWindow::onThemeChanged);

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
    // Live progress during enumeration — without this, the message above
    // never changes until the whole tree is walked, which for a large
    // folder over SFTP/FTP (a real network round trip per directory) can
    // take upwards of ten seconds and reads exactly like a frozen app. No
    // timeout on showMessage() here, same as folderTransferStarted's own
    // call above — folderTransferFinished/Failed always follows and
    // replaces it.
    connect(m_transferManager, &TransferManager::folderTransferProgress,
            this, [this](const QString &name, int itemsFound) {
        statusBar()->showMessage(
            tr("Preparing to transfer \"%1\"... (%2 items found so far)").arg(name).arg(itemsFound));
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

    // The toolbar's Transfers/Commands buttons each reuse their dock's
    // own toggleViewAction() (see buildToolbar()) — a single shared
    // QAction whose ICON, once set there, would otherwise bleed into
    // this menu too (a QAction is one object, visible everywhere it's
    // added). The View menu wants plain checks, no icons, so each entry
    // here is its own separate QAction instead, wired to the dock's
    // visibility BOTH directions: toggling here shows/hides the dock,
    // and the dock's visibilityChanged (which also fires for every other
    // way a dock's visibility can change — the toolbar button, restoreState(),
    // or a floating dock's native WM-drawn close button) keeps the
    // checkmark honest.
    //
    // A real, shipped bug lived here: visibilityChanged ALSO fires when
    // the whole main window is minimized (confirmed directly — a
    // disposable probe showed it firing twice on showMinimized(), with
    // isVisible() going false), which — without the QSignalBlocker below —
    // drove setChecked(false), which (setChecked emits toggled() by
    // default) re-triggered the OTHER connection, calling
    // m_transfersDock->setVisible(false) explicitly. That's the critical
    // difference: minimizing only hides a dock IMPLICITLY (an ancestor
    // became invisible), which Qt automatically reverses when the window
    // is restored — but an explicit setVisible(false) call sets
    // WA_WState_ExplicitShowHide, which restoring the window does NOT
    // reverse, so the dock (and its toolbar/menu checkmark) stayed
    // permanently hidden after every minimize/restore. The
    // QSignalBlocker breaks the loop: this connection only needs to keep
    // the checkmark's VISUAL state honest, never to re-drive setVisible()
    // itself, so it must never re-emit toggled().
    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    QAction *transfersViewToggle = viewMenu->addAction(tr("&Transfers"));
    transfersViewToggle->setCheckable(true);
    transfersViewToggle->setChecked(m_transfersDock->isVisible());
    connect(transfersViewToggle, &QAction::toggled, m_transfersDock, &QDockWidget::setVisible);
    connect(m_transfersDock, &QDockWidget::visibilityChanged, transfersViewToggle, [transfersViewToggle](bool visible) {
        const QSignalBlocker blocker(transfersViewToggle);
        transfersViewToggle->setChecked(visible);
    });

    QAction *commandsViewToggle = viewMenu->addAction(tr("&Commands"));
    commandsViewToggle->setCheckable(true);
    commandsViewToggle->setChecked(m_commandsDock->isVisible());
    connect(commandsViewToggle, &QAction::toggled, m_commandsDock, &QDockWidget::setVisible);
    connect(m_commandsDock, &QDockWidget::visibilityChanged, commandsViewToggle, [commandsViewToggle](bool visible) {
        const QSignalBlocker blocker(commandsViewToggle);
        commandsViewToggle->setChecked(visible);
    });

    // NOT a toggleViewAction() — m_quickConnectToolBar (not a
    // QDockWidget) is what actually gets shown/hidden here; see its own
    // doc comment (MainWindow.h) for why the whole toolbar, not each
    // field individually, is what this toggles. Not duplicated onto the
    // toolbar itself, same reasoning as before this toolbar split: a
    // toolbar icon toggling that same toolbar's own visibility reads as
    // more confusing than useful.
    QAction *quickConnectToggle = viewMenu->addAction(tr("&Quick Connect Toolbar"));
    quickConnectToggle->setCheckable(true);
    quickConnectToggle->setChecked(m_settings->quickConnectFieldVisible());
    connect(quickConnectToggle, &QAction::toggled, this, [this](bool visible) {
        m_quickConnectToolBar->setVisible(visible);
        m_settings->setQuickConnectFieldVisible(visible);
    });

    // Same shape as the quick-connect toggle above — one shared toggle
    // controls BOTH panes' filter-field visibility at once (each pane's
    // own filter TEXT stays independent, since the two panes usually
    // show different directories). m_leftPane/m_rightPane don't exist
    // yet here either (buildLayout() creates them AFTER buildMenuBar()
    // runs) — no ordering hazard though, since this lambda only
    // dereferences them at toggle time (a later, real user action),
    // never at construction time; each pane sets its OWN initial
    // filter-field visibility directly from AppSettings in its own
    // constructor instead of waiting for this toggle to do it.
    QAction *filenameFilterToggle = viewMenu->addAction(tr("Filename &Filter"));
    filenameFilterToggle->setCheckable(true);
    filenameFilterToggle->setChecked(m_settings->filenameFilterVisible());
    connect(filenameFilterToggle, &QAction::toggled, this, [this](bool visible) {
        m_leftPane->setFilterFieldVisible(visible);
        m_rightPane->setFilterFieldVisible(visible);
        m_settings->setFilenameFilterVisible(visible);
    });

    // Stored as a member (m_synchronizedBrowsingToggle), unlike the two
    // toggles just above — disableSynchronizedBrowsingIfActive() needs to
    // programmatically uncheck it later (on a reconnect while it's on),
    // not just read its initial state once here. Same "dereference
    // m_leftPane/m_rightPane only at toggle time" reasoning as
    // filenameFilterToggle above applies to the ON branch below.
    m_synchronizedBrowsingToggle = viewMenu->addAction(tr("S&ynchronized Browsing"));
    // Lookup name for tests — same reasoning as m_leftPane/m_rightPane's
    // own setObjectName() in buildLayout().
    m_synchronizedBrowsingToggle->setObjectName(QStringLiteral("synchronizedBrowsingToggle"));
    m_synchronizedBrowsingToggle->setCheckable(true);
    m_synchronizedBrowsingToggle->setChecked(m_settings->synchronizedBrowsingEnabled());
    connect(m_synchronizedBrowsingToggle, &QAction::toggled, this, [this](bool enabled) {
        m_settings->setSynchronizedBrowsingEnabled(enabled);
        if (enabled) {
            // Snapshot both panes' CURRENT directories as the new
            // anchors — turning this on doesn't move either pane, it
            // just starts tracking future navigation from wherever they
            // already are (matches WinSCP's own behavior). Also clears
            // any stale pending-echo entries from a previous on/off
            // cycle, so they can't be misread against the new anchors.
            m_syncAnchorLeft = m_leftPane->currentDirectory();
            m_syncAnchorRight = m_rightPane->currentDirectory();
            m_pendingSyncDrivenPath.clear();
        }
    });

    viewMenu->addSeparator();
    // Not checkable (unlike the toggle above) — a one-shot action that
    // opens a dialog, not a persistent mode. Deliberately independent of
    // synchronized browsing: requiring that toggle first would needlessly
    // couple an unrelated setting to this feature, and block comparing
    // two directories that don't "correspond" under any anchor (which
    // sync browsing's own relative-path model doesn't require anyway —
    // it only mirrors navigation, not directory identity).
    QAction *compareDirectoriesAction = viewMenu->addAction(tr("&Compare Directories..."));
    connect(compareDirectoriesAction, &QAction::triggered, this, &MainWindow::onCompareDirectoriesTriggered);

    viewMenu->addSeparator();
    // Hiding the menu bar hides this very toggle along with it — that's
    // intentional, not a dead end: holding Alt (see keyPressEvent()'s own
    // doc comment) temporarily reveals the bar so it can be switched back
    // on. No icon, same "View menu is plain checks" convention as every
    // other entry here now follows.
    QAction *menuBarToggle = viewMenu->addAction(tr("Menu &Bar"));
    menuBarToggle->setCheckable(true);
    menuBarToggle->setChecked(m_settings->menuBarVisible());
    connect(menuBarToggle, &QAction::toggled, this, [this](bool visible) {
        menuBar()->setVisible(visible);
        m_settings->setMenuBarVisible(visible);
    });

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    // No toolbar icon, unlike Preferences/About — checked rarely enough
    // (and needs a network round trip) that it doesn't earn permanent
    // toolbar space the way those two do.
    QAction *checkForUpdatesAction = helpMenu->addAction(tr("Check for &Updates..."));
    connect(checkForUpdatesAction, &QAction::triggered, this, &MainWindow::onCheckForUpdatesTriggered);
    QAction *aboutAction = helpMenu->addAction(tr("&About ZephyrFTP..."));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAboutTriggered);

    // Applied last, after every menu above exists — hiding the bar
    // doesn't destroy its menus, just its own visibility.
    menuBar()->setVisible(m_settings->menuBarVisible());
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
    transfersToggle->setIcon(IconTheme::tintedIcon(":/icons/arrows-left-right.svg", IconTheme::Gray()));
    toolbar->addAction(transfersToggle);

    QAction *commandsToggle = m_commandsDock->toggleViewAction();
    commandsToggle->setIcon(IconTheme::tintedIcon(":/icons/terminal-2.svg", IconTheme::Gray()));
    toolbar->addAction(commandsToggle);

    toolbar->addSeparator();

    // Preferences/About: separate QActions from buildMenuBar()'s own
    // (unlike the two toggles above, there's no shared checked-state to
    // keep in sync for a plain one-shot dialog trigger — connecting two
    // actions to the same slot is simpler than threading one shared
    // QAction between the two build*() methods for no behavioral gain).
    m_preferencesAction = toolbar->addAction(
        IconTheme::tintedIcon(":/icons/adjustments.svg", IconTheme::Gray()), tr("Preferences..."));
    connect(m_preferencesAction, &QAction::triggered, this, &MainWindow::onPreferencesTriggered);

    // Blue — "primary / navigation / download / info" per IconTheme's own
    // palette comment; About is the one place "info" applies.
    auto *aboutAction = toolbar->addAction(
        IconTheme::tintedIcon(":/icons/info-circle.svg", IconTheme::Blue), tr("About ZephyrFTP..."));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAboutTriggered);

    // Its own toolbar, directly under the icon bar above — not sharing
    // it the way it originally did (a real reported issue: it read as
    // just another icon-bar widget rather than its own thing). The break
    // forces addToolBar() below onto a new row rather than appending to
    // the same one.
    addToolBarBreak(Qt::TopToolBarArea);
    m_quickConnectToolBar = addToolBar(tr("Quick Connect"));

    // Separate host/username/password/port/protocol fields plus an
    // explicit Connect button — replaces an earlier single free-text
    // "[protocol://][user@]host[:port]" field (a real reported
    // usability issue: the syntax wasn't discoverable). Protocol
    // defaults to Preferences' own default, same precedent
    // connectViaDialog() already establishes for the full Connect
    // dialog. Port is a plain QLineEdit, not a QSpinBox — a port is
    // typed, not nudged one integer at a time (same reasoning
    // PreferencesDialog/SiteManagerDialog's own port fields use).
    // Order (host/username/password/port, THEN protocol, THEN Connect)
    // matches a real reported preference — protocol sits right before
    // the action it configures rather than leading the row, and host
    // leads before username. Password sits right after username (real
    // credential entry, typed here directly) — see
    // onQuickConnectReturnPressed() for why there's no QInputDialog
    // prompt in between anymore.
    m_quickConnectHostEdit = new QLineEdit(this);
    m_quickConnectHostEdit->setPlaceholderText(tr("Host"));
    m_quickConnectHostEdit->setFixedWidth(160);
    connect(m_quickConnectHostEdit, &QLineEdit::returnPressed,
            this, &MainWindow::onQuickConnectReturnPressed);
    m_quickConnectToolBar->addWidget(m_quickConnectHostEdit);

    m_quickConnectUsernameEdit = new QLineEdit(this);
    m_quickConnectUsernameEdit->setPlaceholderText(tr("Username"));
    m_quickConnectUsernameEdit->setFixedWidth(110);
    connect(m_quickConnectUsernameEdit, &QLineEdit::returnPressed,
            this, &MainWindow::onQuickConnectReturnPressed);
    m_quickConnectToolBar->addWidget(m_quickConnectUsernameEdit);

    m_quickConnectPasswordEdit = new QLineEdit(this);
    m_quickConnectPasswordEdit->setPlaceholderText(tr("Password"));
    m_quickConnectPasswordEdit->setEchoMode(QLineEdit::Password);
    m_quickConnectPasswordEdit->setFixedWidth(110);
    connect(m_quickConnectPasswordEdit, &QLineEdit::returnPressed,
            this, &MainWindow::onQuickConnectReturnPressed);
    m_quickConnectToolBar->addWidget(m_quickConnectPasswordEdit);

    m_quickConnectPortEdit = new QLineEdit(this);
    m_quickConnectPortEdit->setPlaceholderText(tr("Port"));
    m_quickConnectPortEdit->setFixedWidth(55);
    connect(m_quickConnectPortEdit, &QLineEdit::returnPressed,
            this, &MainWindow::onQuickConnectReturnPressed);
    m_quickConnectToolBar->addWidget(m_quickConnectPortEdit);

    m_quickConnectProtocolCombo = new QComboBox(this);
    ProtocolCombo::populate(m_quickConnectProtocolCombo);
    const int defaultProtocolIndex =
        m_quickConnectProtocolCombo->findData(int(m_settings->defaultProtocol()));
    if (defaultProtocolIndex >= 0)
        m_quickConnectProtocolCombo->setCurrentIndex(defaultProtocolIndex);
    m_quickConnectToolBar->addWidget(m_quickConnectProtocolCombo);

    m_quickConnectConnectButton = new QPushButton(tr("Connect"), this);
    m_quickConnectConnectButton->setIcon(IconTheme::tintedIcon(":/icons/plug.svg", IconTheme::Green));
    connect(m_quickConnectConnectButton, &QPushButton::clicked,
            this, &MainWindow::onQuickConnectReturnPressed);
    m_quickConnectToolBar->addWidget(m_quickConnectConnectButton);

    m_quickConnectToolBar->setVisible(m_settings->quickConnectFieldVisible());
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
    m_leftPane = new FilePaneWidget(new LocalBackend(), splitter, m_settings, m_transferManager);
    m_rightPane = new FilePaneWidget(new LocalBackend(), splitter, m_settings, m_transferManager);
    // Same objectName-for-lookup precedent m_transfersDock/m_commandsDock
    // already establish below — lets a test find each pane reliably via
    // findChild<FilePaneWidget*>("leftPane"/"rightPane") rather than
    // relying on findChildren()'s traversal order, which Qt doesn't
    // document as a stable left-before-right guarantee.
    m_leftPane->setObjectName(QStringLiteral("leftPane"));
    m_rightPane->setObjectName(QStringLiteral("rightPane"));

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
    connect(m_leftPane, &FilePaneWidget::externalFilesDropped, this, &MainWindow::onExternalFilesDropped);
    connect(m_rightPane, &FilePaneWidget::externalFilesDropped, this, &MainWindow::onExternalFilesDropped);
    connect(m_leftPane, &FilePaneWidget::moveRequested, this, &MainWindow::onLeftMoveRequested);
    connect(m_rightPane, &FilePaneWidget::moveRequested, this, &MainWindow::onRightMoveRequested);
    // Ctrl+C/Ctrl+V — same dual-connection-plus-sender() pattern
    // filesDropped uses above, since both signals are symmetric (either
    // pane can be the copy source or the paste destination).
    connect(m_leftPane, &FilePaneWidget::filesCopied, this, &MainWindow::onFilesCopied);
    connect(m_rightPane, &FilePaneWidget::filesCopied, this, &MainWindow::onFilesCopied);
    connect(m_leftPane, &FilePaneWidget::pasteRequested, this, &MainWindow::onPasteRequested);
    connect(m_rightPane, &FilePaneWidget::pasteRequested, this, &MainWindow::onPasteRequested);

    // Both panes wired to the same slot — sender() identifies which pane
    // fired, same dual-connection pattern filesDropped uses above. Drives
    // synchronized browsing; see onPaneDirectoryChanged()'s own comment.
    connect(m_leftPane, &FilePaneWidget::directoryChanged, this, &MainWindow::onPaneDirectoryChanged);
    connect(m_rightPane, &FilePaneWidget::directoryChanged, this, &MainWindow::onPaneDirectoryChanged);

    // Either pane's own path-bar icon menu can request a connect/site-
    // manager/disconnect on ITSELF — both panes wired to the same three
    // slots, which take the requesting pane directly from the signal.
    connect(m_leftPane, &FilePaneWidget::connectRequested, this, &MainWindow::onPaneConnectRequested);
    connect(m_rightPane, &FilePaneWidget::connectRequested, this, &MainWindow::onPaneConnectRequested);
    connect(m_leftPane, &FilePaneWidget::siteManagerRequested, this, &MainWindow::onPaneSiteManagerRequested);
    connect(m_rightPane, &FilePaneWidget::siteManagerRequested, this, &MainWindow::onPaneSiteManagerRequested);
    connect(m_leftPane, &FilePaneWidget::disconnectRequested, this, &MainWindow::onPaneDisconnectRequested);
    connect(m_rightPane, &FilePaneWidget::disconnectRequested, this, &MainWindow::onPaneDisconnectRequested);
    connect(m_leftPane, &FilePaneWidget::recentConnectionRequested,
            this, &MainWindow::onPaneRecentConnectionRequested);
    connect(m_rightPane, &FilePaneWidget::recentConnectionRequested,
            this, &MainWindow::onPaneRecentConnectionRequested);

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
    m_transferQueueWidget = new TransferQueueWidget(m_transferManager, m_transfersDock);
    m_transfersDock->setWidget(m_transferQueueWidget);
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

void MainWindow::onExternalFilesDropped(const QStringList &localPaths)
{
    // Same sender()-identifies-the-destination technique onFilesDropped()
    // above already uses — there's no source pane to compare against
    // here, since the source is the OS itself, not a different pane.
    auto *destPane = qobject_cast<FilePaneWidget *>(sender());
    if (!destPane)
        return;

    for (const QString &localPath : localPaths) {
        const QFileInfo info(localPath);
        if (!info.exists())
            continue;   // e.g. the OS offered a path that vanished between drag and drop — skip, don't fail the whole batch
        if (info.isDir())
            m_transferManager->enqueueExternalFolder(destPane, localPath, info.fileName());
        else
            m_transferManager->enqueueExternalUpload(destPane, localPath, info.fileName());
    }
}

void MainWindow::onFilesCopied(const QList<RemoteEntry> &entries)
{
    // sender() is the pane the Ctrl+C happened on — same identification
    // technique onFilesDropped() already uses. Capturing
    // currentDirectory() HERE, at copy time, rather than re-reading it
    // whenever Paste eventually happens, is deliberate: unlike every
    // other cross-pane operation this app has (Transfer/Move/drag-drop),
    // which are all a single synchronous user gesture with no gap for
    // anything to change in between, copy-then-paste has an inherent,
    // arbitrary time gap by design — the whole point is copy now,
    // possibly navigate around, paste later. onPasteRequested() checks
    // the source pane's CURRENT directory against what's captured here
    // and refuses the paste rather than silently transferring from
    // wherever that pane happens to be now if the two don't match.
    auto *sourcePane = qobject_cast<FilePaneWidget *>(sender());
    if (!sourcePane)
        return;

    m_clipboardSourcePane = sourcePane;
    m_clipboardSourceDirectory = sourcePane->currentDirectory();
    m_clipboardEntries = entries;

    statusBar()->showMessage(
        entries.size() == 1 ? tr("Copied 1 item — paste into the other pane")
                             : tr("Copied %1 items — paste into the other pane").arg(entries.size()),
        4000);
}

void MainWindow::onPasteRequested()
{
    auto *destPane = qobject_cast<FilePaneWidget *>(sender());
    if (!destPane)
        return;

    if (!m_clipboardSourcePane) {
        statusBar()->showMessage(tr("Nothing copied yet — select something and press Ctrl+C first."), 4000);
        return;
    }
    if (destPane == m_clipboardSourcePane) {
        statusBar()->showMessage(tr("Can't paste into the same pane it was copied from."), 4000);
        return;
    }
    // See onFilesCopied()'s own comment on why this check exists at
    // all — the source pane may have navigated elsewhere in the
    // (arbitrarily long) gap between Ctrl+C and this Ctrl+V, and
    // enqueue()/enqueueFolder() both derive their actual source path
    // from the pane's CURRENT directory, not anything captured earlier.
    // Refusing here, rather than silently transferring whatever
    // (possibly nonexistent, possibly just wrong) entries now share
    // those same names in the new directory, matches this app's
    // existing "explain why, don't guess" precedent (moveRequested's own
    // ineligibility message).
    if (m_clipboardSourcePane->currentDirectory() != m_clipboardSourceDirectory) {
        statusBar()->showMessage(
            tr("The copied selection's source folder has changed — copy again."), 5000);
        return;
    }

    enqueueEntries(m_clipboardSourcePane, destPane, m_clipboardEntries);
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
           "<p>Version %1 — beta software. Expect rough edges; see "
           "<a href=\"https://github.com/arelas/zephyrftp/blob/main/README.md#known-limitations\">"
           "Known limitations</a> in the README.</p>"
           "<p>A dual-pane SFTP client.</p>"
           "<p>GPL-3.0-or-later licensed. &copy; Bad Cluster.<br>"
           "<a href=\"https://github.com/arelas/zephyrftp\">github.com/arelas/zephyrftp</a></p>")
            .arg(QStringLiteral(APP_VERSION)));
}

void MainWindow::onCheckForUpdatesTriggered()
{
    statusBar()->showMessage(tr("Checking for updates..."));

    // Parented to `this` for safety (cleaned up on window close even if
    // the reply never arrives), explicitly deleteLater()'d the moment
    // its one result signal fires either way — a one-shot object with
    // no reason to outlive this single check.
    auto *checker = new UpdateChecker(m_settings, this);
    connect(checker, &UpdateChecker::checked, this,
            [this, checker](bool updateAvailable, const QString &latestVersion, const QString &releaseUrl) {
        statusBar()->clearMessage();
        if (updateAvailable) {
            // Rich-text QMessageBox with an <a href> link, same pattern
            // onAboutTriggered() already uses for its GitHub link —
            // QMessageBox opens it via the system browser automatically
            // when clicked, no extra wiring needed.
            QMessageBox::information(this, tr("Update Available"),
                tr("<p>Version %1 is available — you're on %2.</p>"
                   "<p><a href=\"%3\">View the release on GitHub</a></p>")
                    .arg(latestVersion, QStringLiteral(APP_VERSION), releaseUrl));
        } else {
            QMessageBox::information(this, tr("Check for Updates"),
                tr("You're up to date — version %1 is the latest release.").arg(QStringLiteral(APP_VERSION)));
        }
        checker->deleteLater();
    });
    connect(checker, &UpdateChecker::failed, this, [this, checker](const QString &reason) {
        statusBar()->clearMessage();
        QMessageBox::warning(this, tr("Check for Updates"),
                              tr("Couldn't check for updates:\n%1").arg(reason));
        checker->deleteLater();
    });
    checker->check();
}

void MainWindow::onPreferencesTriggered()
{
    PreferencesDialog dialog(m_settings, this);
    dialog.exec();
}

void MainWindow::onThemeChanged(Theme theme)
{
    IconTheme::setTheme(theme);

    // Qt re-polishes every existing widget automatically on a
    // QApplication-level setStyleSheet() call — a well-established, safe
    // operation, not something needing per-widget intervention.
    const QString themeResourcePath = theme == Theme::Light
        ? QStringLiteral(":/theme/theme-light.qss")
        : QStringLiteral(":/theme/theme.qss");
    QFile themeFile(themeResourcePath);
    if (themeFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qApp->setStyleSheet(QTextStream(&themeFile).readAll());
    }

    // Re-tint the few icons this class set once at construction using a
    // theme-dependent color — see m_preferencesAction's own doc comment
    // for why it's the only toolbar action needing this (everything else
    // either uses a fixed accent color or is reachable via
    // m_transfersDock/m_commandsDock's own toggleViewAction()).
    m_transfersDock->toggleViewAction()->setIcon(
        IconTheme::tintedIcon(":/icons/arrows-left-right.svg", IconTheme::Gray()));
    m_commandsDock->toggleViewAction()->setIcon(
        IconTheme::tintedIcon(":/icons/terminal-2.svg", IconTheme::Gray()));
    m_preferencesAction->setIcon(
        IconTheme::tintedIcon(":/icons/adjustments.svg", IconTheme::Gray()));

    // m_leftPane/m_rightPane are NOT called here — each already
    // self-connects its own retintIcons() to m_settings->themeChanged in
    // its constructor (see FilePaneWidget's own doc comment on that
    // connection), so calling it again here would just redo the same
    // work a second time. TransferQueueWidget doesn't take an
    // AppSettings* today, so it can't self-subscribe the same way —
    // triggered explicitly here instead.
    m_transferQueueWidget->retintIcons();
}

void MainWindow::onCompareDirectoriesTriggered()
{
    // Re-triggering while one is already open raises the existing dialog
    // instead of stacking a second one — m_compareDialog is a QPointer,
    // so it reads as null here once the user has closed a previous one
    // (WA_DeleteOnClose) rather than dangling.
    if (m_compareDialog) {
        m_compareDialog->raise();
        m_compareDialog->activateWindow();
        return;
    }

    m_compareDialog = new CompareDialog(m_leftPane, m_rightPane, m_transferManager, this);
    // CompareDialog doesn't duplicate a transfer-progress UI of its own —
    // it just ensures the existing transfer queue dock is visible/raised
    // right after queuing a copy batch, so the user isn't left wondering
    // where their queued work went.
    connect(m_compareDialog, &CompareDialog::copyQueued, this, [this]() {
        m_transfersDock->setVisible(true);
        m_transfersDock->raise();
    });
    m_compareDialog->show();
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
    const QString host = m_quickConnectHostEdit->text().trimmed();
    if (host.isEmpty()) {
        statusBar()->showMessage(tr("Enter a host to connect to."), 5000);
        return;
    }

    const Protocol protocol = Protocol(m_quickConnectProtocolCombo->currentData().toInt());
    const QString username = m_quickConnectUsernameEdit->text().trimmed();

    // Blank means "use protocol's own default port" — same fallback
    // the old free-text field's parser used.
    const QString portText = m_quickConnectPortEdit->text().trimmed();
    int port = defaultPortFor(protocol);
    if (!portText.isEmpty()) {
        bool portOk = false;
        const int parsedPort = portText.toInt(&portOk);
        if (!portOk || parsedPort < 1 || parsedPort > 65535) {
            statusBar()->showMessage(tr("Invalid port \"%1\".").arg(portText), 5000);
            return;
        }
        port = parsedPort;
    }

    // Password-only auth, typed directly into the toolbar's own password
    // field — no QInputDialog prompt in between (an earlier version of
    // this toolbar popped one, matching SiteManagerDialog::
    // onConnectClicked()'s own pattern; a real reported usability
    // complaint was that this made "quick" connect not actually quick —
    // an extra modal to dismiss on every single connection). No
    // CredentialStore prefill/save-back either way: this is a one-off,
    // not a saved site. Private-key auth still requires the full Connect
    // dialog — a deliberate scope boundary, not an oversight (no
    // reasonable way to fit a key path into this toolbar).
    const QString password = m_quickConnectPasswordEdit->text();

    ConnectionRequest request;
    request.protocol = protocol;
    if (protocol == Protocol::Sftp) {
        request.sftp.host = host;
        request.sftp.port = port;
        request.sftp.username = username;
        request.sftp.authMethod = SftpAuthMethod::Password;
        request.sftp.password = password;
    } else {
        request.ftp.host = host;
        request.ftp.port = port;
        request.ftp.username = username;
        request.ftp.password = password;
        request.ftp.ftpsMode = protocolToFtpsMode(protocol);
    }

    // Same fixed-right-pane shortcut connectViaDialog() already uses —
    // see startConnection()'s own comment.
    startConnection(request, m_rightPane);
    m_quickConnectUsernameEdit->clear();
    m_quickConnectPasswordEdit->clear();
    m_quickConnectHostEdit->clear();
    m_quickConnectPortEdit->clear();
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

void MainWindow::onPaneRecentConnectionRequested(FilePaneWidget *pane, const ConnectionHistoryEntry &entry)
{
    ConnectionRequest request = entry.toConnectionRequest();

    // Same prompt SiteManagerDialog::onConnectClicked() shows for its own
    // Connect button — adapted inline rather than reused directly, since
    // that class owns dialog-specific widget state this flow has no use
    // for. Deliberately no CredentialStore save-back step afterward: a
    // history entry has no saved-site id to key a stored secret on, and
    // pre-filling from a stored secret isn't offered here either, for the
    // same reason. Cancelling the prompt aborts without connecting, same
    // as every other entry point that shows one.
    if (request.protocol == Protocol::Sftp && request.sftp.authMethod == SftpAuthMethod::PublicKey) {
        bool ok = false;
        const QString passphrase = QInputDialog::getText(
            this, tr("Key Passphrase"),
            tr("Passphrase for the private key (leave blank if none):"),
            QLineEdit::Password, QString(), &ok);
        if (!ok)
            return;
        request.sftp.passphrase = passphrase;
    } else {
        bool ok = false;
        const QString password = QInputDialog::getText(
            this, tr("Password"),
            tr("Password for %1@%2:").arg(
                request.protocol == Protocol::Sftp ? request.sftp.username : request.ftp.username,
                request.host()),
            QLineEdit::Password, QString(), &ok);
        if (!ok)
            return;

        if (request.protocol == Protocol::Sftp)
            request.sftp.password = password;
        else
            request.ftp.password = password;
    }

    startConnection(request, pane);
}

void MainWindow::onPaneEditRequested(FilePaneWidget *pane, const RemoteEntry &entry)
{
    m_editSessionManager->startEditing(pane, entry);
}

void MainWindow::onPaneDirectoryChanged(const QString &path)
{
    auto *pane = qobject_cast<FilePaneWidget *>(sender());
    if (!pane)
        return;   // defensive — this slot is only ever connected to a FilePaneWidget's own signal

    // Step 1: is this the echo of a driven navigation THIS class itself
    // just issued to `pane`? Checked and consumed regardless of whether
    // synchronized browsing is still on — if it was on when the drive
    // was issued but got toggled off before the echo arrived, there's
    // still nothing useful to do with a self-inflicted update.
    const auto pendingIt = m_pendingSyncDrivenPath.find(pane);
    if (pendingIt != m_pendingSyncDrivenPath.end()) {
        const QString expected = pendingIt.value();
        m_pendingSyncDrivenPath.erase(pendingIt);
        if (expected == path)
            return;
        // Else: a genuine navigation overtook/raced the driven one —
        // fall through and treat THIS as the real, current position.
    }

    if (!m_settings->synchronizedBrowsingEnabled())
        return;

    FilePaneWidget *other = (pane == m_leftPane) ? m_rightPane : m_leftPane;
    const QString &anchor = (pane == m_leftPane) ? m_syncAnchorLeft : m_syncAnchorRight;
    const QString &otherAnchor = (pane == m_leftPane) ? m_syncAnchorRight : m_syncAnchorLeft;

    // A path-boundary-aware prefix check, not a raw string prefix — plain
    // startsWith(anchor) also matches an unrelated sibling that merely
    // shares the anchor as a TEXT prefix (e.g. anchor "/data" matching
    // "/data2/photos"), which would drive the other pane to a bogus
    // target built from the wrong "relative" substring. Requires the
    // anchor to be followed by a separator (or matched exactly) to count
    // as genuinely "inside" it.
    const QString anchorPrefix = anchor.endsWith('/') ? anchor : anchor + '/';
    if (path != anchor && !path.startsWith(anchorPrefix))
        return;   // navigated out of the anchored subtree (e.g. Up past it) — deliberately doesn't propagate

    // Handles the anchor itself being filesystem root ("/", already ends
    // in '/', so anchorPrefix == anchor) without double-stripping, and
    // navigating back to the anchor's own path exactly (relative stays
    // empty) without tacking on a spurious trailing slash — joinPath()
    // alone would always add one, even onto an empty name.
    const QString relative = (path == anchor) ? QString() : path.mid(anchorPrefix.length());
    const QString target = relative.isEmpty() ? otherAnchor : joinPath(otherAnchor, relative);

    m_pendingSyncDrivenPath[other] = target;
    other->navigateTo(target);
}

void MainWindow::disableSynchronizedBrowsingIfActive()
{
    if (!m_settings->synchronizedBrowsingEnabled())
        return;
    // setChecked() emits toggled(), which is what actually persists the
    // change via AppSettings::setSynchronizedBrowsingEnabled() (see
    // buildMenuBar()'s handler) — not called directly here, so the menu
    // action and the setting can never visibly disagree.
    m_synchronizedBrowsingToggle->setChecked(false);
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
    // Built alongside `backend` in each branch below, capturing the SAME
    // resolved credentials (proxy/bandwidth already mixed in) — used
    // after setBackend() to let the target pane build up to
    // request.simultaneousConnections() total connections to this same
    // server, on demand, for TransferManager to dispatch through in
    // parallel. See FilePaneWidget::configureTransferPool()'s own doc
    // comment. Left default-constructed (empty) for a request with
    // simultaneousConnections() <= 1 — configureTransferPool() still
    // gets called (clamping maxSize to 1), but an empty factory is never
    // invoked since pickIdleTransferBackend() only reaches for it once
    // every existing connection is already busy.
    FilePaneWidget::TransferBackendFactory poolFactory;
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
        poolFactory = [this, creds]() -> QPair<RemoteBackend *, QThread *> {
            auto *extraBackend = new SftpBackend(creds, m_hostKeyVerifier);
            auto *extraThread = new QThread(this);
            extraBackend->moveToThread(extraThread);
            return {extraBackend, extraThread};
        };
        break;
    }
    case Protocol::Ftp:
    case Protocol::Ftps:
    case Protocol::FtpsImplicit: {
        // m_certificateVerifier is FTPS's equivalent of m_hostKeyVerifier:
        // an unverifiable server certificate (self-signed, unknown CA, ...)
        // goes through the same real trust-on-first-use prompt a changed
        // SSH host key would, rather than being silently accepted or
        // silently, permanently refused. Harmless to pass for plain FTP
        // too — FtpBackend only ever calls into it when ftpsMode isn't
        // None (both Explicit and Implicit — see FtpBackend::usesTls()).
        // Plain FTP itself still authenticates the server not at all,
        // which is exactly what "unencrypted" means, and why the
        // connection dialog labels it that way rather than leaving the
        // user to infer it.
        FtpCredentials creds = request.ftp;
        creds.proxy = m_settings->resolvedProxyConfig();
        creds.bandwidthLimitKBps = m_settings->bandwidthLimitKBps();
        backend = new FtpBackend(creds, m_certificateVerifier);
        poolFactory = [this, creds]() -> QPair<RemoteBackend *, QThread *> {
            auto *extraBackend = new FtpBackend(creds, m_certificateVerifier);
            auto *extraThread = new QThread(this);
            extraBackend->moveToThread(extraThread);
            return {extraBackend, extraThread};
        };
        break;
    }
    }

    if (!backend) {
        // Unreachable while Protocol has exactly the four values above,
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
    connect(backend, &RemoteBackend::connected, this, [this, host, targetPane, request]() {
        statusBar()->showMessage(tr("Connected to %1").arg(host), 5000);
        // Any restored queue item still PendingReconnect and waiting for
        // exactly this connection picks back up automatically from here
        // — see TransferManager::tryReclaimPendingItems()'s own doc
        // comment. A harmless no-op when there's nothing waiting.
        m_transferManager->tryReclaimPendingItems(targetPane);

        // Recorded here — on genuine success, not merely on attempt —
        // rather than alongside the ConnectionDescriptor built below,
        // which runs synchronously right after setBackend() regardless
        // of whether the connection actually succeeds. Only ad-hoc
        // connections (no sourceSiteId) are recorded: one already saved
        // in Site Manager is one click away there, so duplicating it
        // into a second list would be noise, not new value. See
        // ConnectionHistoryEntry's own doc comment for why no secret is
        // ever captured here.
        if (request.sourceSiteId.isEmpty()) {
            ConnectionHistoryEntry entry;
            entry.protocol = request.protocol;
            entry.host = request.host();
            entry.port = request.port();
            entry.username = request.username();
            if (request.protocol == Protocol::Sftp) {
                entry.authMethod = request.sftp.authMethod;
                entry.privateKeyPath = request.sftp.privateKeyPath;
            } else {
                entry.ftpsMode = request.ftp.ftpsMode;
            }
            entry.useHomeDirectory = request.useHomeDirectory();
            entry.startingDirectory = request.startingDirectory();
            entry.lastConnectedAtMs = QDateTime::currentMSecsSinceEpoch();
            ConnectionHistoryStore::recordConnection(entry);
        }
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
    // Any synchronized-browsing anchor is about to point at a directory
    // on a connection that no longer exists on this pane — see
    // disableSynchronizedBrowsingIfActive()'s own doc comment.
    disableSynchronizedBrowsingIfActive();
    targetPane->setBackend(backend, thread);
    targetPane->configureTransferPool(request.simultaneousConnections(), poolFactory);

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

    // Same reasoning as startConnection()'s identical call.
    disableSynchronizedBrowsingIfActive();

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

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Alt && !m_settings->menuBarVisible() && !menuBar()->isVisible())
        menuBar()->setVisible(true);
    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent *event)
{
    // menuBar()->activeAction() is non-null while a menu opened from
    // this reveal is still being navigated (arrow keys, a mnemonic) —
    // skip hiding in that case so releasing Alt mid-navigation doesn't
    // yank the bar out from under it.
    if (event->key() == Qt::Key_Alt && !m_settings->menuBarVisible() && !menuBar()->activeAction())
        menuBar()->setVisible(false);
    QMainWindow::keyReleaseEvent(event);
}

void MainWindow::onRefreshTriggered()
{
    refreshBothPanes();
    statusBar()->showMessage(tr("Refreshed"), 2000);
}
