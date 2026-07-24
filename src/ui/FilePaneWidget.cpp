#include "FilePaneWidget.h"
#include "FileTreeView.h"

#include <QLineEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QDir>
#include <QThread>
#include <QMetaObject>
#include <QMenu>
#include <QAction>

namespace {
constexpr int ColName = 0;
constexpr int ColSize = 1;
constexpr int ColModified = 2;
constexpr int ColPermissions = 3;
}

FilePaneWidget::FilePaneWidget(RemoteBackend *backend, QWidget *parent)
    : QWidget(parent)
    , m_backend(nullptr)
    , m_view(nullptr)
    , m_pathBar(nullptr)
    , m_statusLabel(nullptr)
    , m_model(new QStandardItemModel(this))
{
    buildUi();
    setBackend(backend, nullptr);
}

void FilePaneWidget::setBackend(RemoteBackend *backend, QThread *thread)
{
    if (m_backend) {
        disconnect(m_backend, nullptr, this, nullptr);

        // deleteLater() posts a deferred-delete event to the backend's OWN
        // thread's queue. If that's a worker thread, quit()/wait() below only
        // stop the loop after everything already queued (including this
        // delete) has run — that ordering is what makes this safe. Deleting
        // the QThread object itself only happens once its loop has actually
        // stopped, hence the `delete m_backendThread` after wait() returns.
        m_backend->deleteLater();
        if (m_backendThread) {
            m_backendThread->quit();
            m_backendThread->wait();
            delete m_backendThread;
        }
    }

    m_backend = backend;
    m_backendThread = thread;

    if (!m_backendThread) {
        // No thread affinity of its own (e.g. LocalBackend) — parent it to
        // this widget so normal QObject cleanup handles it. A backend with a
        // thread must NOT be parented to a GUI-thread widget; Qt disallows
        // reparenting across thread boundaries, so its lifetime is managed
        // manually above instead.
        m_backend->setParent(this);
    }

    connect(m_backend, &RemoteBackend::directoryListed,
            this, &FilePaneWidget::onDirectoryListed);
    connect(m_backend, &RemoteBackend::connectionFailed, this, [this](const QString &reason) {
        m_statusLabel->setText(QStringLiteral("Error: %1").arg(reason));
    });
    connect(m_backend, &RemoteBackend::connected, this, [this]() {
        QMetaObject::invokeMethod(m_backend, "listDirectory",
                                   Qt::QueuedConnection, Q_ARG(QString, QString()));
    });

    if (m_backendThread)
        m_backendThread->start();

    // Queued even when the backend has no thread of its own: connectToHost()
    // still shouldn't run synchronously inside setBackend() itself.
    QMetaObject::invokeMethod(m_backend, "connectToHost", Qt::QueuedConnection);
}

void FilePaneWidget::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    m_pathBar = new QLineEdit(this);
    connect(m_pathBar, &QLineEdit::returnPressed, this, &FilePaneWidget::onPathBarReturnPressed);
    layout->addWidget(m_pathBar);

    m_model->setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Modified"), tr("Permissions")});

    m_view = new FileTreeView(this, this);
    m_view->setModel(m_model);
    m_view->setRootIsDecorated(false);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->header()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    connect(m_view, &QTreeView::doubleClicked, this, &FilePaneWidget::onRowDoubleClicked);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QTreeView::customContextMenuRequested, this, &FilePaneWidget::showContextMenu);
    connect(m_view, &FileTreeView::filesDroppedFrom, this, &FilePaneWidget::filesDropped);
    layout->addWidget(m_view);

    m_statusLabel = new QLabel(this);
    layout->addWidget(m_statusLabel);
}

void FilePaneWidget::navigateTo(const QString &path)
{
    QMetaObject::invokeMethod(m_backend, "listDirectory",
                               Qt::QueuedConnection, Q_ARG(QString, path));
}

void FilePaneWidget::onPathBarReturnPressed()
{
    navigateTo(m_pathBar->text());
}

void FilePaneWidget::onDirectoryListed(const QString &path, const QList<RemoteEntry> &entries)
{
    m_currentEntries = entries;
    m_pathBar->setText(path);
    m_statusLabel->setText(tr("%1 items").arg(entries.size()));

    m_model->removeRows(0, m_model->rowCount());
    for (const RemoteEntry &e : entries) {
        auto *nameItem = new QStandardItem(e.isDir ? QStringLiteral("[%1]").arg(e.name) : e.name);
        auto *sizeItem = new QStandardItem(e.isDir ? QString() : QString::number(e.size));
        auto *modItem = new QStandardItem(e.modified.toString(Qt::ISODate));
        auto *permItem = new QStandardItem(e.permissions);
        m_model->appendRow({nameItem, sizeItem, modItem, permItem});
    }
}

void FilePaneWidget::onRowDoubleClicked(const QModelIndex &index)
{
    if (!index.isValid() || index.row() >= m_currentEntries.size())
        return;

    const RemoteEntry &entry = m_currentEntries.at(index.row());
    if (entry.isDir) {
        const QString base = currentDirectory();
        const QString next = base.endsWith('/') ? base + entry.name : base + '/' + entry.name;
        navigateTo(next);
    } else {
        emit fileActivated(entry.name);
    }
}

QString FilePaneWidget::selectedEntryName() const
{
    const auto selected = m_view->selectionModel()->selectedRows(ColName);
    if (selected.isEmpty())
        return {};
    const int row = selected.first().row();
    if (row < 0 || row >= m_currentEntries.size())
        return {};
    return m_currentEntries.at(row).name;
}

QStringList FilePaneWidget::selectedFileNames() const
{
    QStringList names;
    const auto selected = m_view->selectionModel()->selectedRows(ColName);
    for (const QModelIndex &index : selected) {
        const int row = index.row();
        if (row < 0 || row >= m_currentEntries.size())
            continue;
        const RemoteEntry &entry = m_currentEntries.at(row);
        if (!entry.isDir)   // directories skipped — see filesActivated's doc comment
            names.append(entry.name);
    }
    return names;
}

void FilePaneWidget::showContextMenu(const QPoint &pos)
{
    const QStringList names = selectedFileNames();

    QMenu menu(this);
    QAction *transferAction = menu.addAction(
        names.size() > 1 ? tr("Transfer %1 Files to Other Pane").arg(names.size())
                          : tr("Transfer to Other Pane"));
    transferAction->setEnabled(!names.isEmpty());

    QAction *chosen = menu.exec(m_view->viewport()->mapToGlobal(pos));
    if (chosen == transferAction)
        emit filesActivated(names);
}
