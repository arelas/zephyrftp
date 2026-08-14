#include "TransferQueueTable.h"
#include "../transfer/TransferManager.h"
#include "../backends/SavedSite.h"
#include "IconTheme.h"

#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QMenu>
#include <QAction>
#include <QProgressBar>
#include <QLabel>
#include <QTimer>
#include <algorithm>

namespace {
constexpr int ColName = 0;
constexpr int ColDirection = 1;
constexpr int ColStatus = 2;
constexpr int ColProgress = 3;
constexpr int ColSpeed = 4;
constexpr int IdRole = Qt::UserRole;
}

QueueCategory categoryFor(TransferStatus status)
{
    switch (status) {
    case TransferStatus::Done:
        return QueueCategory::Completed;
    case TransferStatus::Failed:
    case TransferStatus::Cancelled:
    case TransferStatus::Skipped:
        return QueueCategory::Failed;
    case TransferStatus::Queued:
    case TransferStatus::InProgress:
    case TransferStatus::Paused:
    case TransferStatus::PendingReconnect:
        return QueueCategory::Active;
    }
    return QueueCategory::Active;
}

TransferQueueTable::TransferQueueTable(TransferManager *manager, QueueCategory category, QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
    , m_category(category)
    , m_table(new QTableWidget(this))
{
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels(
        {tr("File"), tr("Direction"), tr("Status"), tr("Progress"), tr("Speed")});
    // File is the one column whose content (a filename/path) genuinely
    // varies in length, so it's the one that should flex. Stretch mode
    // makes Qt auto-manage its width to whatever's left once the other
    // four (below) claim their own small, fixed space — growing the
    // window gives File the extra room; shrinking the window takes room
    // away from File specifically, never from the other four. A Stretch
    // section has no user-draggable handle at all, which is deliberate:
    // this REPLACES two earlier, increasingly complicated attempts at
    // stopping columns from pushing each other off screen (a single
    // header-wide max, then a per-column max — see CHANGELOG.md's
    // history) with removing the dragging that caused the problem in
    // the first place, once it was clear Direction/Status/Progress/Speed
    // gained nothing from being resizable anyway (see below).
    m_table->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    // Direction/Status/Progress/Speed: Fixed, not Interactive. Their
    // content is short and fixed-shape (a 20px icon, a status word, a
    // progress bar, a speed reading) — there's nothing a user gains by
    // resizing them, only cosmetic fiddling, and leaving them fixed is
    // exactly what makes File's own Stretch behavior above sufficient
    // on its own, with no "compress the neighbor on drag" logic needed
    // anywhere. Widths sized to comfortably fit each column's own
    // header label plus its typical content, not the absolute longest
    // possible string — Status in particular can occasionally run
    // longer than this (e.g. "Failed: <server's own error text>", or
    // the phase-aware "Paused - Downloading (1/2)"), and is expected to
    // ellipsize in that case; its cell's tooltip carries the full text
    // either way (see appendRow()/updateItem() below).
    m_table->horizontalHeader()->setSectionResizeMode(ColDirection, QHeaderView::Fixed);
    m_table->setColumnWidth(ColDirection, 90);
    m_table->horizontalHeader()->setSectionResizeMode(ColStatus, QHeaderView::Fixed);
    m_table->setColumnWidth(ColStatus, 110);
    m_table->horizontalHeader()->setSectionResizeMode(ColProgress, QHeaderView::Fixed);
    m_table->setColumnWidth(ColProgress, 110);
    m_table->horizontalHeader()->setSectionResizeMode(ColSpeed, QHeaderView::Fixed);
    m_table->setColumnWidth(ColSpeed, 80);
    // The header row centers section labels by default (Fusion style);
    // "File" reads oddly centered over a column of left-aligned filenames
    // — Direction/Status/Progress/Speed stay centered, matching the
    // centered icon/text/widget each of those columns actually shows.
    m_table->horizontalHeaderItem(ColName)->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableWidget::customContextMenuRequested,
            this, &TransferQueueTable::showContextMenu);
    // A real bug found by code review: this used to be set true here,
    // unconditionally, with setSortIndicator() only ever called later
    // from onHeaderSectionClicked() — QHeaderView's own default indicator
    // state (section 0, ascending) then showed on the File column from
    // startup, misleadingly implying the queue was already sorted
    // alphabetically when m_sortColumn is still -1 (plain insertion
    // order) until a header is actually clicked. Left false here; turned
    // on in onHeaderSectionClicked() instead, at the exact moment a real
    // sort actually begins.
    connect(m_table->horizontalHeader(), &QHeaderView::sectionClicked,
            this, &TransferQueueTable::onHeaderSectionClicked);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_table);
}

int TransferQueueTable::rowForId(int id) const
{
    const auto it = m_rowById.constFind(id);
    if (it != m_rowById.constEnd())
        return it.value();
    // Falls back to a linear scan only if the map somehow missed an
    // entry — should be unreachable in practice (every row is added via
    // appendRow(), which always populates m_rowById at the same time),
    // kept as a defensive net rather than trusting that invariant blindly.
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *item = m_table->item(row, ColName);
        if (item && item->data(IdRole).toInt() == id)
            return row;
    }
    return -1;
}

QString TransferQueueTable::directionText(TransferDirection direction)
{
    switch (direction) {
    case TransferDirection::LocalToRemote:  return tr("local -> remote");
    case TransferDirection::RemoteToLocal:  return tr("remote -> local");
    case TransferDirection::LocalToLocal:   return tr("local copy");
    case TransferDirection::RemoteToRemote: return tr("server -> server");
    case TransferDirection::Move:           return tr("move");
    case TransferDirection::EditDownload:   return tr("opening for edit");
    case TransferDirection::EditUpload:     return tr("saving edit");
    case TransferDirection::Unsupported:    return tr("unsupported");
    }
    return {};
}

QString TransferQueueTable::statusText(const TransferItem &item)
{
    switch (item.status) {
    case TransferStatus::Queued:      return tr("Queued");
    case TransferStatus::InProgress:
        // A RemoteToRemote item's progress bar/percentage resets to 0 at
        // the phase-1->phase-2 transition (see TransferManager::
        // onBackendFinished()) — the "(1/2)"/"(2/2)" suffix is what stops
        // that reset from reading as a bug rather than the two distinct
        // stages it actually is.
        if (item.direction == TransferDirection::RemoteToRemote) {
            return item.phase == TransferPhase::Uploading
                ? tr("Uploading (2/2)") : tr("Downloading (1/2)");
        }
        return tr("Transferring");
    case TransferStatus::Paused:
        // Same phase-aware suffix as InProgress above, and for the same
        // reason — a RemoteToRemote item paused mid-upload still shows
        // bytesDone/bytesTotal from that (reset-to-0) phase, so "Paused"
        // alone would read as "paused right at the very start" even when
        // it's actually well into the upload half.
        if (item.direction == TransferDirection::RemoteToRemote) {
            return item.phase == TransferPhase::Uploading
                ? tr("Paused - Uploading (2/2)") : tr("Paused - Downloading (1/2)");
        }
        return tr("Paused");
    case TransferStatus::Done:        return tr("Done");
    case TransferStatus::Failed:      return item.errorMessage.isEmpty()
                                              ? tr("Failed")
                                              : tr("Failed: %1").arg(item.errorMessage);
    case TransferStatus::Cancelled:   return tr("Cancelled");
    case TransferStatus::Skipped:     return tr("Skipped");
    case TransferStatus::PendingReconnect: {
        // Prefer the saved site's own display name over a bare host —
        // only a live lookup when there IS a savedSiteId to resolve
        // (restored from a plain, non-site connection otherwise), and
        // only for this rare, low-frequency status, not a hot path.
        QString label = item.pendingConnection.host;
        if (!item.pendingConnection.savedSiteId.isEmpty()) {
            const QList<SavedSite> sites = SiteStore::load();
            for (const SavedSite &site : sites) {
                if (site.id == item.pendingConnection.savedSiteId) {
                    label = site.name;
                    break;
                }
            }
        }
        return tr("Waiting to reconnect: %1").arg(label);
    }
    }
    return {};
}

QIcon TransferQueueTable::statusIcon(const TransferItem &item)
{
    // Terminal states get their own icon regardless of direction, per
    // ICON-MAP.md's "Transfer direction & status" table (Done -> green
    // check). Cancelled isn't in the original design (this app has a
    // status the mockup didn't anticipate) — muted x, consistent with how
    // the mockup treats "done" as muted-but-still-marked rather than
    // inventing a new accent color for it.
    if (item.status == TransferStatus::Done)
        return IconTheme::tintedIcon(":/icons/check.svg", IconTheme::Green);
    if (item.status == TransferStatus::Failed)
        return IconTheme::tintedIcon(":/icons/alert-triangle.svg", IconTheme::Red);
    if (item.status == TransferStatus::Cancelled)
        return IconTheme::tintedIcon(":/icons/x.svg", IconTheme::GrayMuted());
    if (item.status == TransferStatus::Skipped)
        // Same icon/color as Cancelled — both mean "didn't happen, not an
        // error" — distinguished by the status text next to it, not a
        // separate accent color the design's four-color system doesn't
        // really have room for a fifth meaning in anyway.
        return IconTheme::tintedIcon(":/icons/x.svg", IconTheme::GrayMuted());
    if (item.status == TransferStatus::Paused)
        return IconTheme::tintedIcon(":/icons/player-pause.svg", IconTheme::Amber);
    if (item.status == TransferStatus::PendingReconnect)
        // Gray, not Amber — visually distinct from Paused: Paused means
        // a live connection exists and this simply isn't running right
        // now, PendingReconnect means no connection exists at all yet.
        // plug.svg (not the direction-shaped icons below) since there's
        // no data flowing in either direction to depict.
        return IconTheme::tintedIcon(":/icons/plug.svg", IconTheme::Gray());

    // Queued or InProgress: a direction-shaped icon (the mockup doesn't
    // cover local-to-local, remote-to-remote, move, or unsupported
    // directions, since it assumes only upload/download exist —
    // arrows-left-right and alert-triangle are this app's own extensions
    // of the same visual language; RemoteToRemote reuses arrows-left-right
    // too, same "both directions" reasoning LocalToLocal already uses it
    // for. Move gets its own plain arrow-right — unlike every other
    // direction here, it's a single one-way relocation with no data
    // transfer, not a "both directions" copy, so arrows-left-right would
    // send the wrong signal about what's actually happening.).
    QString path;
    switch (item.direction) {
    case TransferDirection::LocalToRemote:  path = ":/icons/arrow-up.svg"; break;
    case TransferDirection::RemoteToLocal:  path = ":/icons/arrow-down.svg"; break;
    case TransferDirection::LocalToLocal:   path = ":/icons/arrows-left-right.svg"; break;
    case TransferDirection::RemoteToRemote: path = ":/icons/arrows-left-right.svg"; break;
    case TransferDirection::Move:           path = ":/icons/arrow-right.svg"; break;
    // Reuses the plain download/upload arrows rather than inventing a
    // dedicated "edit" icon — an EditDownload genuinely is a download
    // (remote -> local temp file) and an EditUpload genuinely is an
    // upload (local temp file -> remote), just triggered by opening a
    // file for editing rather than a pane-to-pane transfer.
    case TransferDirection::EditDownload:   path = ":/icons/arrow-down.svg"; break;
    case TransferDirection::EditUpload:     path = ":/icons/arrow-up.svg"; break;
    case TransferDirection::Unsupported:    path = ":/icons/alert-triangle.svg"; break;
    }

    QColor color = IconTheme::Gray();   // Queued default
    if (item.status == TransferStatus::InProgress) {
        switch (item.direction) {
        case TransferDirection::LocalToRemote: color = IconTheme::Green; break;   // active upload
        case TransferDirection::RemoteToLocal: color = IconTheme::Blue; break;    // active download
        case TransferDirection::RemoteToRemote:
            // Matches statusText()'s "(1/2)"/"(2/2)" phase labels: Blue
            // while downloading (same color RemoteToLocal uses), Green
            // while uploading (same color LocalToRemote uses) — the icon
            // color reinforces which half of the staging is active
            // instead of introducing a color the two-phase text doesn't
            // already imply.
            color = item.phase == TransferPhase::Uploading ? IconTheme::Green : IconTheme::Blue;
            break;
        case TransferDirection::Move: color = IconTheme::Blue; break;   // brief; matches its icon's "in-flight" reading
        // Same colors their reused icons already carry elsewhere in this
        // switch — Blue for a download (RemoteToLocal), Green for an
        // upload (LocalToRemote) — not left to `default` below, per the
        // LocalToLocal fix's own lesson just above.
        case TransferDirection::EditDownload: color = IconTheme::Blue; break;
        case TransferDirection::EditUpload: color = IconTheme::Green; break;
        // A real bug found by code review: LocalToLocal had no case here
        // (unlike directionText()/the icon-path switch above, which both
        // already handle it explicitly), so it silently fell into
        // `default`, rendering the exact same Gray as a still-Queued
        // item — an active local copy was indistinguishable from one
        // that hadn't started. Green matches the same "active copy"
        // reading LocalToRemote's upload already uses.
        case TransferDirection::LocalToLocal: color = IconTheme::Green; break;
        default: color = IconTheme::Gray(); break;
        }
    }
    return IconTheme::tintedIcon(path, color);
}

QColor TransferQueueTable::statusTextColor(TransferStatus status)
{
    switch (status) {
    case TransferStatus::Queued:      return IconTheme::Gray();
    case TransferStatus::InProgress:  return IconTheme::Blue;
    case TransferStatus::Paused:      return IconTheme::Amber;
    case TransferStatus::Done:        return IconTheme::Green;
    case TransferStatus::Failed:      return IconTheme::Red;
    case TransferStatus::Cancelled:   return IconTheme::GrayMuted();
    case TransferStatus::Skipped:     return IconTheme::GrayMuted();
    }
    return IconTheme::Gray();
}

int TransferQueueTable::percentFor(const TransferItem &item)
{
    if (item.status == TransferStatus::Done)
        return 100;
    // Paused is frozen at whatever bytesDone reached before the pause —
    // same formula as InProgress, just not advancing anymore.
    if ((item.status == TransferStatus::InProgress || item.status == TransferStatus::Paused)
        && item.bytesTotal > 0)
        return static_cast<int>((item.bytesDone * 100) / item.bytesTotal);
    return 0;
}

QString TransferQueueTable::speedText(const TransferItem &item)
{
    if (item.status != TransferStatus::InProgress || item.speedBytesPerSec <= 0)
        return {};

    const double bps = static_cast<double>(item.speedBytesPerSec);
    if (bps >= 1024.0 * 1024.0)
        return QStringLiteral("%1 MB/s").arg(bps / (1024.0 * 1024.0), 0, 'f', 1);
    if (bps >= 1024.0)
        return QStringLiteral("%1 KB/s").arg(bps / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 B/s").arg(item.speedBytesPerSec);
}

void TransferQueueTable::addItem(const TransferItem &item)
{
    // If a sort is currently active, route through resortAndRebuild()
    // instead of a plain appendRow() — it reads live from
    // manager->items() (filtered to this instance's own category), so
    // this rebuilds the whole table with the new row in its correct
    // sorted place rather than tacked on at the end.
    //
    // Deferred via a coalesced QTimer::singleShot(0, ...), NOT called
    // synchronously — a real, severe bug found from a live crash report:
    // enqueueing N items in a tight loop (any folder transfer, or a
    // multi-select drag-drop) fires this once PER ITEM, synchronously,
    // all within the same loop; calling resortAndRebuild() (a full
    // table wipe + rebuild of every row's items AND cell widgets) on
    // every single one is O(N) work times N calls = O(N²) total.
    // m_rebuildPending guards against scheduling more than one deferred
    // rebuild per burst.
    if (m_sortColumn != -1) {
        if (!m_rebuildPending) {
            m_rebuildPending = true;
            QTimer::singleShot(0, this, [this]() {
                m_rebuildPending = false;
                resortAndRebuild();
            });
        }
        return;
    }
    appendRow(item);
    // appendRow() alone leaves the row at its bare-insert defaults (0%
    // progress, no speed) — essential, not just tidy, when this item is
    // arriving here via a CATEGORY MIGRATION (e.g. just reached Done,
    // moving from the Active table) rather than as a genuinely brand-new
    // item: without this, a migrated row would show 0% forever, since
    // Done is usually the LAST update TransferManager ever sends for
    // that item — nothing later would ever correct it.
    updateItem(item);
}

void TransferQueueTable::appendRow(const TransferItem &item)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_rowById.insert(item.id, row);
    fillRow(row, item);
}

void TransferQueueTable::fillRow(int row, const TransferItem &item)
{
    auto *nameItem = new QTableWidgetItem(item.fileName);
    nameItem->setData(IdRole, item.id);
    nameItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_table->setItem(row, ColName, nameItem);

    // A QLabel, not a QTableWidgetItem with just an icon set — an item's
    // Qt::TextAlignmentRole only affects its TEXT, not its decoration, so
    // an icon-only item is always drawn hugging the left edge no matter
    // what alignment is requested. A label filling the cell (same
    // technique as the progress bar below) actually centers it.
    auto *dirLabel = new QLabel(m_table);
    dirLabel->setAlignment(Qt::AlignCenter);
    dirLabel->setPixmap(statusIcon(item).pixmap(20, 20));
    dirLabel->setToolTip(directionText(item.direction));
    m_table->setCellWidget(row, ColDirection, dirLabel);

    auto *statusItem = new QTableWidgetItem(statusText(item));
    statusItem->setForeground(statusTextColor(item.status));
    statusItem->setTextAlignment(Qt::AlignCenter);
    // Status's column width is fixed and modest (see the constructor's
    // own comment) — this is what keeps the full text reachable for the
    // occasional long one (a Failed item's own server error message)
    // that ellipsizes in the cell itself.
    statusItem->setToolTip(statusText(item));
    m_table->setItem(row, ColStatus, statusItem);

    // Inline progress bars (design decision #6 in the package's README:
    // "trims the table to 3 meaningful columns... rather than a separate
    // text status column plus a separate progress column") — replaces
    // what used to be a plain percentage-text QTableWidgetItem.
    auto *progressBar = new QProgressBar(m_table);
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setTextVisible(false);
    progressBar->setFixedHeight(6);   // matches .zf-progress-track's 6px height

    // setCellWidget() stretches its widget to fill the entire cell rect,
    // but a setFixedHeight() widget can't actually grow into that height —
    // it just sits pinned at the rect's top edge instead. Wrapping it in a
    // container with stretch above and below centers it vertically in the
    // (much taller) row instead.
    auto *progressContainer = new QWidget(m_table);
    auto *progressLayout = new QVBoxLayout(progressContainer);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->addStretch();
    progressLayout->addWidget(progressBar);
    progressLayout->addStretch();
    m_table->setCellWidget(row, ColProgress, progressContainer);

    auto *speedItem = new QTableWidgetItem();
    speedItem->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, ColSpeed, speedItem);
}

void TransferQueueTable::updateItem(const TransferItem &item)
{
    const int row = rowForId(item.id);
    if (row < 0)
        return;

    if (auto *dirLabel = qobject_cast<QLabel *>(m_table->cellWidget(row, ColDirection))) {
        dirLabel->setPixmap(statusIcon(item).pixmap(20, 20));
        dirLabel->setToolTip(directionText(item.direction));
    }

    // Guarded like the ColSpeed/ColProgress lookups just below — an
    // inconsistency found by code review: this was the only one of the
    // three left unguarded, a latent null-pointer deref if any future
    // code path ever delivered an update for a row before ColStatus
    // was populated.
    if (auto *statusItem = m_table->item(row, ColStatus)) {
        const QString text = statusText(item);
        statusItem->setText(text);
        statusItem->setToolTip(text);
        statusItem->setForeground(statusTextColor(item.status));
    }

    auto *progressContainer = m_table->cellWidget(row, ColProgress);
    auto *progressBar = progressContainer ? progressContainer->findChild<QProgressBar *>() : nullptr;
    if (!progressBar)
        return;

    progressBar->setValue(percentFor(item));

    if (auto *speedItem = m_table->item(row, ColSpeed))
        speedItem->setText(speedText(item));

    // Chunk color depends on this specific item's data (status + direction),
    // so it has to be set per-widget here rather than as a single QSS rule
    // in theme.qss — see that file's comment on QProgressBar for why.
    QColor chunkColor = IconTheme::Gray();
    switch (item.status) {
    case TransferStatus::Done:      chunkColor = IconTheme::Green; break;
    case TransferStatus::Failed:    chunkColor = IconTheme::Red; break;
    case TransferStatus::Cancelled: chunkColor = IconTheme::GrayMuted(); break;
    case TransferStatus::Paused:    chunkColor = IconTheme::Amber; break;
    case TransferStatus::InProgress:
        // RemoteToRemote is phase-aware here too, matching statusIcon()'s
        // own Blue-while-downloading/Green-while-uploading — this used to
        // fall into the plain `: Green` default below and stay green for
        // the entire transfer, visibly contradicting the row's own icon
        // and "Downloading (1/2)"/"Uploading (2/2)" status text during
        // the first half.
        if (item.direction == TransferDirection::RemoteToRemote) {
            chunkColor = item.phase == TransferPhase::Uploading ? IconTheme::Green : IconTheme::Blue;
        } else if (item.direction == TransferDirection::Move) {
            // A real bug found by code review: this fell into the plain
            // RemoteToLocal-or-Green branch below, coloring an in-flight
            // Move's progress bar Green while statusIcon() colors that
            // exact same row's direction icon Blue — two status
            // indicators on one row visibly disagreeing about what's
            // happening. Matches statusIcon()'s own Move color.
            chunkColor = IconTheme::Blue;
        } else {
            chunkColor = (item.direction == TransferDirection::RemoteToLocal)
                ? IconTheme::Blue : IconTheme::Green;
        }
        break;
    case TransferStatus::Queued:
        chunkColor = IconTheme::Gray();
        break;
    case TransferStatus::Skipped:
        chunkColor = IconTheme::GrayMuted();
        break;
    }
    // Skip re-applying an unchanged stylesheet — a real, if minor, cost
    // found while investigating the resortAndRebuild() freeze above:
    // updateItem() runs on EVERY progress tick (SFTP/FTP emit
    // transferProgress unthrottled, once per raw ~480KB read chunk — see
    // SftpBackend.cpp/FtpBackend.cpp), but chunkColor only actually
    // changes on a status/direction/phase transition, not on every
    // single percentage tick in between. setStyleSheet() triggers a
    // real style repolish each call; a dynamic property remembers the
    // last color actually applied so a same-status tick (the overwhelming
    // majority of them, during any single InProgress run) touches only
    // setValue() and the two text fields above, not this.
    const QString chunkColorName = chunkColor.name();
    if (progressBar->property("zfChunkColor").toString() != chunkColorName) {
        progressBar->setProperty("zfChunkColor", chunkColorName);
        progressBar->setStyleSheet(
            QStringLiteral("QProgressBar::chunk { background-color: %1; border-radius: 3px; }")
                .arg(chunkColorName));
    }
}

void TransferQueueTable::removeItem(int id)
{
    const int row = rowForId(id);
    if (row < 0)
        return;
    m_table->removeRow(row);
    rebuildRowIndexAfterRemoval();
}

void TransferQueueTable::rebuildRowIndexAfterRemoval()
{
    m_rowById.clear();
    for (int r = 0; r < m_table->rowCount(); ++r) {
        if (auto *it = m_table->item(r, ColName))
            m_rowById.insert(it->data(IdRole).toInt(), r);
    }
}

void TransferQueueTable::showContextMenu(const QPoint &pos)
{
    const int row = m_table->rowAt(pos.y());
    if (row < 0)
        return;

    auto *nameItem = m_table->item(row, ColName);
    if (!nameItem)
        return;
    const int id = nameItem->data(IdRole).toInt();

    // Look up the item's current status from the manager rather than
    // trusting the table's displayed text — the manager is the source of
    // truth, the table is just a mirror of it.
    TransferStatus status = TransferStatus::Queued;
    TransferDirection direction = TransferDirection::Unsupported;
    bool found = false;
    for (const TransferItem &item : m_manager->items()) {
        if (item.id == id) {
            status = item.status;
            direction = item.direction;
            found = true;
            break;
        }
    }
    if (!found)
        return;

    // Pause is only meaningful for directions SftpBackend actually
    // implements it for — LocalBackend's requestPause() is a documented
    // no-op (QFile::copy() is atomic, there's no partial progress to
    // preserve), so offering Pause for a local-to-local transfer would
    // show an action that silently does nothing. Rather than that, it's
    // just not offered.
    // RemoteToRemote is now pause-capable too — it used to be excluded on
    // the theory that resuming a staged two-phase transfer would need
    // extra work to preserve phase/tempFilePath/the resume offset across
    // the pause. Code review found that reasoning had never actually been
    // verified: TransferManager's pauseItem()/resumeItem() are already
    // direction-agnostic and don't reset any of those three, so nothing
    // extra was ever needed — it just hadn't been tested.
    const bool pauseCapableDirection = direction == TransferDirection::LocalToRemote
        || direction == TransferDirection::RemoteToLocal || direction == TransferDirection::RemoteToRemote;

    // A Move item is never TransferManager's "active item" (see
    // TransferManager::moveEntry()'s doc comment) — it's dispatched
    // immediately and goes straight to InProgress outside the ordinary
    // Queued/m_activeIndex pipeline. That means cancelItem() has nothing
    // to actually interrupt (a real cancel-in-flight isn't offered for
    // v1, matching the plan's "single fast round trip, low-stakes to
    // skip" call), and retryItem() would be actively wrong: it re-queues
    // through startNext()/dispatchActiveItem(), which has no case for
    // TransferDirection::Move and would fail with a misleading generic
    // "no backend to execute this transfer" error. Both are excluded
    // here rather than left to silently misbehave; redoing "Move
    // Selected" is the retry path for a failed move instead.
    const bool isMove = direction == TransferDirection::Move;

    QMenu menu(this);
    QAction *cancelAction = menu.addAction(IconTheme::tintedIcon(":/icons/x.svg", IconTheme::Red), tr("Cancel"));
    cancelAction->setEnabled(!isMove
                              && (status == TransferStatus::Queued || status == TransferStatus::InProgress
                                  || status == TransferStatus::Paused
                                  || status == TransferStatus::PendingReconnect));
    QAction *pauseAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/player-pause.svg", IconTheme::Amber), tr("Pause"));
    pauseAction->setEnabled(status == TransferStatus::InProgress && pauseCapableDirection);
    QAction *resumeAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/player-play.svg", IconTheme::Green), tr("Resume"));
    resumeAction->setEnabled(status == TransferStatus::Paused);
    QAction *retryAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/refresh.svg", IconTheme::Amber), tr("Retry"));
    retryAction->setEnabled(!isMove
                             && (status == TransferStatus::Failed || status == TransferStatus::Cancelled
                                 || status == TransferStatus::Skipped));

    QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
    if (chosen == cancelAction)
        m_manager->cancelItem(id);
    else if (chosen == pauseAction)
        m_manager->pauseItem(id);
    else if (chosen == resumeAction)
        m_manager->resumeItem(id);
    else if (chosen == retryAction)
        m_manager->retryItem(id);
}

void TransferQueueTable::onHeaderSectionClicked(int column)
{
    // See the constructor's own comment: the indicator only starts being
    // shown once a real sort actually begins, right here. Harmless to
    // call again on every subsequent click — already true by then.
    m_table->horizontalHeader()->setSortIndicatorShown(true);
    if (column == m_sortColumn)
        m_sortOrder = (m_sortOrder == Qt::AscendingOrder) ? Qt::DescendingOrder : Qt::AscendingOrder;
    else {
        m_sortColumn = column;
        m_sortOrder = Qt::AscendingOrder;
    }
    m_table->horizontalHeader()->setSortIndicator(m_sortColumn, m_sortOrder);
    resortAndRebuild();
}

void TransferQueueTable::resortAndRebuild()
{
    QList<TransferItem> items;
    for (const TransferItem &item : m_manager->items()) {
        if (categoryFor(item.status) == m_category)
            items.append(item);
    }
    const bool ascending = (m_sortOrder == Qt::AscendingOrder);

    switch (m_sortColumn) {
    case ColName:
        std::stable_sort(items.begin(), items.end(), [ascending](const TransferItem &a, const TransferItem &b) {
            const int cmp = a.fileName.localeAwareCompare(b.fileName);
            return ascending ? cmp < 0 : cmp > 0;
        });
        break;
    case ColDirection:
        // A real bug found by code review: this compared raw
        // TransferDirection enum ordinals (declaration order), not
        // anything visible on screen — the Direction cell shows only an
        // icon and a tooltip, no sortable text, so the resulting order
        // looked arbitrary with nothing to explain it. ColStatus just
        // above sorts by its own displayed statusText(); this now does
        // the same with directionText(), the tooltip text that column's
        // icon already carries.
        std::stable_sort(items.begin(), items.end(), [ascending](const TransferItem &a, const TransferItem &b) {
            const int cmp = directionText(a.direction).localeAwareCompare(directionText(b.direction));
            return ascending ? cmp < 0 : cmp > 0;
        });
        break;
    case ColStatus:
        std::stable_sort(items.begin(), items.end(), [ascending](const TransferItem &a, const TransferItem &b) {
            const int cmp = statusText(a).localeAwareCompare(statusText(b));
            return ascending ? cmp < 0 : cmp > 0;
        });
        break;
    case ColProgress:
        std::stable_sort(items.begin(), items.end(), [ascending](const TransferItem &a, const TransferItem &b) {
            const int pa = percentFor(a);
            const int pb = percentFor(b);
            return ascending ? pa < pb : pa > pb;
        });
        break;
    case ColSpeed:
        std::stable_sort(items.begin(), items.end(), [ascending](const TransferItem &a, const TransferItem &b) {
            return ascending ? a.speedBytesPerSec < b.speedBytesPerSec : a.speedBytesPerSec > b.speedBytesPerSec;
        });
        break;
    default:
        return;   // m_sortColumn == -1 (no header clicked yet) — insertion order, nothing to rebuild
    }

    // fillRow() — NOT appendRow(), which would call insertRow() once per
    // item — populates a row that ALREADY EXISTS, from the single
    // setRowCount(items.size()) below, instead of growing the table one
    // row at a time. Not addItem() either, which would recurse straight
    // back into this method for as long as a sort is active.
    //
    // Two real performance bugs found reproducing a live report
    // ("clicked sort during a transfer and the app froze"), neither
    // caught by transfer-queue-test's own Phase 6 because it never calls
    // show() on the table it measures — the Transfers dock is always
    // visible in the real app, and that turns out to matter enormously:
    // 1. Calling insertRow() once per item (the old appendRow()-in-a-loop
    //    shape) is fine on a hidden table but, once actually shown,
    //    measured directly at roughly O(N^1.7) rather than the O(N) the
    //    hidden-table benchmark implied — each individual insertRow()
    //    call was paying for real, synchronous layout/paint work scoped
    //    to the table's current size at that moment. A single upfront
    //    setRowCount(items.size()) reduces N such calls to exactly one.
    // 2. setUpdatesEnabled(false)/(true) around the whole rebuild, since
    //    suppressing repaint scheduling during bulk mutation is the
    //    standard fix for this class of problem — kept even though, once
    //    (1) was also fixed, it measured as a small additional win, not
    //    the dominant one on its own.
    // Combined, measured directly on a real, shown table: a 1500-row
    // resort went from 1107ms to 112ms (roughly 10x), with the fixed
    // version scaling close to linearly afterward (500 rows: 32ms; 3000
    // rows: 268ms) — captured via a disposable, non-committed probe
    // (this project's established technique for a change like this that
    // doesn't warrant a new permanent test target on its own, since the
    // existing Phase 6 already covers the correctness side — just not,
    // until now, the shown-widget performance side).
    m_table->setUpdatesEnabled(false);
    m_table->setRowCount(0);
    m_table->setRowCount(items.size());
    m_rowById.clear();
    int row = 0;
    for (const TransferItem &item : items) {
        m_rowById.insert(item.id, row);
        fillRow(row, item);
        updateItem(item);
        ++row;
    }
    m_table->setUpdatesEnabled(true);
}
