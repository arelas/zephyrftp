#pragma once

#include <QWidget>
#include <QPoint>
#include <QIcon>
#include <QColor>
#include "../transfer/TransferItem.h"

class QTableWidget;
class TransferManager;

// Table showing the transfer queue: filename, direction, status, progress.
// Mostly a view mirroring TransferManager, but also the entry point for
// per-item actions (right-click Cancel/Retry) — those just call back into
// the manager, which owns all the actual state.
class TransferQueueWidget : public QWidget {
    Q_OBJECT
public:
    explicit TransferQueueWidget(TransferManager *manager, QWidget *parent = nullptr);

private slots:
    void onItemAdded(const TransferItem &item);
    void onItemUpdated(const TransferItem &item);
    void showContextMenu(const QPoint &pos);

    // Clicking a header sorts by that column; clicking the same one again
    // reverses it — QTableWidget's own sortItems() isn't used for this
    // (see resortAndRebuild()'s doc comment on why), so this has to be
    // driven manually off QHeaderView::sectionClicked instead of the
    // usual setSortingEnabled(true).
    void onHeaderSectionClicked(int column);

private:
    int rowForId(int id) const;
    static QString directionText(TransferDirection direction);
    static QString statusText(const TransferItem &item);

    // 0-100, from bytesDone/bytesTotal — shared by onItemUpdated() (drives
    // the real progress bar) and resortAndRebuild()'s Progress-column sort
    // key, so the two can't drift apart.
    static int percentFor(const TransferItem &item);

    // Re-sorts m_manager->items() by (m_sortColumn, m_sortOrder) and
    // rebuilds every row from scratch via onItemAdded()+onItemUpdated().
    // A full rebuild rather than QTableWidget::sortItems() specifically
    // because ColDirection and ColProgress are QWidget cell widgets
    // (setCellWidget()), not QTableWidgetItems — sortItems() only
    // reorders items, so a cell widget stays pinned to its original row
    // number while the item text around it moves, silently pairing each
    // widget with the wrong row. Rebuilding from the same TransferManager
    // data onItemAdded() already trusts sidesteps that entirely.
    void resortAndRebuild();

    int m_sortColumn = -1;   // -1: unsorted, insertion order (the pre-sort default)
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;

    // Direction arrow (up/down/left-right) recolored per the item's
    // current status, or check/x for the two terminal states — per
    // ICON-MAP.md's "Transfer direction & status" table.
    static QIcon statusIcon(const TransferItem &item);
    static QColor statusTextColor(TransferStatus status);

    // "1.2 MB/s" style formatting — empty string when not meaningful
    // (anything other than InProgress; see TransferManager, which zeroes
    // speedBytesPerSec on every terminal/paused transition).
    static QString speedText(const TransferItem &item);

    QTableWidget *m_table;
    TransferManager *m_manager;
};
