#include "FolderEnumerator.h"
#include "../backends/RemoteBackend.h"
#include "../PathUtils.h"

#include <QMetaObject>

// See the header's own comment on s_nextRequestId for why this is a
// static, class-wide counter rather than a per-instance one.
int FolderEnumerator::s_nextRequestId = 1;

FolderEnumerator::FolderEnumerator(RemoteBackend *backend, const QString &rootPath,
                                    const QString &rootName, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_rootPath(rootPath)
    , m_rootName(rootName)
{
    connect(m_backend, &RemoteBackend::directoryEnumerated, this, &FolderEnumerator::onDirectoryEnumerated);
    connect(m_backend, &RemoteBackend::enumerationFailed, this, &FolderEnumerator::onEnumerationFailed);
}

void FolderEnumerator::start()
{
    // The root folder itself is always the first result — it needs to
    // exist on the destination before anything found inside it, and its
    // name is what everything else nests under.
    EnumeratedItem rootItem;
    rootItem.relativePath = m_rootName;
    rootItem.isDir = true;
    m_results.append(rootItem);

    m_pending.append({m_rootPath, m_rootName});
    enumerateNext();
}

void FolderEnumerator::enumerateNext()
{
    if (m_pending.isEmpty()) {
        emit finished(m_results);
        return;
    }

    m_activeDir = m_pending.takeFirst();
    m_activeRequestId = s_nextRequestId++;

    QMetaObject::invokeMethod(m_backend, "listDirectoryForEnumeration", Qt::QueuedConnection,
                               Q_ARG(QString, m_activeDir.absolutePath), Q_ARG(int, m_activeRequestId));
}

void FolderEnumerator::onDirectoryEnumerated(const QString &path, const QList<RemoteEntry> &entries, int requestId)
{
    Q_UNUSED(path);
    if (requestId != m_activeRequestId)
        return;   // not the request we're currently waiting on — defensive, shouldn't happen given strict serial processing

    for (const RemoteEntry &entry : entries) {
        EnumeratedItem item;
        item.relativePath = joinPath(m_activeDir.relativePrefix, entry.name);
        item.isDir = entry.isDir;
        item.size = entry.size;
        item.modified = entry.modified;
        m_results.append(item);

        if (entry.isDir) {
            m_pending.append({joinPath(m_activeDir.absolutePath, entry.name), item.relativePath});
        }
    }

    enumerateNext();
}

void FolderEnumerator::onEnumerationFailed(const QString &path, const QString &reason, int requestId)
{
    if (requestId != m_activeRequestId)
        return;

    emit failed(QStringLiteral("%1: %2").arg(path, reason));
}
