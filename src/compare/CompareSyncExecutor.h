#pragma once

#include <QObject>
#include <QList>
#include "CompareTypes.h"

class FilePaneWidget;
class TransferManager;

// Turns a set of user-selected CompareEntry rows (from a DirectoryComparer
// result, reviewed in CompareDialog) into actual backend calls — kept
// separate from DirectoryComparer (read-only diff) and CompareDialog (UI)
// so diffing, displaying, and executing stay three separate
// responsibilities.
class CompareSyncExecutor : public QObject {
    Q_OBJECT
public:
    enum class CopyDirection { ToLeft, ToRight };
    enum class DeleteSide { Left, Right };

    CompareSyncExecutor(FilePaneWidget *leftPane, FilePaneWidget *rightPane,
                         TransferManager *transferManager, QObject *parent = nullptr);

    // Copies every selected OnlyLeft/OnlyRight/Differs row in the given
    // direction, reusing TransferManager::enqueue() exactly as-is (it
    // already accepts a multi-segment relative path and joins it
    // correctly) — no new transfer machinery. Directories missing on the
    // destination are created first, in the order given (DirectoryComparer
    // already returns parent-before-child order, preserved as long as the
    // caller's selection subset keeps the original relative order). A
    // Differs row is copied with TransferManager::enqueue()'s
    // assumeOverwrite set, since the destination conflict is already known
    // and expected from the diff itself — otherwise a bulk sync of many
    // changed files would pop one Overwrite/Skip modal per file. A row
    // whose two sides disagree on file-vs-directory (Differs && isDir,
    // the only way that combination occurs — see DirectoryComparer's own
    // comment) is skipped entirely: ambiguous, not offered for copy in the
    // UI, and not attempted here defensively either.
    void copySelected(const QList<CompareEntry> &selected, CopyDirection direction);

    // Deletes every selected row whose status is "extra" for the given
    // side — OnlyLeft rows when side == Left, OnlyRight rows when side ==
    // Right — never the opposite "only" status, and never Differs/
    // Identical (deleting a "different" file isn't what this is for;
    // replacing it is a copy-with-overwrite, handled above). A caller
    // passing a mismatched status for the given side (e.g. an OnlyRight
    // row with side == Left) has that row silently skipped rather than
    // acted on — see this method's own .cpp comment for why that's a
    // defensive necessity, not just filtering.
    // Files are deleted first, in one batched FilePaneWidget::
    // deleteEntriesAt() call; directories are then deleted in a SECOND,
    // separate batched call, sorted deepest-first by relative-path depth
    // — guaranteeing every directory is already empty by the time its
    // non-recursive deleteEntry() runs, using only the tree already known
    // from the diff (no re-enumeration, no new recursive-delete primitive
    // anywhere). The two calls must stay sequential, not combined, so
    // Qt's per-backend queued-connection FIFO ordering guarantees every
    // file delete is dispatched before the first directory delete is even
    // queued.
    void deleteSelected(const QList<CompareEntry> &selected, DeleteSide side);

private:
    FilePaneWidget *m_leftPane;
    FilePaneWidget *m_rightPane;
    TransferManager *m_transferManager;
};
