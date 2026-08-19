#pragma once

#include <QWidget>
#include <QHash>
#include <QIcon>
#include <QColor>
#include "../transfer/TransferItem.h"

class QTableWidget;
class TransferManager;

// Which of TransferQueueWidget's three tabs an item currently belongs
// in, purely a function of its TransferStatus.
enum class QueueCategory { Active, Completed, Failed };

// Active: still queued, running, paused, or waiting to reconnect —
// anything not yet at a terminal outcome. Completed: Done. Failed:
// Failed, Cancelled, or Skipped — everything retryItem() itself already
// accepts (TransferManager.cpp), grouped together since "didn't finish
// successfully, might want to retry" is the same user intent regardless
// of which of the three specific terminal statuses produced it.
QueueCategory categoryFor(TransferStatus status);

// One tab's worth of the transfer queue: a single sortable table plus
// the per-item actions context menu (Cancel/Pause/Resume/Retry) —
// extracted from what used to be TransferQueueWidget's ENTIRE
// implementation, before the queue grew a second and third table
// sharing all of this same logic (see TransferQueueWidget's own class
// doc comment for why: separate Active/Completed/Failed tabs, with
// items migrating between them as their status changes, a direct user
// request). TransferQueueWidget owns three instances — one per tab —
// and routes every TransferManager::itemAdded/itemUpdated signal to
// whichever instance currently matches the item's category, migrating
// a row between two instances via removeItem()+addItem() when an
// item's category actually changes (Failed -> Active on retry,
// InProgress -> Completed on Done, etc.). This class itself has no
// notion of tabs or categories other than its own — it's just "one
// filtered table," which is also what keeps resortAndRebuild() (see
// its own doc comment) correct without any cross-table coordination.
class TransferQueueTable : public QWidget {
    Q_OBJECT
public:
    // category is used only by resortAndRebuild(), to filter
    // manager->items() down to just what belongs in THIS instance —
    // TransferQueueWidget is what actually decides whether a given item
    // is ever routed here in the first place (addItem()/removeItem()
    // below).
    TransferQueueTable(TransferManager *manager, QueueCategory category, QWidget *parent = nullptr);

    // Appends item as a new row (or, if a sort is currently active,
    // coalesces into a deferred full rebuild — see onHeaderSectionClicked()'s
    // own doc comment for why). Always brings the row fully up to date
    // with item's CURRENT state (not just insertion defaults) before
    // returning — essential, not just tidy, for a migrated item (e.g.
    // arriving here already Done from another instance), since nothing
    // later may ever call updateItem() again for it.
    void addItem(const TransferItem &item);

    // Updates an EXISTING row in place — a no-op if id isn't currently
    // held here (the caller is responsible for knowing which instance
    // currently owns a given item; see TransferQueueWidget's own
    // m_categoryById).
    void updateItem(const TransferItem &item);

    // Removes an existing row — used when TransferQueueWidget migrates
    // an item to a DIFFERENT instance because its category changed.
    void removeItem(int id);

    int rowCount() const { return m_rowById.size(); }

    // Every item id currently held here — TransferQueueWidget's own
    // tab-header "Clear" action (see its own doc comment) uses this to
    // gather exactly what's in the clicked tab RIGHT NOW, before handing
    // the list to TransferManager::clearItems().
    QList<int> itemIds() const { return m_rowById.keys(); }

    // The exact text shown in the Direction column's tooltip, and what
    // resortAndRebuild()'s Direction-column sort compares — public so
    // TransferQueueWidget::directionText() (transfer-queue-test's own
    // public API, unaffected by the Active/Completed/Failed split) can
    // forward to this class's real implementation without duplicating
    // it, the same reason FilePaneWidget::parentOfPath() is public.
    static QString directionText(TransferDirection direction);

signals:
    // The one piece of forwarding this class needs beyond its own direct
    // m_manager calls (see showContextMenu()'s own comment) — "Verify
    // Checksum..." needs ChecksumVerifier, which this class has no
    // reference to; TransferQueueWidget (which already constructs every
    // instance of this class) connects this to
    // ChecksumVerifier::verify().
    void verifyChecksumRequested(int itemId);

private slots:
    void showContextMenu(const QPoint &pos);

    // Clicking a header sorts by that column; clicking the same one
    // again reverses it — QTableWidget's own sortItems() isn't used for
    // this (see resortAndRebuild()'s own doc comment on why), so this
    // has to be driven manually off QHeaderView::sectionClicked instead
    // of the usual setSortingEnabled(true).
    void onHeaderSectionClicked(int column);

private:
    int rowForId(int id) const;
    void appendRow(const TransferItem &item);
    void fillRow(int row, const TransferItem &item);

    // Re-sorts manager->items() (filtered down to just m_category) by
    // (m_sortColumn, m_sortOrder) and rebuilds every row from scratch
    // via appendRow()+updateItem(). A full rebuild rather than
    // QTableWidget::sortItems() specifically because the Direction and
    // Progress columns are QWidget cell widgets (setCellWidget()), not
    // QTableWidgetItems — sortItems() only reorders items, so a cell
    // widget stays pinned to its original row number while the item
    // text around it moves, silently pairing each widget with the
    // wrong row.
    void resortAndRebuild();

    // Every row after a removed one shifted up by one index — cheaper
    // and simpler to rebuild the whole id->row map from the table's own
    // current state than to patch each entry by hand. Fine cost-wise:
    // removal only happens once per item's whole lifetime here (a
    // category migration), not a hot per-progress-tick path the way
    // updateItem()'s own rowForId() lookup is.
    void rebuildRowIndexAfterRemoval();

    static QString statusText(const TransferItem &item);
    static QIcon statusIcon(const TransferItem &item);
    static QColor statusTextColor(TransferStatus status);

    // 0-100, from bytesDone/bytesTotal — shared by updateItem() (drives
    // the real progress bar) and resortAndRebuild()'s Progress-column
    // sort key, so the two can't drift apart.
    static int percentFor(const TransferItem &item);

    // "1.2 MB/s" style formatting — empty string when not meaningful
    // (anything other than InProgress; see TransferManager, which zeroes
    // speedBytesPerSec on every terminal/paused transition).
    static QString speedText(const TransferItem &item);

    TransferManager *m_manager;
    QueueCategory m_category;
    QTableWidget *m_table;

    int m_sortColumn = -1;   // -1: unsorted, insertion order (the pre-sort default)
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;

    // Coalesces addItem()'s resortAndRebuild() calls while a sort is
    // active — see onHeaderSectionClicked()'s own doc comment for the
    // real, severe bug this fixes (an O(N²) rebuild storm on a large
    // batch enqueue).
    bool m_rebuildPending = false;

    // item id -> current row — addItem() O(1) lookups instead of a
    // linear scan on every progress tick (a real O(N) cost found from a
    // live large-batch-hang report). Kept in sync by appendRow() (every
    // insert) and resortAndRebuild()/rebuildRowIndexAfterRemoval() (full
    // repopulate, since either renumbers every row).
    QHash<int, int> m_rowById;
};
