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

    // The exact text shown in the Direction column's tooltip, and (since
    // a code-review fix) what resortAndRebuild()'s Direction-column sort
    // now compares — already effectively public information (anyone
    // hovering a row sees it); public here mainly so transfer-queue-test
    // can verify that sort's comparator basis directly, the same reason
    // FilePaneWidget::parentOfPath() is public.
    static QString directionText(TransferDirection direction);

    // Live Dark/Light switching — this widget doesn't take an
    // AppSettings* (unlike FilePaneWidget), so it can't self-subscribe
    // to themeChanged the same way; MainWindow::onThemeChanged() calls
    // this explicitly instead. Re-runs onItemUpdated() for every
    // currently-queued item (which already re-sets its own status/
    // direction icons and progress-bar chunk color from IconTheme) —
    // no separate re-tint logic needed, this just re-triggers the
    // existing per-row rendering path.
    void retintIcons();

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
    static QString statusText(const TransferItem &item);

    // 0-100, from bytesDone/bytesTotal — shared by onItemUpdated() (drives
    // the real progress bar) and resortAndRebuild()'s Progress-column sort
    // key, so the two can't drift apart.
    static int percentFor(const TransferItem &item);

    // The actual row-construction onItemAdded() used to do inline —
    // factored out so resortAndRebuild()'s rebuild loop can call this
    // directly instead of onItemAdded() itself, which would otherwise
    // recurse straight back into resortAndRebuild() for as long as a sort
    // is active (see onItemAdded()'s own comment on the real bug this
    // split fixes). Always appends at the CURRENT bottom row — callers
    // that care about sorted order are responsible for that themselves
    // (onItemAdded() via resortAndRebuild(); resortAndRebuild() via its
    // own already-sorted iteration order).
    void appendRow(const TransferItem &item);

    // Re-sorts m_manager->items() by (m_sortColumn, m_sortOrder) and
    // rebuilds every row from scratch via appendRow()+onItemUpdated(). A
    // full rebuild rather than QTableWidget::sortItems() specifically
    // because ColDirection and ColProgress are QWidget cell widgets
    // (setCellWidget()), not QTableWidgetItems — sortItems() only
    // reorders items, so a cell widget stays pinned to its original row
    // number while the item text around it moves, silently pairing each
    // widget with the wrong row. Rebuilding from the same TransferManager
    // data appendRow() already trusts sidesteps that entirely.
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
