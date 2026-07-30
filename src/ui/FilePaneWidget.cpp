#include "FilePaneWidget.h"
#include "FileTreeView.h"
#include "IconTheme.h"

#include <QLineEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
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
    , m_backButton(nullptr)
    , m_forwardButton(nullptr)
    , m_upButton(nullptr)
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

    updatePathBarIcon();
    resetHistory();

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

    // Back/forward/up, matching any ordinary file manager. Neutral gray
    // rather than a semantic accent color — these are generic navigation,
    // not upload/download/connect-style actions, so they deliberately sit
    // outside the four-color system the rest of the icon set follows.
    m_backButton = new QToolButton(this);
    m_backButton->setIcon(IconTheme::tintedIcon(":/icons/arrow-left.svg", IconTheme::Gray));
    m_backButton->setToolTip(tr("Back"));
    m_backButton->setEnabled(false);
    connect(m_backButton, &QToolButton::clicked, this, &FilePaneWidget::goBack);

    m_forwardButton = new QToolButton(this);
    m_forwardButton->setIcon(IconTheme::tintedIcon(":/icons/arrow-right.svg", IconTheme::Gray));
    m_forwardButton->setToolTip(tr("Forward"));
    m_forwardButton->setEnabled(false);
    connect(m_forwardButton, &QToolButton::clicked, this, &FilePaneWidget::goForward);

    m_upButton = new QToolButton(this);
    m_upButton->setIcon(IconTheme::tintedIcon(":/icons/corner-left-up.svg", IconTheme::Gray));
    m_upButton->setToolTip(tr("Up one level"));
    connect(m_upButton, &QToolButton::clicked, this, &FilePaneWidget::goUp);

    m_pathBar = new QLineEdit(this);
    connect(m_pathBar, &QLineEdit::returnPressed, this, &FilePaneWidget::onPathBarReturnPressed);

    auto *pathRow = new QHBoxLayout;
    pathRow->addWidget(m_backButton);
    pathRow->addWidget(m_forwardButton);
    pathRow->addWidget(m_upButton);
    pathRow->addWidget(m_pathBar, 1);
    layout->addLayout(pathRow);

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

void FilePaneWidget::updatePathBarIcon()
{
    // ICON-MAP.md: local machine root = device-laptop (blue), remote
    // server root = server (green). QLineEdit::addAction(icon, Leading)
    // is what draws this inside the field itself rather than needing a
    // separate header row widget.
    if (m_pathBarLeadingIcon) {
        m_pathBar->removeAction(m_pathBarLeadingIcon);
        delete m_pathBarLeadingIcon;
        m_pathBarLeadingIcon = nullptr;
    }

    const QIcon icon = m_backend->isLocalFilesystem()
        ? IconTheme::tintedIcon(":/icons/device-laptop.svg", IconTheme::Blue)
        : IconTheme::tintedIcon(":/icons/server.svg", IconTheme::Green);
    m_pathBarLeadingIcon = m_pathBar->addAction(icon, QLineEdit::LeadingPosition);
}

QIcon FilePaneWidget::iconForEntry(const RemoteEntry &e)
{
    // ICON-MAP.md's file/folder type table. Not exhaustive — matches the
    // design package's own framing ("a starting map, not exhaustive").
    if (e.isDir) {
        // Dotfile-directory heuristic for "permission-restricted folder"
        // (the design's own example is .ssh) — simpler than actually
        // parsing the permissions string for missing group/world access,
        // and matches the visual intent (flag sensitive-looking dirs)
        // well enough for a re-skin pass.
        if (e.name.startsWith(QLatin1Char('.')))
            return IconTheme::tintedIcon(":/icons/lock.svg", IconTheme::Amber);
        return IconTheme::tintedIcon(":/icons/folder.svg", IconTheme::Gray);
    }

    const QString lower = e.name.toLower();
    if (lower.endsWith(QLatin1String(".pdf")))
        return IconTheme::tintedIcon(":/icons/file-type-pdf.svg", IconTheme::Red);
    if (lower.endsWith(QLatin1String(".sh")) || lower.endsWith(QLatin1String(".bash"))
        || lower.endsWith(QLatin1String(".zsh")))
        return IconTheme::tintedIcon(":/icons/terminal-2.svg", IconTheme::Green);
    if (lower.endsWith(QLatin1String(".zip")) || lower.endsWith(QLatin1String(".tar.gz"))
        || lower.endsWith(QLatin1String(".tgz")) || lower.endsWith(QLatin1String(".tar"))
        || lower.endsWith(QLatin1String(".gz")) || lower.endsWith(QLatin1String(".7z"))
        || lower.endsWith(QLatin1String(".rar")) || lower.endsWith(QLatin1String(".dll")))
        return IconTheme::tintedIcon(":/icons/file-zip.svg", IconTheme::Amber);
    if (lower.endsWith(QLatin1String(".exe")) || lower.endsWith(QLatin1String(".app")))
        return IconTheme::tintedIcon(":/icons/app-window.svg", IconTheme::Green);

    return IconTheme::tintedIcon(":/icons/file.svg", IconTheme::Gray);
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
        nameItem->setIcon(iconForEntry(e));
        auto *sizeItem = new QStandardItem(e.isDir ? QString() : QString::number(e.size));
        auto *modItem = new QStandardItem(e.modified.toString(Qt::ISODate));
        auto *permItem = new QStandardItem(e.permissions);
        m_model->appendRow({nameItem, sizeItem, modItem, permItem});
    }

    // History bookkeeping happens here — on a CONFIRMED successful
    // listing — rather than eagerly when navigateTo() is called, since a
    // navigation request can fail (bad path) and a failed one has no
    // business becoming a history entry.
    if (m_navigatingHistory) {
        // This listing is the result of goBack()/goForward(), which
        // already moved m_historyIndex — nothing further to do here.
    } else {
        // A fresh navigation (path bar, double-click, Up, initial
        // connect): drop any "forward" entries beyond the current
        // position, same convention every browser uses, then push. Skip
        // pushing a duplicate of the current top entry — Refresh
        // re-lists the same directory and shouldn't create a new step.
        while (m_history.size() > m_historyIndex + 1)
            m_history.removeLast();
        if (m_history.isEmpty() || m_history.last() != path) {
            m_history.append(path);
            m_historyIndex = m_history.size() - 1;
        }
    }
    m_navigatingHistory = false;

    updateNavigationButtonsEnabled();
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

void FilePaneWidget::goBack()
{
    if (m_historyIndex <= 0)
        return;   // m_backButton should already be disabled in this case — defensive, not load-bearing
    m_historyIndex--;
    m_navigatingHistory = true;
    navigateTo(m_history.at(m_historyIndex));
}

void FilePaneWidget::goForward()
{
    if (m_historyIndex >= m_history.size() - 1)
        return;
    m_historyIndex++;
    m_navigatingHistory = true;
    navigateTo(m_history.at(m_historyIndex));
}

void FilePaneWidget::goUp()
{
    navigateTo(parentOfPath(currentDirectory()));
}

QString FilePaneWidget::parentOfPath(const QString &path)
{
    QString clean = path;
    if (clean.length() > 1 && clean.endsWith(QLatin1Char('/')))
        clean.chop(1);

    const int lastSlash = clean.lastIndexOf(QLatin1Char('/'));
    if (lastSlash < 0)
        return path;   // no slash at all (e.g. a bare Windows drive letter) — nothing to go up to, stay put
    if (lastSlash == 0)
        return QStringLiteral("/");   // parent of "/something" is root

    QString parent = clean.left(lastSlash);

    // Windows drive-root special case: "C:/Users" has its last '/' right
    // after "C:", so the naive result is the bare string "C:" — which
    // Windows interprets as "the process's current directory on drive C"
    // (a legacy per-drive-working-directory quirk), NOT the drive's
    // actual root. Confirmed as a real bug: pressing Up from a top-level
    // folder landed wherever the app happened to launch from instead of
    // "C:\". An explicit trailing slash disambiguates it to the real,
    // absolute drive root — matches what "C:/" already does one Up press
    // later (a safe no-op, since chopping-then-searching-for-'/' on "C:"
    // finds none, so this function returns the input unchanged there).
    if (parent.length() == 2 && parent.at(1) == QLatin1Char(':'))
        parent += QLatin1Char('/');

    return parent;
}

void FilePaneWidget::updateNavigationButtonsEnabled()
{
    m_backButton->setEnabled(canGoBack());
    m_forwardButton->setEnabled(canGoForward());
    // Up has no history dependency — parentOfPath() is a safe no-op at
    // any root, so it's simplest to just always leave it enabled rather
    // than trying to detect "already at root" ahead of time.
}

void FilePaneWidget::resetHistory()
{
    m_history.clear();
    m_historyIndex = -1;
    m_navigatingHistory = false;
    // Buttons may not exist yet the very first time this runs (called
    // from setBackend() during the constructor, before buildUi() —
    // actually after, since buildUi() runs first in the constructor, but
    // defensive nullptr checks cost nothing and this order could
    // plausibly change later).
    if (m_backButton)
        updateNavigationButtonsEnabled();
}
