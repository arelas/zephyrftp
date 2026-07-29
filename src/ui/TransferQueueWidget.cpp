#include "TransferQueueWidget.h"
#include "../transfer/TransferManager.h"
#include "IconTheme.h"

#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QMenu>
#include <QAction>
#include <QProgressBar>

namespace {
constexpr int ColName = 0;
constexpr int ColDirection = 1;
constexpr int ColStatus = 2;
constexpr int ColProgress = 3;
constexpr int ColSpeed = 4;
constexpr int IdRole = Qt::UserRole;
}

TransferQueueWidget::TransferQueueWidget(TransferManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_table(new QTableWidget(this))
    , m_manager(manager)
{
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels(
        {tr("File"), tr("Direction"), tr("Status"), tr("Progress"), tr("Speed")});
    m_table->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableWidget::customContextMenuRequested,
            this, &TransferQueueWidget::showContextMenu);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_table);

    connect(manager, &TransferManager::itemAdded, this, &TransferQueueWidget::onItemAdded);
    connect(manager, &TransferManager::itemUpdated, this, &TransferQueueWidget::onItemUpdated);
}

int TransferQueueWidget::rowForId(int id) const
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *item = m_table->item(row, ColName);
        if (item && item->data(IdRole).toInt() == id)
            return row;
    }
    return -1;
}

QString TransferQueueWidget::directionText(TransferDirection direction)
{
    switch (direction) {
    case TransferDirection::LocalToRemote: return tr("local -> remote");
    case TransferDirection::RemoteToLocal: return tr("remote -> local");
    case TransferDirection::LocalToLocal:  return tr("local copy");
    case TransferDirection::Unsupported:   return tr("unsupported");
    }
    return {};
}

QString TransferQueueWidget::statusText(const TransferItem &item)
{
    switch (item.status) {
    case TransferStatus::Queued:      return tr("Queued");
    case TransferStatus::InProgress:  return tr("Transferring");
    case TransferStatus::Paused:      return tr("Paused");
    case TransferStatus::Done:        return tr("Done");
    case TransferStatus::Failed:      return item.errorMessage.isEmpty()
                                              ? tr("Failed")
                                              : tr("Failed: %1").arg(item.errorMessage);
    case TransferStatus::Cancelled:   return tr("Cancelled");
    }
    return {};
}

QIcon TransferQueueWidget::statusIcon(const TransferItem &item)
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
        return IconTheme::tintedIcon(":/icons/x.svg", IconTheme::GrayMuted);
    if (item.status == TransferStatus::Paused)
        return IconTheme::tintedIcon(":/icons/player-pause.svg", IconTheme::Amber);

    // Queued or InProgress: a direction-shaped icon (the mockup doesn't
    // cover local-to-local or unsupported directions, since it assumes
    // only upload/download exist — arrows-left-right and alert-triangle
    // are this app's own extensions of the same visual language).
    QString path;
    switch (item.direction) {
    case TransferDirection::LocalToRemote: path = ":/icons/arrow-up.svg"; break;
    case TransferDirection::RemoteToLocal: path = ":/icons/arrow-down.svg"; break;
    case TransferDirection::LocalToLocal:  path = ":/icons/arrows-left-right.svg"; break;
    case TransferDirection::Unsupported:   path = ":/icons/alert-triangle.svg"; break;
    }

    QColor color = IconTheme::Gray;   // Queued default
    if (item.status == TransferStatus::InProgress) {
        switch (item.direction) {
        case TransferDirection::LocalToRemote: color = IconTheme::Green; break;   // active upload
        case TransferDirection::RemoteToLocal: color = IconTheme::Blue; break;    // active download
        default: color = IconTheme::Gray; break;
        }
    }
    return IconTheme::tintedIcon(path, color);
}

QColor TransferQueueWidget::statusTextColor(TransferStatus status)
{
    switch (status) {
    case TransferStatus::Queued:      return IconTheme::Gray;
    case TransferStatus::InProgress:  return IconTheme::Blue;
    case TransferStatus::Paused:      return IconTheme::Amber;
    case TransferStatus::Done:        return IconTheme::Green;
    case TransferStatus::Failed:      return IconTheme::Red;
    case TransferStatus::Cancelled:   return IconTheme::GrayMuted;
    }
    return IconTheme::Gray;
}

QString TransferQueueWidget::speedText(const TransferItem &item)
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

void TransferQueueWidget::onItemAdded(const TransferItem &item)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);

    auto *nameItem = new QTableWidgetItem(item.fileName);
    nameItem->setData(IdRole, item.id);
    m_table->setItem(row, ColName, nameItem);

    auto *dirItem = new QTableWidgetItem();
    dirItem->setIcon(statusIcon(item));
    dirItem->setToolTip(directionText(item.direction));
    m_table->setItem(row, ColDirection, dirItem);

    auto *statusItem = new QTableWidgetItem(statusText(item));
    statusItem->setForeground(statusTextColor(item.status));
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
    m_table->setCellWidget(row, ColProgress, progressBar);

    m_table->setItem(row, ColSpeed, new QTableWidgetItem());
}

void TransferQueueWidget::onItemUpdated(const TransferItem &item)
{
    const int row = rowForId(item.id);
    if (row < 0)
        return;

    m_table->item(row, ColDirection)->setIcon(statusIcon(item));
    m_table->item(row, ColDirection)->setToolTip(directionText(item.direction));

    auto *statusItem = m_table->item(row, ColStatus);
    statusItem->setText(statusText(item));
    statusItem->setForeground(statusTextColor(item.status));

    auto *progressBar = qobject_cast<QProgressBar *>(m_table->cellWidget(row, ColProgress));
    if (!progressBar)
        return;

    int percent = 0;
    if (item.status == TransferStatus::InProgress && item.bytesTotal > 0)
        percent = static_cast<int>((item.bytesDone * 100) / item.bytesTotal);
    else if (item.status == TransferStatus::Done)
        percent = 100;
    else if (item.status == TransferStatus::Paused && item.bytesTotal > 0)
        percent = static_cast<int>((item.bytesDone * 100) / item.bytesTotal);   // frozen at the pause point
    progressBar->setValue(percent);

    if (auto *speedItem = m_table->item(row, ColSpeed))
        speedItem->setText(speedText(item));

    // Chunk color depends on this specific item's data (status + direction),
    // so it has to be set per-widget here rather than as a single QSS rule
    // in theme.qss — see that file's comment on QProgressBar for why.
    QColor chunkColor = IconTheme::Gray;
    switch (item.status) {
    case TransferStatus::Done:      chunkColor = IconTheme::Green; break;
    case TransferStatus::Failed:    chunkColor = IconTheme::Red; break;
    case TransferStatus::Cancelled: chunkColor = IconTheme::GrayMuted; break;
    case TransferStatus::Paused:    chunkColor = IconTheme::Amber; break;
    case TransferStatus::InProgress:
        chunkColor = (item.direction == TransferDirection::RemoteToLocal)
            ? IconTheme::Blue : IconTheme::Green;
        break;
    case TransferStatus::Queued:
        chunkColor = IconTheme::Gray;
        break;
    }
    progressBar->setStyleSheet(
        QStringLiteral("QProgressBar::chunk { background-color: %1; border-radius: 3px; }")
            .arg(chunkColor.name()));
}

void TransferQueueWidget::showContextMenu(const QPoint &pos)
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
    const bool pauseCapableDirection =
        direction == TransferDirection::LocalToRemote || direction == TransferDirection::RemoteToLocal;

    QMenu menu(this);
    QAction *cancelAction = menu.addAction(IconTheme::tintedIcon(":/icons/x.svg", IconTheme::Red), tr("Cancel"));
    cancelAction->setEnabled(status == TransferStatus::Queued || status == TransferStatus::InProgress
                              || status == TransferStatus::Paused);
    QAction *pauseAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/player-pause.svg", IconTheme::Amber), tr("Pause"));
    pauseAction->setEnabled(status == TransferStatus::InProgress && pauseCapableDirection);
    QAction *resumeAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/player-play.svg", IconTheme::Green), tr("Resume"));
    resumeAction->setEnabled(status == TransferStatus::Paused);
    QAction *retryAction = menu.addAction(
        IconTheme::tintedIcon(":/icons/refresh.svg", IconTheme::Amber), tr("Retry"));
    retryAction->setEnabled(status == TransferStatus::Failed || status == TransferStatus::Cancelled);

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
