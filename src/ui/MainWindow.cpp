#include "MainWindow.h"
#include "FilePaneWidget.h"
#include "ConnectionDialog.h"
#include "TransferQueueWidget.h"
#include "HostKeyVerifier.h"
#include "../backends/LocalBackend.h"
#include "../backends/SftpBackend.h"
#include "../transfer/TransferManager.h"

#include <QSplitter>
#include <QToolBar>
#include <QStatusBar>
#include <QAction>
#include <QThread>
#include <QMessageBox>
#include <QDockWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("ZephyrFTP"));
    resize(1100, 650);

    m_transferManager = new TransferManager(this);
    m_hostKeyVerifier = new HostKeyVerifier(this);

    buildToolbar();
    buildLayout();
    buildTransferQueue();

    connect(m_transferManager, &TransferManager::transferSucceeded,
            this, &MainWindow::onTransferSucceeded);

    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::buildToolbar()
{
    auto *toolbar = addToolBar(tr("Main"));

    auto *connectAction = toolbar->addAction(tr("Connect..."));
    connect(connectAction, &QAction::triggered, this, &MainWindow::onConnectTriggered);

    auto *disconnectAction = toolbar->addAction(tr("Disconnect"));
    connect(disconnectAction, &QAction::triggered, this, &MainWindow::onDisconnectTriggered);

    toolbar->addSeparator();
    toolbar->addAction(tr("Refresh"));
}

void MainWindow::buildLayout()
{
    auto *splitter = new QSplitter(Qt::Horizontal, this);

    // Both panes start out on LocalBackend so the app is immediately
    // usable as a two-pane local file manager. The right pane's backend
    // gets swapped for an SftpBackend by onConnectTriggered() below.
    m_leftPane = new FilePaneWidget(new LocalBackend(), splitter);
    m_rightPane = new FilePaneWidget(new LocalBackend(), splitter);

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
    auto *dock = new QDockWidget(tr("Transfers"), this);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    dock->setWidget(new TransferQueueWidget(m_transferManager, dock));
    addDockWidget(Qt::BottomDockWidgetArea, dock);
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

void MainWindow::onLeftFilesActivated(const QStringList &names)
{
    // One enqueue() call per file — TransferManager's queue is already
    // designed to hold many items and process them serially, so a
    // multi-select "Transfer Selected" is just several single-file
    // enqueues in a row, not a new code path.
    for (const QString &name : names)
        m_transferManager->enqueue(m_leftPane, m_rightPane, name);
}

void MainWindow::onRightFilesActivated(const QStringList &names)
{
    for (const QString &name : names)
        m_transferManager->enqueue(m_rightPane, m_leftPane, name);
}

void MainWindow::onFilesDropped(FilePaneWidget *sourcePane, const QStringList &names)
{
    // sourcePane is carried explicitly in the signal (the drag's origin);
    // the destination is whichever pane actually received the drop, which
    // is the object that emitted this signal — filesDropped is forwarded
    // straight through from FileTreeView on the receiving side, so
    // sender() here is reliably the destination FilePaneWidget.
    auto *destPane = qobject_cast<FilePaneWidget *>(sender());
    if (!destPane || destPane == sourcePane)
        return;

    for (const QString &name : names)
        m_transferManager->enqueue(sourcePane, destPane, name);
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

void MainWindow::onConnectTriggered()
{
    ConnectionDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const SftpCredentials creds = dialog.credentials();

    if (creds.host.isEmpty()) {
        QMessageBox::warning(this, tr("Connect"), tr("Host cannot be empty."));
        return;
    }
    if (creds.authMethod == SftpAuthMethod::PublicKey && creds.privateKeyPath.isEmpty()) {
        QMessageBox::warning(this, tr("Connect"), tr("Select a private key file."));
        return;
    }

    // No parent: SftpBackend is about to be moved to a worker thread, and
    // Qt refuses to reparent an object across thread boundaries. FilePaneWidget
    // owns its lifetime manually from here via setBackend()'s deleteLater +
    // quit()/wait() teardown path instead of the usual QObject parent-child chain.
    auto *backend = new SftpBackend(creds, m_hostKeyVerifier);
    auto *thread = new QThread(this);   // the QThread *controller* object is fine
                                         // to parent normally — it's backend that
                                         // can't be, since IT is what moves.
    backend->moveToThread(thread);

    statusBar()->showMessage(tr("Connecting to %1...").arg(creds.host));
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
