#pragma once

#include <QWidget>
#include <QHash>
#include "../transfer/TransferItem.h"
#include "TransferQueueTable.h"

class TransferManager;
class ChecksumVerifier;
class QTabWidget;
class QProgressDialog;

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

    // Must be called exactly once, early — BEFORE QApplication::
    // setStyleSheet() and before any widget is constructed (see
    // main.cpp's own call site). Fixes this app's only QTabBar
    // rendering left-aligned instead of QMacStyle's own centered
    // default on macOS — deliberately NOT done via QWidget::setStyle()
    // on the tab bar itself (tried first, twice): that call is
    // documented to disconnect a widget from the app's own stylesheet
    // cascade, which broke this app's QSS background-color rule for
    // the tab bar specifically, confirmed via two separate real macOS
    // screenshots. An app-wide QProxyStyle installed before the
    // stylesheet even loads doesn't have this problem — Qt's own
    // automatic QStyleSheetStyle wrapping still applies normally to
    // every widget, including this one, same as if this function were
    // never called at all.
    static void installTabBarAlignmentFix();

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

    // Fired once per id TransferManager::clearItems() actually hid —
    // removes that row from whichever tab currently holds it (looked up
    // via m_categoryById, same as onItemUpdated()'s own migration path)
    // and refreshes that tab's label count.
    void onItemRemoved(int id);

    // Right-click on a tab HEADER (not a row) — see buildUi()'s own
    // wiring of this to m_tabs->tabBar()'s customContextMenuRequested.
    // Offers "Clear Completed"/"Clear Failed" on those two tabs (enabled
    // only when that tab actually has something in it); the Active tab
    // gets the same action shown but permanently disabled — there's
    // nothing safe to silently clear there (everything in it is still
    // queued, in progress, paused, or waiting to reconnect; Cancel is
    // the right action for those, a different one this menu doesn't
    // offer), and showing a disabled action reads as "not applicable
    // here" rather than the right-click looking broken/inert.
    void onTabBarContextMenuRequested(const QPoint &pos);

    // "Verify Checksum..." wiring — this class owns the ChecksumVerifier
    // (see ChecksumVerifier.h's own doc comment on why: it needs
    // presentation (QMessageBox/QProgressDialog), which the individual
    // TransferQueueTable instances don't have, but MainWindow doesn't
    // need to be involved either — this widget already sits right above
    // all three tables and already owns the TransferManager* they share).
    void onVerifyChecksumRequested(int itemId);
    void onVerificationProgress(int itemId, qint64 bytesDone, qint64 bytesTotal);
    void onVerificationFinished(int itemId, bool matched, const QString &localHex, const QString &remoteHex);
    void onVerificationFailed(int itemId, const QString &reason);

private:
    TransferQueueTable *tableFor(QueueCategory category) const;
    void updateTabLabel(QueueCategory category);
    void closeVerifyDialog(int itemId);
    QString fileNameForItem(int id) const;

    TransferManager *m_manager;
    ChecksumVerifier *m_checksumVerifier;
    QTabWidget *m_tabs;
    TransferQueueTable *m_activeTable;
    TransferQueueTable *m_completedTable;
    TransferQueueTable *m_failedTable;

    // One progress dialog per in-flight verification, keyed by the
    // ORIGINAL item's id — supports verifying more than one transfer at
    // once, same as the app already supports concurrent transfers
    // themselves. Removed (and deleteLater()'d) the moment
    // verificationFinished/verificationFailed arrives for that id.
    QHash<int, QProgressDialog *> m_verifyDialogs;

    // item id -> which tab currently holds it — lets onItemUpdated()
    // detect a category change (migrate: remove from the old tab, add
    // to the new one) vs. an ordinary in-place update, in O(1), without
    // asking all three tables "do you have this id".
    QHash<int, QueueCategory> m_categoryById;
};
