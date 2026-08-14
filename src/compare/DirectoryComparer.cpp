#include "DirectoryComparer.h"

#include <QHash>
#include <QSet>

DirectoryComparer::DirectoryComparer(RemoteBackend *leftBackend, const QString &leftRoot,
                                      RemoteBackend *rightBackend, const QString &rightRoot,
                                      QObject *parent)
    : QObject(parent)
    , m_leftBackend(leftBackend)
    , m_leftRoot(leftRoot)
    , m_rightBackend(rightBackend)
    , m_rightRoot(rightRoot)
{
}

void DirectoryComparer::start()
{
    // rootName == "" deliberately — see the header's own comment on why,
    // and on the two side effects (synthetic empty-path root item,
    // leading '/' on every other result) classify() has to undo.
    m_leftEnumerator = new FolderEnumerator(m_leftBackend, m_leftRoot, QString(), this);
    m_rightEnumerator = new FolderEnumerator(m_rightBackend, m_rightRoot, QString(), this);

    connect(m_leftEnumerator, &FolderEnumerator::finished, this, &DirectoryComparer::onLeftFinished);
    connect(m_leftEnumerator, &FolderEnumerator::failed, this, &DirectoryComparer::onLeftFailed);
    connect(m_rightEnumerator, &FolderEnumerator::finished, this, &DirectoryComparer::onRightFinished);
    connect(m_rightEnumerator, &FolderEnumerator::failed, this, &DirectoryComparer::onRightFailed);

    m_leftEnumerator->start();
    m_rightEnumerator->start();
}

void DirectoryComparer::onLeftFinished(const QList<EnumeratedItem> &items)
{
    if (m_failed)
        return;
    m_leftItems = items;
    m_leftDone = true;
    maybeClassify();
}

void DirectoryComparer::onRightFinished(const QList<EnumeratedItem> &items)
{
    if (m_failed)
        return;
    m_rightItems = items;
    m_rightDone = true;
    maybeClassify();
}

void DirectoryComparer::onLeftFailed(const QString &reason)
{
    if (m_failed)
        return;
    m_failed = true;
    emit failed(QStringLiteral("Left: %1").arg(reason));
}

void DirectoryComparer::onRightFailed(const QString &reason)
{
    if (m_failed)
        return;
    m_failed = true;
    emit failed(QStringLiteral("Right: %1").arg(reason));
}

void DirectoryComparer::maybeClassify()
{
    if (m_failed || !m_leftDone || !m_rightDone)
        return;
    classify();
}

namespace {

// Strips FolderEnumerator's leading '/' (an artifact of passing an empty
// rootName — see DirectoryComparer.h's own comment) and drops the
// synthetic root item (relativePath == "" before stripping).
QList<EnumeratedItem> normalized(const QList<EnumeratedItem> &items)
{
    QList<EnumeratedItem> result;
    result.reserve(items.size());
    for (const EnumeratedItem &item : items) {
        if (item.relativePath.isEmpty())
            continue;
        EnumeratedItem copy = item;
        if (copy.relativePath.startsWith(QLatin1Char('/')))
            copy.relativePath = copy.relativePath.mid(1);
        result.append(copy);
    }
    return result;
}

} // namespace

void DirectoryComparer::classify()
{
    const QList<EnumeratedItem> leftItems = normalized(m_leftItems);
    const QList<EnumeratedItem> rightItems = normalized(m_rightItems);

    QHash<QString, const EnumeratedItem *> leftMap;
    leftMap.reserve(leftItems.size());
    for (const EnumeratedItem &item : leftItems)
        leftMap.insert(item.relativePath, &item);

    QHash<QString, const EnumeratedItem *> rightMap;
    rightMap.reserve(rightItems.size());
    for (const EnumeratedItem &item : rightItems)
        rightMap.insert(item.relativePath, &item);

    // Preserve FolderEnumerator's parent-before-child ordering: walk the
    // left list first in its own discovery order, then append any
    // right-only paths in the right list's own order — a QHash-keyed
    // union alone would lose this, and the compare tree relies on it to
    // build top-down without a separate sort pass.
    QList<QString> orderedPaths;
    QSet<QString> seen;
    orderedPaths.reserve(leftItems.size() + rightItems.size());
    for (const EnumeratedItem &item : leftItems) {
        if (!seen.contains(item.relativePath)) {
            orderedPaths.append(item.relativePath);
            seen.insert(item.relativePath);
        }
    }
    for (const EnumeratedItem &item : rightItems) {
        if (!seen.contains(item.relativePath)) {
            orderedPaths.append(item.relativePath);
            seen.insert(item.relativePath);
        }
    }

    QList<CompareEntry> results;
    results.reserve(orderedPaths.size());

    for (const QString &path : orderedPaths) {
        const EnumeratedItem *left = leftMap.value(path, nullptr);
        const EnumeratedItem *right = rightMap.value(path, nullptr);

        CompareEntry entry;
        entry.relativePath = path;

        if (left && !right) {
            entry.isDir = left->isDir;
            entry.status = CompareStatus::OnlyLeft;
            entry.leftSize = left->size;
            entry.leftModified = left->modified;
        } else if (!left && right) {
            entry.isDir = right->isDir;
            entry.status = CompareStatus::OnlyRight;
            entry.rightSize = right->size;
            entry.rightModified = right->modified;
        } else {
            // Present on both sides.
            entry.leftSize = left->size;
            entry.leftModified = left->modified;
            entry.rightSize = right->size;
            entry.rightModified = right->modified;

            if (left->isDir != right->isDir) {
                // A file on one side, a directory of the same relative
                // path on the other — can't be merged into one row's
                // normal semantics. Surfaced as Differs (with copy
                // disabled for this row in the UI) rather than silently
                // picked one way, per this class's own doc comment.
                entry.isDir = left->isDir;
                entry.status = CompareStatus::Differs;
            } else if (left->isDir) {
                // Both directories: a directory's own "content" is its
                // children, which get their own rows — nothing on the
                // directory entry itself is worth comparing.
                entry.isDir = true;
                entry.status = CompareStatus::Identical;
            } else {
                entry.isDir = false;
                const bool same = left->size == right->size && left->modified == right->modified;
                entry.status = same ? CompareStatus::Identical : CompareStatus::Differs;
            }
        }

        results.append(entry);
    }

    emit finished(results);
}
