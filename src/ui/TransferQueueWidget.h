#pragma once

#include <QWidget>
#include <QHash>
#include "../transfer/TransferItem.h"
#include "TransferQueueTable.h"

class TransferManager;
class QTabWidget;

// Three tabs — Active, Completed, Failed — splitting what used to be one
// flat table by each item's own status category (see QueueCategory in
// TransferQueueTable.h for the exact status->category mapping). A direct
// user request: watching a batch finish, or finding a failed item to
// retry, used to mean scrolling or sorting through everything else still
// queued or already done to find it. Items migrate between tabs live as
// their status changes — most visibly, retrying a Failed item moves it
// straight back to the Active tab, the same live-migration mechanism as
// any other item whose category just changed (e.g. Active -> Completed
// on Done). The actual per-tab table/sort/context-menu implementation
// lives in TransferQueueTable — this class owns three instances and is
// only responsible for routing TransferManager's signals to whichever
// one currently matches each item.
class TransferQueueWidget : public QWidget {
    Q_OBJECT
public:
    explicit TransferQueueWidget(TransferManager *manager, QWidget *parent = nullptr);

    // Forwards to TransferQueueTable::directionText() — kept as a public
    // static method here too so transfer-queue-test's own existing calls
    // (TransferQueueWidget::directionText(...)) keep working unchanged.
    static QString directionText(TransferDirection direction) {
        return TransferQueueTable::directionText(direction);
    }

    // Live Dark/Light switching — this widget doesn't take an
    // AppSettings* (unlike FilePaneWidget), so it can't self-subscribe
    // to themeChanged the same way; MainWindow::onThemeChanged() calls
    // this explicitly instead. Re-runs updateItem() (via whichever tab
    // currently holds it) for every item TransferManager currently
    // knows about — no separate re-tint logic needed, this just
    // re-triggers each item's existing per-row rendering path.
    void retintIcons();

private slots:
    void onItemAdded(const TransferItem &item);
    void onItemUpdated(const TransferItem &item);

private:
    TransferQueueTable *tableFor(QueueCategory category) const;
    void updateTabLabel(QueueCategory category);

    TransferManager *m_manager;
    QTabWidget *m_tabs;
    TransferQueueTable *m_activeTable;
    TransferQueueTable *m_completedTable;
    TransferQueueTable *m_failedTable;

    // item id -> which tab currently holds it — lets onItemUpdated()
    // detect a category change (migrate: remove from the old tab, add
    // to the new one) vs. an ordinary in-place update, in O(1), without
    // asking all three tables "do you have this id".
    QHash<int, QueueCategory> m_categoryById;
};
