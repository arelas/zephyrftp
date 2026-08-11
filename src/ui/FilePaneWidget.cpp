#include "FilePaneWidget.h"
#include "FileTreeView.h"
#include "IconTheme.h"
#include "../AppSettings.h"

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
#include <QInputDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QItemSelection>
#include <algorithm>

namespace {
constexpr int ColName = 0;
constexpr int ColSize = 1;
constexpr int ColModified = 2;
constexpr int ColPermissions = 3;

// Reused across two unrelated purposes on two different columns' items —
// harmless, since QStandardItem role data is per-item, not global: on
// ColName it holds the entry's real (unbracketed) name, so a sorted row
// can still be matched back to its RemoteEntry; on ColSize it holds the
// byte count as a real qint64 for numeric sorting (see SizeItem below).
constexpr int SortDataRole = Qt::UserRole + 1;

// ColName only — whether the row is a directory, for NameItem's own
// folders-first comparator below. A separate role from SortDataRole
// since that one already holds the real name (a QString), needed for
// entryForRow()'s lookup regardless of whether NameItem also uses it.
constexpr int SortIsDirRole = Qt::UserRole + 2;

// Size is displayed as a plain, unpadded byte count (QString::number()),
// which sorts wrong lexicographically ("10" < "9"). Modified's ISO-8601
// text sorts correctly as a plain string (fixed-width, so lexicographic
// order already matches chronological order) and Permissions is meant
// to sort as the text shown, so only Size and Name need a real sort key
// of their own — stashed in SortDataRole since QStandardItem's default
// operator< compares DisplayRole text.
class SizeItem : public QStandardItem {
public:
    using QStandardItem::QStandardItem;
    bool operator<(const QStandardItem &other) const override
    {
        return data(SortDataRole).toLongLong() < other.data(SortDataRole).toLongLong();
    }
};

// A real bug found by code review: clicking the Name header used to sort
// on QStandardItem's default comparator — DisplayRole text, i.e. the
// bracketed "[Folder]" display string, not the real name, and with no
// folders-first grouping and no locale awareness (QString::operator<'s
// plain code-point order). Both real problems: '[' sits between
// uppercase and lowercase ASCII, so folders interleaved inconsistently
// with files instead of staying grouped; and code-point order doesn't
// match what QLocale-aware comparison (what rebuildModel()'s own default
// listing order already uses) would produce for anything outside plain
// ASCII. Same folders-first-via-sort-key convention as SizeItem above
// (a directory just sorts as "smaller" and flips to the end on a
// descending click, exactly like SizeItem's own dirs-first-ascending
// behavior) rather than pinning folders first unconditionally — matching
// what a header click's usual ascending/descending toggle should do.
class NameItem : public QStandardItem {
public:
    using QStandardItem::QStandardItem;
    bool operator<(const QStandardItem &other) const override
    {
        const bool thisIsDir = data(SortIsDirRole).toBool();
        const bool otherIsDir = other.data(SortIsDirRole).toBool();
        if (thisIsDir != otherIsDir)
            return thisIsDir;
        return data(SortDataRole).toString().localeAwareCompare(
                   other.data(SortDataRole).toString()) < 0;
    }
};

// Same logic as TransferManager.cpp's identical (file-local, not shared)
// helper — small enough that duplicating it here beats introducing a
// shared-utility header for one three-line function.
QString joinPath(const QString &dir, const QString &name)
{
    return dir.endsWith('/') ? dir + name : dir + '/' + name;
}
}

FilePaneWidget::FilePaneWidget(RemoteBackend *backend, QWidget *parent, AppSettings *settings)
    : QWidget(parent)
    , m_backend(nullptr)
    , m_view(nullptr)
    , m_backButton(nullptr)
    , m_forwardButton(nullptr)
    , m_upButton(nullptr)
    , m_homeButton(nullptr)
    , m_pathBar(nullptr)
    , m_statusLabel(nullptr)
    , m_model(new QStandardItemModel(this))
    , m_settings(settings)
{
    buildUi();
    setBackend(backend, nullptr);

    // Connected once here, not in setBackend() — setBackend() runs again
    // every time the backend is swapped (connect/disconnect/reconnect),
    // and re-adding this connection each time would fire rebuildModel()
    // once per past connection instead of once.
    if (m_settings) {
        connect(m_settings, &AppSettings::showHiddenFilesChanged, this, [this](bool) {
            rebuildModel();
        });
    }
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
    // Only a thread-owning backend's connectToHost() can actually block
    // long enough to matter — LocalBackend's runs synchronously on the GUI
    // thread and returns immediately either way, so there's nothing for a
    // caller to usefully wait out. See isConnecting()'s own doc comment.
    m_connecting = (m_backendThread != nullptr);

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
        m_connecting = false;
        // A failed listDirectory() (bad path) reports here too, NOT via
        // directoryListed() — see m_navigationInFlight's own doc comment.
        // Without this, a failed navigation would leave the flag stuck
        // true forever, silently refusing every navigation attempt after
        // the first bad path.
        m_navigationInFlight = false;
        // A real bug found by code review: a failed goBack()/goForward()
        // left m_navigatingHistory stuck true too — only onDirectoryListed()
        // ever reset it (on a CONFIRMED successful listing), and a failed
        // navigation never reaches that. The next successful fresh
        // navigation (path bar, double-click, Up) would then wrongly take
        // onDirectoryListed()'s "this was Back/Forward" branch and skip
        // pushing itself onto history at all, desyncing m_history/
        // m_historyIndex from what's actually displayed for the rest of
        // the pane's session.
        m_navigatingHistory = false;
        m_statusLabel->setText(QStringLiteral("Error: %1").arg(reason));
    });
    connect(m_backend, &RemoteBackend::connected, this, [this]() {
        m_connecting = false;
        // Through navigateTo() (not a raw invokeMethod call) so this
        // initial listing also holds m_navigationInFlight — otherwise a
        // Back/Forward/Up/path-bar action fired in the brief window before
        // this first response arrives could overlap it, the same race
        // m_navigationInFlight exists to prevent elsewhere.
        navigateTo(QString());
    });
    connect(m_backend, &RemoteBackend::fileOperationFailed,
            this, &FilePaneWidget::onFileOperationFailed);
    connect(m_backend, &RemoteBackend::commandLogged,
            this, &FilePaneWidget::commandLogged);

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

    m_homeButton = new QToolButton(this);
    m_homeButton->setIcon(IconTheme::tintedIcon(":/icons/home.svg", IconTheme::Gray));
    m_homeButton->setToolTip(tr("Home"));
    connect(m_homeButton, &QToolButton::clicked, this, &FilePaneWidget::goHome);

    m_pathBar = new QLineEdit(this);
    connect(m_pathBar, &QLineEdit::returnPressed, this, &FilePaneWidget::onPathBarReturnPressed);

    auto *pathRow = new QHBoxLayout;
    pathRow->addWidget(m_backButton);
    pathRow->addWidget(m_forwardButton);
    pathRow->addWidget(m_upButton);
    pathRow->addWidget(m_homeButton);
    pathRow->addWidget(m_pathBar, 1);
    layout->addLayout(pathRow);

    m_model->setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Modified"), tr("Permissions")});

    m_view = new FileTreeView(this, this);
    m_view->setModel(m_model);
    m_view->setRootIsDecorated(false);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Interactive like every other column — Stretch here used to make
    // Name's width purely a side effect of dragging the OTHER columns'
    // handles, with no handle of its own to drag. QTreeView's own
    // stretchLastSection default (Permissions, the actual last column)
    // is what should absorb leftover width instead.
    m_view->header()->setSectionResizeMode(ColName, QHeaderView::Interactive);
    m_view->setColumnWidth(ColName, 220);
    // Same real bug, same fix as TransferQueueWidget's identical File
    // column: with no upper bound, dragging Name wide enough pushes
    // Size/Modified/Permissions past the visible area, unreachable
    // (stretchLastSection actively avoids a horizontal scrollbar).
    // QHeaderView has no per-section maximum, only this header-wide
    // one — acceptable since Name is the only column anyone actually
    // drags wider in practice.
    m_view->header()->setMaximumSectionSize(500);
    // Clicking a header sorts by that column; QHeaderView/QTreeView's own
    // built-in click handling toggles ascending/descending on a second
    // click of the same section — no extra wiring needed for that part.
    // Row lookups elsewhere (onRowDoubleClicked, selectedEntries(), ...)
    // go through entryForRow() rather than indexing m_currentEntries by
    // row position, since sorting is exactly what breaks that assumption.
    m_view->setSortingEnabled(true);
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
    // No explicit disconnect needed for the OLD icon's connection below —
    // it's deleted a few lines up along with the QAction itself every time
    // this method runs (backend swap), which tears down its connections too.
    connect(m_pathBarLeadingIcon, &QAction::triggered, this, &FilePaneWidget::onPathBarIconClicked);
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
    // See m_navigationInFlight's own doc comment for the real race this
    // prevents. Callers that mutate other state before calling this
    // (goBack()/goForward()'s m_historyIndex) check the flag THEMSELVES
    // first, so they never touch that state for a request that ends up
    // refused here.
    if (m_navigationInFlight)
        return;
    m_navigationInFlight = true;
    QMetaObject::invokeMethod(m_backend, "listDirectory",
                               Qt::QueuedConnection, Q_ARG(QString, path));
}

void FilePaneWidget::onPathBarReturnPressed()
{
    // A real bug found by code review: unlike goBack()/goForward(), which
    // check m_navigationInFlight THEMSELVES before doing anything, this
    // used to just call navigateTo() unconditionally and rely entirely on
    // its own internal guard — safe (never corrupts state), but silent.
    // Pressing Enter on a freshly typed path while a prior navigation is
    // still resolving got dropped with zero feedback, and looked actively
    // broken once that earlier navigation's response arrived:
    // onDirectoryListed() overwrites the path bar with ITS path,
    // silently erasing whatever the person had just typed with nothing
    // to explain why.
    if (m_navigationInFlight) {
        m_statusLabel->setText(tr("A navigation is already in progress — try again once it finishes."));
        return;
    }
    navigateTo(m_pathBar->text());
}

void FilePaneWidget::rebuildModel()
{
    // A real bug: this used to clear and rebuild the model unconditionally
    // with no attempt to preserve the current selection, so toggling "Show
    // hidden files" or hitting Refresh mid-selection (both re-enter here
    // for the SAME directory, not just a genuine navigation to a
    // different one) silently dropped whatever was selected — losing the
    // target set of a pending Transfer/Move with no indication why.
    // Captured by NAME rather than row/index, since sorting and filtering
    // below both change row positions and this needs to survive either.
    // A second real bug found by code review, in this same restore: it
    // used to also fire for a genuine navigation to a DIFFERENT
    // directory, on the assumption that an old name "essentially never"
    // matches an unrelated directory's listing — false for common names
    // (README.md, .gitignore, index.js, __init__.py), which silently
    // auto-selected a same-named file with no user action. Captured here
    // (before m_entriesDirectory is updated below) so it can be compared
    // against the new directory once rebuildModel() knows what that is.
    QStringList previouslySelectedNames;
    for (const QModelIndex &index : m_view->selectionModel()->selectedRows(ColName)) {
        if (const RemoteEntry *entry = entryForRow(index.row()))
            previouslySelectedNames.append(entry->name);
    }
    const bool sameDirectoryAsBefore = (m_entriesDirectory == currentDirectory());
    m_entriesDirectory = currentDirectory();

    // Real, existing inconsistency this filter also fixes: LocalBackend
    // has always excluded dotfiles outright (its QDir::entryInfoList()
    // call never passes QDir::Hidden), while SftpBackend/FtpBackend only
    // ever excluded "."/".." and always showed every other dotfile — two
    // panes disagreeing about what "the directory listing" even means,
    // with no way to change either. Filtering centrally here, once,
    // applies the same rule to every backend uniformly.
    const bool showHidden = m_settings && m_settings->showHiddenFiles();

    QList<RemoteEntry> filtered;
    for (const RemoteEntry &e : m_lastRawEntries) {
        if (showHidden || !e.name.startsWith(QLatin1Char('.')))
            filtered.append(e);
    }

    // Default listing order: folders first, then name descending — applied
    // uniformly here rather than left to each backend, since only
    // LocalBackend ever sorted its own results (QDir::DirsFirst |
    // QDir::Name); SftpBackend/FtpBackend returned whatever order the
    // server happened to list in. Sorted by the entry's real name, not
    // nameItem's later "[folder]" display text, for the same reason
    // entryForRow() looks entries up by real name below. A later header
    // click overrides this via the ordinary column-sort machinery further
    // down — this only decides what a FRESH listing looks like before
    // that's ever happened.
    std::stable_sort(filtered.begin(), filtered.end(), [](const RemoteEntry &a, const RemoteEntry &b) {
        if (a.isDir != b.isDir)
            return a.isDir;
        return a.name.localeAwareCompare(b.name) > 0;
    });

    m_currentEntries.clear();
    m_model->removeRows(0, m_model->rowCount());
    for (const RemoteEntry &e : filtered) {
        m_currentEntries.append(e);
        auto *nameItem = new NameItem(e.isDir ? QStringLiteral("[%1]").arg(e.name) : e.name);
        nameItem->setIcon(iconForEntry(e));
        // entryForRow() looks entries up by this rather than row position —
        // sorting physically reorders m_model's rows, so row index alone
        // no longer identifies which RemoteEntry a row belongs to. Also
        // NameItem's own sort key (see its class doc comment) — the real
        // name, not the bracketed "[Folder]" display text above.
        nameItem->setData(e.name, SortDataRole);
        nameItem->setData(e.isDir, SortIsDirRole);
        auto *sizeItem = new SizeItem(e.isDir ? QString() : QString::number(e.size));
        sizeItem->setData(e.isDir ? qint64(-1) : e.size, SortDataRole);
        auto *modItem = new QStandardItem(e.modified.toString(Qt::ISODate));
        auto *permItem = new QStandardItem(e.permissions);
        m_model->appendRow({nameItem, sizeItem, modItem, permItem});
    }
    m_statusLabel->setText(tr("%1 items").arg(m_currentEntries.size()));

    if (sameDirectoryAsBefore && !previouslySelectedNames.isEmpty()) {
        QItemSelection restoredSelection;
        for (int row = 0; row < m_model->rowCount(); ++row) {
            const RemoteEntry *entry = entryForRow(row);
            if (entry && previouslySelectedNames.contains(entry->name)) {
                restoredSelection.select(m_model->index(row, 0),
                                          m_model->index(row, m_model->columnCount() - 1));
            }
        }
        if (!restoredSelection.isEmpty()) {
            m_view->selectionModel()->select(
                restoredSelection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
    }
}

void FilePaneWidget::onDirectoryListed(const QString &path, const QList<RemoteEntry> &entries)
{
    // See m_pendingFileOpRefreshes's own doc comment: this directoryListed
    // is a delete/rename/create's own "fire and refresh", not a response
    // to navigateTo() — update the display but leave m_navigationInFlight
    // and history untouched, since neither belongs to this dispatch.
    if (m_pendingFileOpRefreshes > 0) {
        --m_pendingFileOpRefreshes;
        m_lastRawEntries = entries;
        rebuildModel();
        return;
    }

    m_navigationInFlight = false;
    m_lastRawEntries = entries;
    m_pathBar->setText(path);
    rebuildModel();

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

const RemoteEntry *FilePaneWidget::entryForRow(int row) const
{
    if (row < 0 || row >= m_model->rowCount())
        return nullptr;
    const QStandardItem *nameItem = m_model->item(row, ColName);
    if (!nameItem)
        return nullptr;
    const QString name = nameItem->data(SortDataRole).toString();
    for (const RemoteEntry &e : m_currentEntries) {
        if (e.name == name)
            return &e;
    }
    return nullptr;
}

void FilePaneWidget::onRowDoubleClicked(const QModelIndex &index)
{
    const RemoteEntry *entry = index.isValid() ? entryForRow(index.row()) : nullptr;
    if (!entry)
        return;

    if (entry->isDir) {
        const QString base = currentDirectory();
        const QString next = base.endsWith('/') ? base + entry->name : base + '/' + entry->name;
        navigateTo(next);
    } else {
        emit fileActivated(entry->name);
    }
}

QString FilePaneWidget::selectedEntryName() const
{
    const auto selected = m_view->selectionModel()->selectedRows(ColName);
    if (selected.isEmpty())
        return {};
    const RemoteEntry *entry = entryForRow(selected.first().row());
    return entry ? entry->name : QString();
}

QStringList FilePaneWidget::selectedFileNames() const
{
    QStringList names;
    const auto selected = m_view->selectionModel()->selectedRows(ColName);
    for (const QModelIndex &index : selected) {
        const RemoteEntry *entry = entryForRow(index.row());
        if (entry && !entry->isDir)   // directories skipped — see filesActivated's doc comment
            names.append(entry->name);
    }
    return names;
}

QList<RemoteEntry> FilePaneWidget::selectedEntries() const
{
    QList<RemoteEntry> entries;
    const auto selected = m_view->selectionModel()->selectedRows(ColName);
    for (const QModelIndex &index : selected) {
        if (const RemoteEntry *entry = entryForRow(index.row()))
            entries.append(*entry);
    }
    return entries;
}

void FilePaneWidget::showContextMenu(const QPoint &pos)
{
    const QList<RemoteEntry> selected = selectedEntries();  // files + folders
    // A real bug found by code review: promptAndRename()/confirmAndDelete()
    // etc. used to call currentDirectory() themselves, AFTER their own
    // modal QInputDialog/QMessageBox had already closed — but menu.exec()
    // just below is ALSO a nested event loop, and either one still pumps
    // the queued, cross-thread directoryListed signal a genuinely
    // in-flight navigation (started just before this right-click) can
    // deliver while the menu or dialog is still open. currentDirectory()
    // read AFTER any of that could then return a DIFFERENT directory than
    // the one `selected` above was actually drawn from, silently
    // dispatching a create/rename/delete against the wrong location.
    // Captured once, here, at the same moment as `selected` — both are a
    // consistent snapshot of "what was actually on screen when this menu
    // was invoked" — and threaded through to every action below instead
    // of each calling currentDirectory() again later. Refresh is the one
    // deliberate exception: it wants whatever's CURRENT, not this snapshot.
    const QString directory = currentDirectory();

    QMenu menu(this);

    QAction *transferAction = menu.addAction(
        selected.size() > 1 ? tr("Transfer %1 Items to Other Pane").arg(selected.size())
                             : tr("Transfer to Other Pane"));
    transferAction->setEnabled(!selected.isEmpty());

    // Always visible/enabled whenever there's a selection, same as
    // Transfer above — not grayed out based on live eligibility (this
    // pane has no way to query the OTHER pane's backend; see
    // moveRequested()'s doc comment). MainWindow shows an explanatory
    // message if the panes turn out not to be move-eligible.
    QAction *moveAction = menu.addAction(
        selected.size() > 1 ? tr("Move %1 Items to Other Pane").arg(selected.size())
                             : tr("Move to Other Pane"));
    moveAction->setEnabled(!selected.isEmpty());

    menu.addSeparator();
    QAction *newFileAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/file-plus.svg", IconTheme::Green), tr("New File..."));
    QAction *newFolderAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/folder-plus.svg", IconTheme::Green), tr("New Folder..."));

    menu.addSeparator();
    QAction *renameAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/edit.svg", IconTheme::Blue), tr("Rename..."));
    renameAction->setEnabled(selected.size() == 1);   // renaming several items to one name doesn't make sense

    QAction *deleteAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/trash.svg", IconTheme::Red),
        selected.size() > 1 ? tr("Delete %1 Items").arg(selected.size()) : tr("Delete"));
    deleteAction->setEnabled(!selected.isEmpty());

    menu.addSeparator();
    QAction *refreshAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/refresh.svg", IconTheme::Amber), tr("Refresh"));

    QAction *chosen = menu.exec(m_view->viewport()->mapToGlobal(pos));

    if (chosen == transferAction)
        emit filesActivated(selected);
    else if (chosen == moveAction)
        emit moveRequested(selected);
    else if (chosen == newFileAction)
        promptAndCreateFile(directory);
    else if (chosen == newFolderAction)
        promptAndCreateFolder(directory);
    else if (chosen == renameAction)
        promptAndRename(selected.first(), directory);
    else if (chosen == deleteAction)
        confirmAndDelete(selected, directory);
    else if (chosen == refreshAction)
        navigateTo(currentDirectory());   // deliberately fresh — see this function's own doc comment
}

void FilePaneWidget::onPathBarIconClicked()
{
    // Same icons/colors as the global toolbar's Connect/Sites/Disconnect
    // (see MainWindow::buildToolbar()) — this is the per-pane equivalent,
    // not a different action, so it should look like one.
    QMenu menu(this);
    QAction *sitesAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/server-cog.svg", IconTheme::Blue), tr("Sites..."));
    QAction *connectAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/plug.svg", IconTheme::Green), tr("Connect..."));
    QAction *disconnectAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/plug-connected-x.svg", IconTheme::Red), tr("Disconnect"));
    // Present but disabled when already local, same convention
    // showContextMenu() already uses for renameAction/deleteAction's
    // conditional enablement — rather than hiding it outright.
    disconnectAction->setEnabled(!m_backend->isLocalFilesystem());

    QAction *chosen = menu.exec(m_pathBar->mapToGlobal(QPoint(0, m_pathBar->height())));

    // No dialog construction, no backend knowledge here — purely an
    // intent signal, same shape as promptAndCreateFile() etc. prompting
    // locally but never constructing a concrete backend itself.
    // MainWindow owns startConnection()/setBackend() and is the only
    // place in the UI layer that names a concrete backend type.
    if (chosen == sitesAction)
        emit siteManagerRequested(this);
    else if (chosen == connectAction)
        emit connectRequested(this);
    else if (chosen == disconnectAction)
        emit disconnectRequested(this);
}

void FilePaneWidget::promptAndCreateFile(const QString &directory)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New File"), tr("File name:"),
                                                QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;

    const QString path = joinPath(directory, name.trimmed());
    ++m_pendingFileOpRefreshes;   // see its own doc comment
    QMetaObject::invokeMethod(m_backend, "createFile", Qt::QueuedConnection, Q_ARG(QString, path));
}

void FilePaneWidget::promptAndCreateFolder(const QString &directory)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New Folder"), tr("Folder name:"),
                                                QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;

    const QString path = joinPath(directory, name.trimmed());
    ++m_pendingFileOpRefreshes;   // see its own doc comment
    QMetaObject::invokeMethod(m_backend, "createDirectory", Qt::QueuedConnection, Q_ARG(QString, path));
}

void FilePaneWidget::promptAndRename(const RemoteEntry &entry, const QString &directory)
{
    bool ok = false;
    const QString newName = QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                                   QLineEdit::Normal, entry.name, &ok);
    if (!ok || newName.trimmed().isEmpty() || newName.trimmed() == entry.name)
        return;

    const QString oldPath = joinPath(directory, entry.name);
    const QString newPath = joinPath(directory, newName.trimmed());
    ++m_pendingFileOpRefreshes;   // see its own doc comment
    QMetaObject::invokeMethod(m_backend, "renameEntry", Qt::QueuedConnection,
                               Q_ARG(QString, oldPath), Q_ARG(QString, newPath));
}

void FilePaneWidget::confirmAndDelete(const QList<RemoteEntry> &entries, const QString &directory)
{
    if (entries.isEmpty())
        return;

    const QString message = entries.size() == 1
        ? tr("Delete \"%1\"? This can't be undone.").arg(entries.first().name)
        : tr("Delete %1 items? This can't be undone.").arg(entries.size());
    const auto reply = QMessageBox::question(this, tr("Delete"), message,
                                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    // One invokeMethod() call per entry, queued in order — the backend
    // lives on (at most) one thread of its own, so Qt's queue processes
    // these strictly one at a time; each delete's own internal refresh
    // (see RemoteBackend::deleteEntry()'s doc comment) completes before
    // the next deletion starts. No race between them.
    for (const RemoteEntry &entry : entries) {
        const QString path = joinPath(directory, entry.name);
        ++m_pendingFileOpRefreshes;   // see its own doc comment — one per entry, each gets its own refresh
        QMetaObject::invokeMethod(m_backend, "deleteEntry", Qt::QueuedConnection,
                                   Q_ARG(QString, path), Q_ARG(bool, entry.isDir));
    }
}

void FilePaneWidget::onFileOperationFailed(const QString &operation, const QString &path, const QString &reason)
{
    // A failed dispatch never produces the directoryListed refresh
    // onDirectoryListed() would otherwise consume — decrement here so
    // m_pendingFileOpRefreshes doesn't stay permanently inflated and start
    // misclassifying a later, unrelated navigation response as a refresh.
    if (m_pendingFileOpRefreshes > 0)
        --m_pendingFileOpRefreshes;
    QMessageBox::warning(this, operation,
                          tr("%1 failed for \"%2\":\n%3")
                              .arg(operation, QFileInfo(path).fileName(), reason));
}

void FilePaneWidget::goBack()
{
    if (m_historyIndex <= 0)
        return;   // m_backButton should already be disabled in this case — defensive, not load-bearing
    // Checked here, before mutating m_historyIndex, NOT left to
    // navigateTo()'s own guard below — a refused request must never have
    // already moved the index, or this pane's idea of "where history
    // currently points" desyncs from what's actually on screen. See
    // m_navigationInFlight's own doc comment.
    if (m_navigationInFlight)
        return;
    m_historyIndex--;
    m_navigatingHistory = true;
    navigateTo(m_history.at(m_historyIndex));
}

void FilePaneWidget::goForward()
{
    if (m_historyIndex >= m_history.size() - 1)
        return;
    if (m_navigationInFlight)
        return;
    m_historyIndex++;
    m_navigatingHistory = true;
    navigateTo(m_history.at(m_historyIndex));
}

void FilePaneWidget::goUp()
{
    navigateTo(parentOfPath(currentDirectory()));
}

void FilePaneWidget::goHome()
{
    // m_history[0] IS the home directory, by construction: resetHistory()
    // clears it on every setBackend() call, and the very next successful
    // listing — connectToHost()'s own connected()->navigateTo(QString())
    // (see setBackend()'s own comment) — is always a "fresh" navigation,
    // never a back/forward one, so onDirectoryListed() pushes it as the
    // first entry. That's QDir::homePath() for LocalBackend (its
    // m_currentPath starts there) and the server's resolved PWD/starting
    // directory for SFTP/FTP (see ensureConnected()/ensureSession()'s own
    // "useHomeDirectory" handling). No separate backend-level "home path"
    // concept needed — this reuses history state that already exists.
    if (m_history.isEmpty())
        return;   // still connecting; nothing to go home to yet
    if (m_navigationInFlight)
        return;
    navigateTo(m_history.first());
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
    // Called from setBackend() as part of swapping to a new backend — the
    // OLD backend (and any navigateTo() request still outstanding against
    // it) is being torn down right here, so its response can never arrive
    // to clear this itself. Without resetting it, a navigation left
    // in-flight at swap time would permanently block the new backend's
    // very first navigateTo() call too.
    m_navigationInFlight = false;
    // Same reasoning as m_navigationInFlight just above — any file op
    // dispatched against the OLD backend can never deliver its refresh or
    // failure signal once it's been torn down (setBackend() disconnects
    // it first), so the count would otherwise survive the swap and
    // misclassify the new backend's first real navigation response.
    m_pendingFileOpRefreshes = 0;
    // Buttons may not exist yet the very first time this runs (called
    // from setBackend() during the constructor, before buildUi() —
    // actually after, since buildUi() runs first in the constructor, but
    // defensive nullptr checks cost nothing and this order could
    // plausibly change later).
    if (m_backButton)
        updateNavigationButtonsEnabled();
}
