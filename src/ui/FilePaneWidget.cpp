#include "FilePaneWidget.h"
#include "FileTreeView.h"
#include "IconTheme.h"
#include "PermissionsDialog.h"
#include "../AppSettings.h"
#include "../PathUtils.h"

#include <QLineEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QHeaderView>
#include <QHash>
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
// which sorts wrong lexicographically ("10" < "9"). Modified's
// "yyyy-MM-dd hh:mm:ss" text sorts correctly as a plain string
// (fixed-width, so lexicographic order already matches chronological
// order — the space between date and time is the same character on
// every row, so it doesn't disturb that) and Permissions is meant
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
}

FilePaneWidget::FilePaneWidget(RemoteBackend *backend, QWidget *parent, AppSettings *settings,
                                TransferManager *transferManager)
    : QWidget(parent)
    , m_backend(nullptr)
    , m_view(nullptr)
    , m_backButton(nullptr)
    , m_forwardButton(nullptr)
    , m_upButton(nullptr)
    , m_homeButton(nullptr)
    , m_refreshButton(nullptr)
    , m_pathBar(nullptr)
    , m_filterEdit(nullptr)
    , m_statusLabel(nullptr)
    , m_model(new QStandardItemModel(this))
    , m_settings(settings)
    , m_transferManager(transferManager)
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
        // Same "connect once here, not in setBackend()" reasoning as
        // above — live Dark/Light switching, see retintIcons()'s own
        // doc comment.
        connect(m_settings, &AppSettings::themeChanged, this, [this](Theme) {
            retintIcons();
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

    // Pool members (if any) never outlive the primary connection they
    // were opened alongside — same teardown pattern as the primary
    // above, once per member. Any in-flight transfer still bound to one
    // of these (TransferItem::capturedTransferBackend) simply fails the
    // same way a primary-backend disconnect mid-transfer already does —
    // no special-cased resilience for pool members, matching this
    // feature's own documented non-goals. m_poolBackendFactory is reset
    // too: it closed over the OLD connection's credentials, which are
    // no longer valid for whatever setBackend() is (re)establishing now
    // — MainWindow::startConnection() calls configureTransferPool()
    // again, with a fresh factory, right after this returns, if the new
    // connection should keep pooling.
    for (RemoteBackend *poolBackend : std::as_const(m_poolBackends))
        poolBackend->deleteLater();
    for (QThread *poolThread : std::as_const(m_poolThreads)) {
        // A pool member built with thread=nullptr (its factory's own
        // choice — e.g. a LocalBackend pool member would never need
        // one, and this test file's own fakes don't either) is legitimate,
        // matching the primary's own if (m_backendThread) guard right
        // above — not every pool member necessarily has a thread to
        // tear down.
        if (!poolThread)
            continue;
        poolThread->quit();
        poolThread->wait();
        delete poolThread;
    }
    m_poolBackends.clear();
    m_poolThreads.clear();
    m_maxPoolSize = 1;
    m_poolBackendFactory = TransferBackendFactory();

    m_backend = backend;
    m_backendThread = thread;
    // Reset unconditionally on every swap, not just Disconnect — a
    // stale descriptor pointing at whatever was connected before would
    // otherwise survive into a new connection (or a plain LocalBackend)
    // until something explicitly overwrote it. MainWindow::
    // startConnection() sets a real one right after this call returns;
    // for every other setBackend() call (Disconnect, or a fresh pane at
    // startup) an empty descriptor is exactly correct.
    m_connectionDescriptor = ConnectionDescriptor();
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

void FilePaneWidget::configureTransferPool(int maxSize, TransferBackendFactory factory)
{
    // setBackend() already reset m_maxPoolSize/m_poolBackendFactory to
    // "no pooling" — this just installs the real values for the
    // connection setBackend() (called immediately before this, by
    // MainWindow::startConnection()) just established. maxSize < 1
    // would leave the primary itself unusable as a fallback; clamped up
    // to 1 rather than trusted, since 0 or negative isn't a meaningful
    // "pool size" a caller could have intended.
    m_maxPoolSize = qMax(1, maxSize);
    m_poolBackendFactory = std::move(factory);
}

RemoteBackend *FilePaneWidget::pickIdleTransferBackend(const std::function<bool(RemoteBackend *)> &isClaimed)
{
    if (m_backend && !isClaimed(m_backend))
        return m_backend;

    for (RemoteBackend *poolBackend : std::as_const(m_poolBackends)) {
        if (!isClaimed(poolBackend))
            return poolBackend;
    }

    // Every existing connection (primary + pool) is busy — grow the
    // pool by one more, but only up to the configured cap, and only if
    // a factory was actually configured (configureTransferPool() was
    // never called, or maxPoolSize is 1 — either way, "no pooling").
    // 1 (primary) + m_poolBackends.size() is the current total; a new
    // member is only worth building if that total is still under the
    // cap.
    if (!m_poolBackendFactory || 1 + m_poolBackends.size() >= m_maxPoolSize)
        return nullptr;

    const QPair<RemoteBackend *, QThread *> built = m_poolBackendFactory();
    RemoteBackend *newBackend = built.first;
    QThread *newThread = built.second;
    if (!newBackend)
        return nullptr;   // factory failed — treated the same as "pool exhausted," not an error

    // Same forwarding wiring setBackend() gives the primary — a real,
    // live-traffic pool connection's commands should show up in the
    // Commands pane the same as the primary's, not silently vanish just
    // because they're not the pane's "main" connection. Deliberately NOT
    // wired to directoryListed/connectionFailed/connected/
    // fileOperationFailed — pool members never browse, only transfer;
    // TransferManager talks to them directly via checkExists()/
    // downloadFile()/uploadFile() and their own transferProgress/
    // transferFinished/transferFailed signals, none of which route
    // through FilePaneWidget at all (see TransferManager::
    // ensureExistsCheckConnected()/dispatchActiveItem()).
    connect(newBackend, &RemoteBackend::commandLogged, this, &FilePaneWidget::commandLogged);

    if (newThread)
        newThread->start();
    // No explicit connectToHost() call — every real SftpBackend/
    // FtpBackend operation (checkExists(), downloadFile(), uploadFile())
    // starts with ensureSession()/ensureConnected(), an idempotent
    // lazy-connect (confirmed directly: `if (m_session && m_sftp) return
    // true;` at the top of SftpBackend::ensureSession()). The FIRST
    // operation TransferManager actually dispatches to this new backend
    // connects it, on its own worker thread — not the GUI thread, and
    // not a wasted eager connect for a pool member that ends up never
    // being needed.

    m_poolBackends.append(newBackend);
    m_poolThreads.append(newThread);
    return newBackend;
}

void FilePaneWidget::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    // Back/forward/up, matching any ordinary file manager. Blue —
    // IconTheme's own documented "primary / navigation / download / info"
    // accent — rather than the plain neutral gray these originally used;
    // real reported low contrast (both the enabled color and, worse, the
    // disabled Back/Forward state, which motivated tintedIcon()'s own
    // explicit-Disabled-pixmap fix) is why. Rendered a size up (24 vs.
    // this app's usual 20px default) for the same reason — a small extra
    // size bump reads as more solid/present without changing hue.
    m_backButton = new QToolButton(this);
    m_backButton->setIcon(IconTheme::tintedIcon(":/icons/arrow-left.svg", IconTheme::Blue, 24));
    m_backButton->setToolTip(tr("Back"));
    m_backButton->setEnabled(false);
    connect(m_backButton, &QToolButton::clicked, this, &FilePaneWidget::goBack);

    m_forwardButton = new QToolButton(this);
    m_forwardButton->setIcon(IconTheme::tintedIcon(":/icons/arrow-right.svg", IconTheme::Blue, 24));
    m_forwardButton->setToolTip(tr("Forward"));
    m_forwardButton->setEnabled(false);
    connect(m_forwardButton, &QToolButton::clicked, this, &FilePaneWidget::goForward);

    m_upButton = new QToolButton(this);
    m_upButton->setIcon(IconTheme::tintedIcon(":/icons/corner-left-up.svg", IconTheme::Blue, 24));
    m_upButton->setToolTip(tr("Up one level"));
    connect(m_upButton, &QToolButton::clicked, this, &FilePaneWidget::goUp);

    m_homeButton = new QToolButton(this);
    m_homeButton->setIcon(IconTheme::tintedIcon(":/icons/home.svg", IconTheme::Blue, 24));
    m_homeButton->setToolTip(tr("Home"));
    connect(m_homeButton, &QToolButton::clicked, this, &FilePaneWidget::goHome);

    m_refreshButton = new QToolButton(this);
    m_refreshButton->setIcon(IconTheme::tintedIcon(":/icons/refresh.svg", IconTheme::Blue, 24));
    m_refreshButton->setToolTip(tr("Refresh"));
    // Same deliberately-fresh re-listing the right-click context menu's
    // own Refresh action already uses — see that call site's comment.
    // navigateTo() self-guards against a navigation already in flight, so
    // no separate check is needed here.
    connect(m_refreshButton, &QToolButton::clicked, this, [this]() {
        navigateTo(currentDirectory());
    });

    m_pathBar = new QLineEdit(this);
    connect(m_pathBar, &QLineEdit::returnPressed, this, &FilePaneWidget::onPathBarReturnPressed);

    auto *pathRow = new QHBoxLayout;
    pathRow->addWidget(m_backButton);
    pathRow->addWidget(m_forwardButton);
    pathRow->addWidget(m_upButton);
    pathRow->addWidget(m_homeButton);
    pathRow->addWidget(m_refreshButton);
    pathRow->addWidget(m_pathBar, 1);
    layout->addLayout(pathRow);

    // Case-insensitive substring filter by name — composes with
    // showHiddenFiles in rebuildModel()'s own filter loop (see that
    // method's own comment), not a separate proxy-model pass; there is
    // no QSortFilterProxyModel anywhere in this codebase, and sorting is
    // already done directly on m_model via QStandardItem::operator<
    // overrides, so filtering follows the same "filter the raw cached
    // entries, then rebuild the model" shape showHiddenFiles already
    // established rather than introducing a second mechanism.
    // setClearButtonEnabled(true) is Qt's own built-in "x" to clear —
    // no custom button needed. Initial visibility from AppSettings
    // (shared across both panes via setFilterFieldVisible(), but each
    // pane's own filter TEXT stays independent) — defaults to visible
    // when m_settings is null (every test constructing a pane directly),
    // same "true" default filenameFilterVisible() itself uses.
    m_filterEdit = new QLineEdit(this);
    // Same convention ConnectionDialog's own fields use — lets tests
    // find this specific QLineEdit unambiguously now that the pane has
    // two (this one and m_pathBar).
    m_filterEdit->setObjectName(QStringLiteral("filterEdit"));
    m_filterEdit->setPlaceholderText(tr("Filter by name..."));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setVisible(!m_settings || m_settings->filenameFilterVisible());
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString &) {
        rebuildModel();
    });
    layout->addWidget(m_filterEdit);

    m_model->setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Modified"), tr("Permissions")});

    m_view = new FileTreeView(this, this);
    m_view->setModel(m_model);
    m_view->setRootIsDecorated(false);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Explicit, not relying on Qt's default (DoubleClicked |
    // EditKeyPressed): items are QStandardItem-editable by default, and
    // EditKeyPressed's platform key is F2 on most platforms — exactly
    // the key this pane now binds to its own promptAndRename() dialog
    // (see FileTreeView::keyPressEvent()). Without this, F2 (and
    // possibly double-click, which onRowDoubleClicked() below already
    // handles itself) could additionally pop Qt's own built-in inline
    // cell editor, which this app has never used or handled setData()
    // for. Renaming has always gone through the dedicated QInputDialog
    // flow instead; this just makes that the only possible outcome.
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // Interactive on every column, including Permissions — Stretch was
    // tried on Name early on and rejected: it makes Name's width purely
    // a side effect of dragging the OTHER columns' handles, with no
    // handle of its own to drag, which is worse for a column whose
    // whole point is reading long filenames.
    //
    // QTreeView's own stretchLastSection default used to be what let
    // Permissions (the actual last column) absorb all leftover width —
    // on anything but a narrow window that made it, not Name, the
    // widest column on open, the opposite of what a file browser should
    // lead with. Disabled here so every column instead gets its own
    // explicit initial width below, sized so Name is always the
    // largest on open: comfortably wider than any one of Size/Modified/
    // Permissions individually, while still leaving the app's own
    // fallback default window size (1280x900, MainWindow::MainWindow())
    // free of a horizontal scrollbar — and, same reasoning as
    // TransferQueueTable's own File column, the one column whose
    // content (arbitrarily long filenames) actually benefits from a
    // user dragging it wider still.
    //
    // These four widths (Name/Size/Modified/Permissions below) were
    // re-derived together from real QFontMetrics/QHeaderView::
    // sectionSizeHint() measurements against this app's actual UI font,
    // not eyeballed: Modified and Permissions sized to the tightest width
    // that doesn't clip their own binding constraint (content for
    // Modified, the header label itself for Permissions — Qt's own
    // sectionSizeHint() reflects header-label width only, which is why
    // it alone would rank Permissions above Modified even though
    // Modified's actual timestamp content is the wider of the two), Size
    // given deliberate breathing room rather than its own tight minimum,
    // and Name expanded to take the remaining width at the new default
    // window size.
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setSectionResizeMode(ColName, QHeaderView::Interactive);
    m_view->setColumnWidth(ColName, 280);
    // Header label ("Size") needs only ~22px and a realistic 8-digit byte
    // count ("12345678") ~55px raw (both measured via QFontMetrics) — this
    // is deliberately looser than either tight minimum, not sized to them.
    m_view->header()->setSectionResizeMode(ColSize, QHeaderView::Interactive);
    m_view->setColumnWidth(ColSize, 90);
    // The full "yyyy-MM-dd hh:mm:ss" timestamp is the binding constraint
    // here, not the "Modified" header label — measured at ~113px raw via
    // QFontMetrics, so 140px leaves a real (~27px) margin without being
    // needlessly loose; the maxWidths cap further down still allows
    // dragging it wider interactively.
    m_view->header()->setSectionResizeMode(ColModified, QHeaderView::Interactive);
    m_view->setColumnWidth(ColModified, 140);
    // Here the "Permissions" header LABEL is the binding constraint, not
    // the "rwxr-xr-x" value itself (~68px vs ~51px raw, measured via
    // QFontMetrics) — 95px is already close to the genuine tight minimum
    // before the label itself would clip, not much further room to shrink
    // it without doing that. Now that stretchLastSection is off, this is
    // a genuine bounded column with its own right-hand divider
    // (QHeaderView::section's border-right in both theme files) instead
    // of an unbounded stretch with no visible end — the trailing space
    // past it just shows the pane's own background, matching every other
    // empty area in the app.
    m_view->header()->setSectionResizeMode(ColPermissions, QHeaderView::Interactive);
    m_view->setColumnWidth(ColPermissions, 95);
    // Same real bug, same fix as TransferQueueWidget's identical File
    // column (see that file's own comment for the full story, including
    // the follow-up that found this exact class of fix needs to be
    // PER-column, not one header-wide cap): with no upper bound,
    // dragging one column wide enough pushes the others past the
    // visible area, unreachable (with stretchLastSection now off, nothing
    // absorbs the overflow — a horizontal scrollbar would appear instead
    // of silently clipping). Every column gets its own appropriately-
    // sized cap, Name's alone generous enough to actually read a long
    // filename.
    connect(m_view->header(), &QHeaderView::sectionResized, this,
            [this](int logicalIndex, int /*oldSize*/, int newSize) {
        static const QHash<int, int> maxWidths{
            {ColName, 500}, {ColSize, 120}, {ColModified, 180}, {ColPermissions, 200}
        };
        const auto it = maxWidths.constFind(logicalIndex);
        if (it != maxWidths.constEnd() && newSize > it.value())
            m_view->header()->resizeSection(logicalIndex, it.value());
    });
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
    connect(m_view, &FileTreeView::externalFilesDropped, this, &FilePaneWidget::externalFilesDropped);
    // Keyboard shortcuts — see FileTreeView::keyPressEvent()'s own
    // comment for why the actual logic lives in these handlers rather
    // than in FileTreeView itself.
    connect(m_view, &FileTreeView::deleteKeyPressed, this, &FilePaneWidget::onDeleteKeyPressed);
    connect(m_view, &FileTreeView::renameKeyPressed, this, &FilePaneWidget::onRenameKeyPressed);
    connect(m_view, &FileTreeView::refreshKeyPressed, this, &FilePaneWidget::onRefreshKeyPressed);
    connect(m_view, &FileTreeView::copyKeyPressed, this, &FilePaneWidget::onCopyKeyPressed);
    connect(m_view, &FileTreeView::pasteKeyPressed, this, &FilePaneWidget::onPasteKeyPressed);
    layout->addWidget(m_view);

    m_statusLabel = new QLabel(this);
    layout->addWidget(m_statusLabel);
}

void FilePaneWidget::setFilterFieldVisible(bool visible)
{
    m_filterEdit->setVisible(visible);
    if (!visible)
        m_filterEdit->clear();   // fires textChanged -> rebuildModel() on its own
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
        return IconTheme::tintedIcon(":/icons/folder.svg", IconTheme::Gray());
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

    return IconTheme::tintedIcon(":/icons/file.svg", IconTheme::Gray());
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

void FilePaneWidget::retintIcons()
{
    m_backButton->setIcon(IconTheme::tintedIcon(":/icons/arrow-left.svg", IconTheme::Blue, 24));
    m_forwardButton->setIcon(IconTheme::tintedIcon(":/icons/arrow-right.svg", IconTheme::Blue, 24));
    m_upButton->setIcon(IconTheme::tintedIcon(":/icons/corner-left-up.svg", IconTheme::Blue, 24));
    m_homeButton->setIcon(IconTheme::tintedIcon(":/icons/home.svg", IconTheme::Blue, 24));
    m_refreshButton->setIcon(IconTheme::tintedIcon(":/icons/refresh.svg", IconTheme::Blue, 24));
    rebuildModel();
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
    // Case-insensitive substring match — deliberately not wildcard/regex,
    // same "simplest thing that solves the real problem" restraint this
    // project applies elsewhere (chmod's 9-bits-only, quick-connect's
    // no-IPv6 boundary). Composed with showHidden in the SAME predicate,
    // not a second pass — a dotfile matching the filter text still stays
    // excluded unless showHiddenFiles is also on.
    const QString filterText = m_filterEdit ? m_filterEdit->text().trimmed() : QString();

    QList<RemoteEntry> filtered;
    for (const RemoteEntry &e : m_lastRawEntries) {
        if ((showHidden || !e.name.startsWith(QLatin1Char('.')))
            && (filterText.isEmpty() || e.name.contains(filterText, Qt::CaseInsensitive)))
            filtered.append(e);
    }

    // Default listing order: folders first, then name ascending (A-Z) —
    // applied uniformly here rather than left to each backend, since only
    // LocalBackend ever sorted its own results (QDir::DirsFirst |
    // QDir::Name, itself ascending); SftpBackend/FtpBackend returned
    // whatever order the server happened to list in. Sorted by the
    // entry's real name, not nameItem's later "[folder]" display text,
    // for the same reason entryForRow() looks entries up by real name
    // below. A later header click overrides this via the ordinary
    // column-sort machinery further down — this only decides what a
    // FRESH listing looks like before that's ever happened.
    //
    // **A real, live-reported inconsistency, not just a preference**:
    // this comparator used to sort descending (`> 0`) — the opposite of
    // both NameItem's own ascending-by-default header-click comparator
    // just above and LocalBackend's pre-unification behavior this
    // comment already claimed to match. A fresh listing showed Z-A with
    // no sort indicator on the Name header at all, then clicking that
    // same header once flipped to A-Z — a jarring, inconsistent-feeling
    // reversal on the very first click, exactly matching the report.
    // Fixed to ascending, matching NameItem's comparator and this
    // comment's own original (if previously unrealized) intent.
    std::stable_sort(filtered.begin(), filtered.end(), [](const RemoteEntry &a, const RemoteEntry &b) {
        if (a.isDir != b.isDir)
            return a.isDir;
        return a.name.localeAwareCompare(b.name) < 0;
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
        auto *modItem = new QStandardItem(e.modified.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")));
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
    emit directoryChanged(path);
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

    // Chmod — single entry only (matching Rename's own precedent; a
    // multi-select "apply this mode to everything selected" is a cheap,
    // natural follow-up later, not attempted here), but offered for
    // BOTH files and directories and on every backend including Local
    // (unlike Edit below, which is deliberately remote-only) — Unix
    // permissions are meaningful for a local file too. lock.svg is
    // already used elsewhere in this file (iconForEntry(), for a
    // dotfile-looking "restricted" directory's row icon) — same padlock
    // semantic, a different UI surface, a deliberate reuse rather than
    // a new icon.
    QAction *permissionsAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/lock.svg", IconTheme::Amber), tr("Permissions..."));
    permissionsAction->setEnabled(selected.size() == 1);

    // Edit-in-place — downloads to a local temp file, opens it in an
    // external editor, and re-uploads on every save (see
    // EditSessionManager). Gray/app-window.svg rather than reusing
    // Rename's edit.svg+Blue: this app has no separate "open externally"
    // icon vendored, but the two actions sitting right next to each
    // other in this same menu need to read as visually distinct, not
    // just textually distinct. Remote-only (a local file already opens
    // directly via the OS, no round-trip needed) and single-file only
    // (multiple simultaneous edit sessions from one click isn't a
    // real use case worth the added complexity).
    QAction *editAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/app-window.svg", IconTheme::Gray()), tr("Edit"));
    editAction->setEnabled(selected.size() == 1 && !selected.first().isDir
                            && !m_backend->isLocalFilesystem());

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
    else if (chosen == permissionsAction)
        promptAndChmod(selected.first(), directory);
    else if (chosen == editAction)
        emit editRequested(this, selected.first());
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

    // Loaded directly, the same way TransferQueueTable.cpp already calls
    // SiteStore::load() for its own site-picking submenu — a small, pure
    // data read, not worth routing through MainWindow first. Built
    // BEFORE Connect...'s own action so "pick something to reconnect to
    // quickly" reads as the more prominent option, matching Sites...'s
    // own precedence.
    const QList<ConnectionHistoryEntry> recent = ConnectionHistoryStore::load();
    QMenu *recentMenu = menu.addMenu(
        IconTheme::tintedIcon(":/icons/device-laptop.svg", IconTheme::Blue), tr("Recent Connections"));
    QHash<QAction *, ConnectionHistoryEntry> recentActions;
    if (recent.isEmpty()) {
        QAction *placeholder = recentMenu->addAction(tr("No recent connections"));
        placeholder->setEnabled(false);
    } else {
        for (const ConnectionHistoryEntry &entry : recent) {
            const QString protocolSuffix = entry.protocol == Protocol::Ftp
                ? tr(" (FTP)")
                : entry.protocol == Protocol::Ftps ? tr(" (FTPS)") : QString();
            QAction *action = recentMenu->addAction(
                tr("%1@%2:%3%4").arg(entry.username, entry.host).arg(entry.port).arg(protocolSuffix));
            recentActions.insert(action, entry);
        }
        recentMenu->addSeparator();
        recentMenu->addAction(tr("Clear Recent Connections"), this, [] {
            ConnectionHistoryStore::clear();
        });
    }

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
    else if (recentActions.contains(chosen))
        emit recentConnectionRequested(this, recentActions.value(chosen));
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

void FilePaneWidget::promptAndChmod(const RemoteEntry &entry, const QString &directory)
{
    PermissionsDialog dialog(entry, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString path = joinPath(directory, entry.name);
    ++m_pendingFileOpRefreshes;   // see its own doc comment
    QMetaObject::invokeMethod(m_backend, "setPermissions", Qt::QueuedConnection,
                               Q_ARG(QString, path), Q_ARG(int, dialog.mode()));
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

    QList<QPair<QString, bool>> pathsAndIsDir;
    pathsAndIsDir.reserve(entries.size());
    for (const RemoteEntry &entry : entries)
        pathsAndIsDir.append({joinPath(directory, entry.name), entry.isDir});
    deleteEntriesAt(pathsAndIsDir, /*offerRecursiveDeleteOnFailure=*/true);
}

void FilePaneWidget::deleteEntriesAt(const QList<QPair<QString, bool>> &pathsAndIsDir,
                                      bool offerRecursiveDeleteOnFailure)
{
    // One invokeMethod() call per entry, queued in order — the backend
    // lives on (at most) one thread of its own, so Qt's queue processes
    // these strictly one at a time; each delete's own internal refresh
    // (see RemoteBackend::deleteEntry()'s doc comment) completes before
    // the next deletion starts. No race between them.
    for (const auto &pathAndIsDir : pathsAndIsDir) {
        ++m_pendingFileOpRefreshes;   // see its own doc comment — one per entry, each gets its own refresh
        if (offerRecursiveDeleteOnFailure && pathAndIsDir.second)
            m_recursiveDeleteCandidates.insert(pathAndIsDir.first);
        QMetaObject::invokeMethod(m_backend, "deleteEntry", Qt::QueuedConnection,
                                   Q_ARG(QString, pathAndIsDir.first), Q_ARG(bool, pathAndIsDir.second));
    }
}

void FilePaneWidget::onFileOperationFailed(const QString &operation, const QString &path, const QString &reason)
{
    // A failed dispatch never produces the directoryListed refresh
    // onDirectoryListed() would otherwise consume — decrement here so
    // m_pendingFileOpRefreshes doesn't stay permanently inflated and start
    // misclassifying a later, unrelated navigation response as a refresh.
    // Unconditional, regardless of whether this specific failure's own
    // dialog shows immediately or gets deferred below — the dispatch
    // this call answers for is resolved the moment this callback runs,
    // not whenever its dialog eventually appears.
    if (m_pendingFileOpRefreshes > 0)
        --m_pendingFileOpRefreshes;

    // A "Delete" failure for a path this pane itself opted into the
    // recursive-delete offer for (see deleteEntriesAt()'s own doc
    // comment) gets a chance to become something recoverable instead of
    // a plain dead end — everything else (files, CompareSyncExecutor's
    // own directory deletes, which never populate the set) behaves
    // exactly as before.
    if (operation == QStringLiteral("Delete") && m_recursiveDeleteCandidates.remove(path)) {
        runOrDeferModalAction([this, path, reason]() { offerRecursiveDelete(path, reason); });
        return;
    }

    runOrDeferModalAction([this, operation, path, reason]() {
        showFailureWarning(operation, path, reason);
    });
}

void FilePaneWidget::onDeleteKeyPressed()
{
    // Mirrors showContextMenu()'s deleteAction exactly — same selection
    // snapshot, same currentDirectory() capture, same confirmAndDelete()
    // call. Selection can legitimately be empty (Delete pressed with
    // nothing selected) — silently do nothing, same as deleteAction
    // being disabled in that case, rather than showing a dialog about
    // nothing.
    const QList<RemoteEntry> selected = selectedEntries();
    if (selected.isEmpty())
        return;
    confirmAndDelete(selected, currentDirectory());
}

void FilePaneWidget::onRenameKeyPressed()
{
    // Mirrors showContextMenu()'s renameAction: single-selection only —
    // renaming several items to one name doesn't make sense, same
    // reasoning as that action's own setEnabled(selected.size() == 1).
    const QList<RemoteEntry> selected = selectedEntries();
    if (selected.size() != 1)
        return;
    promptAndRename(selected.first(), currentDirectory());
}

void FilePaneWidget::onRefreshKeyPressed()
{
    // Same deliberately-fresh re-listing the right-click Refresh action
    // and the toolbar's refresh button both already use.
    navigateTo(currentDirectory());
}

void FilePaneWidget::onCopyKeyPressed()
{
    const QList<RemoteEntry> selected = selectedEntries();
    if (selected.isEmpty())
        return;
    emit filesCopied(selected);
}

void FilePaneWidget::onPasteKeyPressed()
{
    emit pasteRequested();
}

void FilePaneWidget::runOrDeferModalAction(std::function<void()> action)
{
    // See m_modalDialogInProgress's own doc comment (FilePaneWidget.h)
    // for the real crash this guards against.
    if (m_modalDialogInProgress) {
        m_deferredDialogActions.append(std::move(action));
        return;
    }

    action();

    // Drain anything that arrived while that action's own dialog was
    // open — a LOOP, not recursion, so a burst of many failures (the
    // exact scenario this whole mechanism exists for) can't itself
    // become unbounded C++ call-stack depth. Re-checks isEmpty() each
    // iteration rather than snapshotting once: a fresh action can still
    // arrive live (a modal dialog's own exec() pumps the event queue)
    // while an earlier DEFERRED one's own dialog is showing here.
    while (!m_deferredDialogActions.isEmpty()) {
        const auto next = m_deferredDialogActions.takeFirst();
        next();
    }
}

void FilePaneWidget::showFailureWarning(const QString &operation, const QString &path, const QString &reason)
{
    m_modalDialogInProgress = true;
    QMessageBox::warning(this, operation,
                          tr("%1 failed for \"%2\":\n%3")
                              .arg(operation, QFileInfo(path).fileName(), reason));
    m_modalDialogInProgress = false;
}

void FilePaneWidget::offerRecursiveDelete(const QString &path, const QString &reason)
{
    const QString folderName = QFileInfo(path).fileName();
    auto *enumerator = new FolderEnumerator(m_backend, path, folderName, this);

    connect(enumerator, &FolderEnumerator::finished, this,
            [this, enumerator, path, folderName](const QList<EnumeratedItem> &items) {
        enumerator->deleteLater();

        // items includes the root folder itself (relativePath ==
        // folderName, confirmed by TransferManager.cpp's own comment on
        // FolderEnumerator's output) — exclude it and count the rest.
        int fileCount = 0;
        int dirCount = 0;
        for (const EnumeratedItem &item : items) {
            if (item.relativePath == folderName)
                continue;
            if (item.isDir) ++dirCount; else ++fileCount;
        }

        if (fileCount == 0 && dirCount == 0) {
            // Genuinely empty by the time this walk finished — a race
            // (something else emptied it between the failed plain
            // delete and this enumeration completing). Nothing to
            // offer; fall back to the original failure.
            runOrDeferModalAction([this, path]() {
                showFailureWarning(tr("Delete"), path,
                                    tr("Could not remove folder — it may not be empty"));
            });
            return;
        }

        runOrDeferModalAction([this, path, folderName, items, fileCount, dirCount]() {
            // No i18n loading infrastructure exists in this project yet
            // (see ARCHITECTURE.md's own Known-limitations entry) —
            // plain count == 1 branching, matching confirmAndDelete()'s
            // own identical singular/plural handling, rather than
            // reaching for Qt's %n mechanism for translations that
            // can't be loaded yet anyway.
            const QString filesPart = fileCount == 1 ? tr("1 file") : tr("%1 files").arg(fileCount);
            const QString foldersPart = dirCount == 1 ? tr("1 folder") : tr("%1 folders").arg(dirCount);
            QString contentsDescription;
            if (fileCount > 0 && dirCount > 0)
                contentsDescription = tr("%1 and %2").arg(filesPart, foldersPart);
            else if (dirCount > 0)
                contentsDescription = foldersPart;
            else
                contentsDescription = filesPart;

            m_modalDialogInProgress = true;
            const auto reply = QMessageBox::warning(
                this, tr("Delete"),
                tr("\"%1\" isn't empty — it contains %2. Deleting it will permanently "
                   "delete everything inside, including all subfolders. This can't be undone.")
                    .arg(folderName, contentsDescription),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            m_modalDialogInProgress = false;

            if (reply != QMessageBox::Yes)
                return;

            runRecursiveDelete(path, items);
        });
    });

    connect(enumerator, &FolderEnumerator::failed, this,
            [this, enumerator, path](const QString &enumReason) {
        enumerator->deleteLater();
        runOrDeferModalAction([this, path, enumReason]() {
            showFailureWarning(tr("Delete"), path,
                                tr("Couldn't check what's inside this folder: %1").arg(enumReason));
        });
    });

    enumerator->start();
}

void FilePaneWidget::runRecursiveDelete(const QString &path, const QList<EnumeratedItem> &items)
{
    const QString root = QFileInfo(path).path();

    // Same two-phase, deepest-first pattern CompareSyncExecutor::
    // deleteSelected() already established for its own bottom-up
    // delete: every file goes first (any depth, order doesn't matter),
    // then every directory sorted deepest-first by relative-path '/'
    // count — the root folder's own relativePath has zero slashes, so
    // it sorts naturally last, no special-casing needed. By the time
    // each directory's turn comes up, everything that was ever inside
    // it (files and deeper subdirectories alike) is already gone.
    QList<QPair<QString, bool>> filePairs;
    QList<EnumeratedItem> dirItems;
    for (const EnumeratedItem &item : items) {
        if (item.isDir)
            dirItems.append(item);
        else
            filePairs.append({joinPath(root, item.relativePath), false});
    }

    std::sort(dirItems.begin(), dirItems.end(), [](const EnumeratedItem &a, const EnumeratedItem &b) {
        return a.relativePath.count(QLatin1Char('/')) > b.relativePath.count(QLatin1Char('/'));
    });

    QList<QPair<QString, bool>> dirPairs;
    dirPairs.reserve(dirItems.size());
    for (const EnumeratedItem &item : dirItems)
        dirPairs.append({joinPath(root, item.relativePath), true});

    // Two separate calls, not merged, so Qt's per-backend queued-
    // connection FIFO ordering keeps every file delete dispatched
    // before the first directory delete is even queued — same
    // reasoning CompareSyncExecutor::deleteSelected()'s own comment
    // documents. offerRecursiveDeleteOnFailure=true again deliberately:
    // a nested directory that unexpectedly fails its own turn (a
    // genuine race, not the common case) gets the identical recursive
    // offer instead of just failing inconsistently with its siblings.
    if (!filePairs.isEmpty())
        deleteEntriesAt(filePairs, /*offerRecursiveDeleteOnFailure=*/true);
    if (!dirPairs.isEmpty())
        deleteEntriesAt(dirPairs, /*offerRecursiveDeleteOnFailure=*/true);
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
