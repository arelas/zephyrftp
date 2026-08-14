#include "CompareSyncExecutor.h"

#include <QMetaObject>
#include <algorithm>

#include "../ui/FilePaneWidget.h"
#include "../transfer/TransferManager.h"
#include "../backends/RemoteBackend.h"
#include "../PathUtils.h"

CompareSyncExecutor::CompareSyncExecutor(FilePaneWidget *leftPane, FilePaneWidget *rightPane,
                                          TransferManager *transferManager, QObject *parent)
    : QObject(parent)
    , m_leftPane(leftPane)
    , m_rightPane(rightPane)
    , m_transferManager(transferManager)
{
}

void CompareSyncExecutor::copySelected(const QList<CompareEntry> &selected, CopyDirection direction)
{
    FilePaneWidget *sourcePane = (direction == CopyDirection::ToRight) ? m_leftPane : m_rightPane;
    FilePaneWidget *destPane = (direction == CopyDirection::ToRight) ? m_rightPane : m_leftPane;
    const CompareStatus sourceOnlyStatus =
        (direction == CopyDirection::ToRight) ? CompareStatus::OnlyLeft : CompareStatus::OnlyRight;

    RemoteBackend *destBackend = destPane->backend();

    // Directories first, in the order given — DirectoryComparer's own
    // parent-before-child order, preserved as long as the caller passes a
    // subset that keeps the original relative order (CompareDialog's tree
    // selection does). Mirrors TransferManager::startFolderFileTransfers()'s
    // own directory-then-file two-pass structure and its same reliance on
    // Qt's per-backend queued-connection FIFO ordering rather than waiting
    // on each call individually.
    for (const CompareEntry &item : selected) {
        if (!item.isDir)
            continue;
        if (item.status != sourceOnlyStatus)
            continue;   // a Differs+isDir row is the file/dir-type-mismatch edge case — not offered for copy
        const QString destDirPath = joinPath(destPane->currentDirectory(), item.relativePath);
        QMetaObject::invokeMethod(destBackend, "createDirectory", Qt::QueuedConnection,
                                   Q_ARG(QString, destDirPath));
    }

    for (const CompareEntry &item : selected) {
        if (item.isDir)
            continue;
        if (item.status != sourceOnlyStatus && item.status != CompareStatus::Differs)
            continue;
        const bool assumeOverwrite = (item.status == CompareStatus::Differs);
        m_transferManager->enqueue(sourcePane, destPane, item.relativePath, assumeOverwrite);
    }
}

void CompareSyncExecutor::deleteSelected(const QList<CompareEntry> &selected, DeleteSide side)
{
    FilePaneWidget *pane = (side == DeleteSide::Left) ? m_leftPane : m_rightPane;
    const QString root = pane->currentDirectory();
    // An "extra" only exists relative to ONE side: OnlyLeft is extra on
    // the left, OnlyRight is extra on the right. Filtering on the status
    // that actually matches the requested side (rather than accepting
    // either status for either side) guards against a caller bug passing
    // an OnlyRight row through with side == Left, which would otherwise
    // delete whatever happens to sit at that relative path on the WRONG
    // pane — a real data-loss risk for a destructive action, not just
    // defensive style.
    const CompareStatus extraStatus = (side == DeleteSide::Left) ? CompareStatus::OnlyLeft : CompareStatus::OnlyRight;

    QList<QPair<QString, bool>> filePairs;
    QList<CompareEntry> dirEntries;
    for (const CompareEntry &entry : selected) {
        if (entry.status != extraStatus)
            continue;   // never Differs/Identical, and never the OTHER side's "only" status
        if (entry.isDir)
            dirEntries.append(entry);
        else
            filePairs.append({joinPath(root, entry.relativePath), false});
    }

    if (!filePairs.isEmpty())
        pane->deleteEntriesAt(filePairs);

    // Deepest-first (most '/' segments first) so every directory is
    // already empty — its own children, and any deeper subdirectories,
    // were already deleted earlier in this same pass — by the time its
    // non-recursive deleteEntry() call runs.
    std::sort(dirEntries.begin(), dirEntries.end(), [](const CompareEntry &a, const CompareEntry &b) {
        return a.relativePath.count(QLatin1Char('/')) > b.relativePath.count(QLatin1Char('/'));
    });

    QList<QPair<QString, bool>> dirPairs;
    dirPairs.reserve(dirEntries.size());
    for (const CompareEntry &entry : dirEntries)
        dirPairs.append({joinPath(root, entry.relativePath), true});

    if (!dirPairs.isEmpty())
        pane->deleteEntriesAt(dirPairs);
}
