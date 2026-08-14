#pragma once

#include <QDialog>
#include <QList>
#include <QHash>
#include <functional>
#include "../compare/CompareTypes.h"
#include "../compare/CompareSyncExecutor.h"

class FilePaneWidget;
class TransferManager;
class DirectoryComparer;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QCheckBox;
class QPushButton;
class QProgressBar;

// The dedicated compare-and-sync view: diffs the two panes' CURRENT
// directories (snapshotted once, at construction — see m_leftRoot/
// m_rightRoot's own comment) recursively via DirectoryComparer, shows the
// full tree with a status per relative path, and lets the user copy or
// (opt-in, itemized) delete the differences via CompareSyncExecutor.
//
// A non-modal QDialog (show(), never exec()) rather than a QDockWidget —
// unlike the transfers/commands docks, a compare result is a one-shot
// snapshot that goes stale the moment either pane changes, not a
// persistent always-current panel; non-modal specifically so the transfer
// queue dock stays interactive (watchable) while this stays open for a
// possible follow-up delete pass.
class CompareDialog : public QDialog {
    Q_OBJECT
public:
    CompareDialog(FilePaneWidget *leftPane, FilePaneWidget *rightPane,
                  TransferManager *transferManager, QWidget *parent = nullptr);

signals:
    // Emitted right after a copy batch is queued, so MainWindow can raise/
    // show the transfers dock — this dialog deliberately doesn't duplicate
    // a progress UI of its own.
    void copyQueued();

private:
    void buildUi();
    void runCompare();
    void onCompareFinished(const QList<CompareEntry> &results);
    void onCompareFailed(const QString &reason);
    void populateTree();
    void onItemChanged(QTreeWidgetItem *item, int column);
    void applyHideIdentical(bool hide);

    // True only while both panes are still exactly where they were when
    // the compare was run. Checked before any copy/delete action — both
    // TransferManager::enqueue() and the delete path resolve paths via
    // the pane's LIVE currentDirectory() at call time, not at diff time,
    // so acting after either pane navigated away would silently operate
    // against the wrong directory.
    bool anchorsStillValid() const;

    // Every tree row currently checked whose CompareEntry satisfies
    // statusMatches — used by each action handler with its own predicate
    // (e.g. "OnlyLeft or Differs" for Copy to Right).
    QList<CompareEntry> checkedEntriesMatching(const std::function<bool(CompareStatus)> &statusMatches) const;

    void onCopyToRight();
    void onCopyToLeft();
    void onDeleteExtras();

    FilePaneWidget *m_leftPane;
    FilePaneWidget *m_rightPane;
    TransferManager *m_transferManager;
    CompareSyncExecutor *m_executor;
    DirectoryComparer *m_comparer = nullptr;

    // Snapshotted once, at construction — same "anchor at the moment"
    // pattern MainWindow's own m_syncAnchorLeft/m_syncAnchorRight use for
    // synchronized browsing, though this is an entirely separate concept
    // (this dialog works independently of whether that toggle is on).
    QString m_leftRoot;
    QString m_rightRoot;

    QList<CompareEntry> m_results;

    QLabel *m_rootsLabel = nullptr;
    QLabel *m_progressLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QTreeWidget *m_tree = nullptr;
    QCheckBox *m_hideIdenticalCheck = nullptr;
    QCheckBox *m_allowDeleteCheck = nullptr;
    QPushButton *m_copyToRightButton = nullptr;
    QPushButton *m_copyToLeftButton = nullptr;
    QPushButton *m_deleteExtrasButton = nullptr;
    QLabel *m_staleWarningLabel = nullptr;

    bool m_cascading = false;   // reentrancy guard for directory-checkbox cascade
};
