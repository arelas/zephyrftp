#include "TransferManager.h"
#include "../ui/FilePaneWidget.h"
#include "../backends/RemoteBackend.h"

#include <QMetaObject>

namespace {
QString joinPath(const QString &dir, const QString &name)
{
    return dir.endsWith('/') ? dir + name : dir + '/' + name;
}
}

TransferManager::TransferManager(QObject *parent)
    : QObject(parent)
{
}

void TransferManager::enqueue(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                               const QString &fileName)
{
    TransferItem item;
    item.id = m_nextId++;
    item.fileName = fileName;
    item.sourcePane = sourcePane;
    item.destPane = destPane;
    item.sourcePath = joinPath(sourcePane->currentDirectory(), fileName);
    item.destPath = joinPath(destPane->currentDirectory(), fileName);

    const bool srcLocal = sourcePane->backend()->isLocalFilesystem();
    const bool dstLocal = destPane->backend()->isLocalFilesystem();

    if (srcLocal && dstLocal) {
        item.direction = TransferDirection::LocalToLocal;
    } else if (srcLocal && !dstLocal) {
        item.direction = TransferDirection::LocalToRemote;
    } else if (!srcLocal && dstLocal) {
        item.direction = TransferDirection::RemoteToLocal;
    } else {
        // Remote-to-remote: neither backend can currently do a direct
        // server-to-server transfer, and there's no stage-through-a-temp-
        // file fallback yet either. Reported as a failed item immediately
        // rather than silently dropped, so it's visible in the queue.
        item.direction = TransferDirection::Unsupported;
        item.status = TransferStatus::Failed;
        item.errorMessage = tr("Remote-to-remote transfers aren't supported yet");
    }

    m_items.append(item);
    emit itemAdded(m_items.last());

    if (item.status == TransferStatus::Queued)
        startNext();
}

void TransferManager::startNext()
{
    if (m_activeIndex != -1)
        return;   // already processing something; this item will get picked up when it finishes

    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].status != TransferStatus::Queued)
            continue;

        m_activeIndex = i;
        TransferItem &item = m_items[i];
        item.status = TransferStatus::InProgress;
        emit itemUpdated(item);

        RemoteBackend *srcBackend = item.sourcePane->backend();
        RemoteBackend *dstBackend = item.destPane->backend();

        RemoteBackend *executor = nullptr;
        const char *methodName = nullptr;
        QString argA, argB;

        switch (item.direction) {
        case TransferDirection::LocalToRemote:
            // uploadFile(localPath, remotePath) — runs on the remote side's backend
            executor = dstBackend;
            methodName = "uploadFile";
            argA = item.sourcePath;
            argB = item.destPath;
            break;
        case TransferDirection::RemoteToLocal:
            // downloadFile(remotePath, localPath) — runs on the remote side's backend
            executor = srcBackend;
            methodName = "downloadFile";
            argA = item.sourcePath;
            argB = item.destPath;
            break;
        case TransferDirection::LocalToLocal:
            // Either backend is a plain LocalBackend here; uploadFile's
            // (localPath, remotePath) signature just becomes (src, dst)
            // for LocalBackend's QFile::copy-based implementation.
            executor = dstBackend;
            methodName = "uploadFile";
            argA = item.sourcePath;
            argB = item.destPath;
            break;
        case TransferDirection::Unsupported:
            // Shouldn't reach here — enqueue() marks these Failed up front
            // so they never get queued as Queued in the first place.
            break;
        }

        if (!executor) {
            item.status = TransferStatus::Failed;
            item.errorMessage = tr("Internal error: no backend to execute this transfer");
            emit itemUpdated(item);
            m_activeIndex = -1;
            continue;   // try the next queued item instead of getting stuck
        }

        connectToBackend(executor);
        QMetaObject::invokeMethod(executor, methodName, Qt::QueuedConnection,
                                   Q_ARG(QString, argA), Q_ARG(QString, argB));
        return;
    }

    // Nothing left to run.
    m_activeIndex = -1;
}

void TransferManager::cancelItem(int id)
{
    const int idx = indexById(id);
    if (idx < 0)
        return;

    TransferItem &item = m_items[idx];

    if (idx == m_activeIndex) {
        // Currently running — ask the executing backend to stop, and
        // remember to report the resulting transferFailed as Cancelled
        // rather than Failed once it arrives.
        if (m_currentBackend) {
            m_activeItemCancelled = true;
            m_currentBackend->requestCancel();
        }
        return;
    }

    if (item.status == TransferStatus::Queued) {
        // Hasn't started yet — nothing to interrupt, just mark it done.
        item.status = TransferStatus::Cancelled;
        emit itemUpdated(item);
    }
    // Done/Failed/Cancelled items: no-op, nothing meaningful to cancel.
}

void TransferManager::retryItem(int id)
{
    const int idx = indexById(id);
    if (idx < 0)
        return;

    TransferItem &item = m_items[idx];
    if (item.status != TransferStatus::Failed && item.status != TransferStatus::Cancelled)
        return;

    item.status = TransferStatus::Queued;
    item.bytesDone = 0;
    item.bytesTotal = 0;
    item.errorMessage.clear();
    emit itemUpdated(item);

    startNext();
}

void TransferManager::connectToBackend(RemoteBackend *backend)
{
    if (m_currentBackend)
        disconnect(m_currentBackend, nullptr, this, nullptr);

    connect(backend, &RemoteBackend::transferProgress, this, &TransferManager::onBackendProgress);
    connect(backend, &RemoteBackend::transferFinished, this, &TransferManager::onBackendFinished);
    connect(backend, &RemoteBackend::transferFailed, this, &TransferManager::onBackendFailed);

    m_currentBackend = backend;
}

void TransferManager::onBackendProgress(const QString &fileName, qint64 bytesDone, qint64 bytesTotal)
{
    Q_UNUSED(fileName);   // only one transfer active at a time, so no need to match by name
    if (m_activeIndex < 0 || m_activeIndex >= m_items.size())
        return;

    TransferItem &item = m_items[m_activeIndex];
    item.bytesDone = bytesDone;
    item.bytesTotal = bytesTotal;
    emit itemUpdated(item);
}

void TransferManager::onBackendFinished(const QString &fileName)
{
    Q_UNUSED(fileName);
    if (m_activeIndex < 0 || m_activeIndex >= m_items.size())
        return;

    TransferItem &item = m_items[m_activeIndex];
    item.status = TransferStatus::Done;
    item.bytesDone = item.bytesTotal > 0 ? item.bytesTotal : item.bytesDone;
    m_activeItemCancelled = false;   // in case cancelItem() was called just as this finished anyway
    emit itemUpdated(item);
    emit transferSucceeded();

    m_activeIndex = -1;
    startNext();
}

void TransferManager::onBackendFailed(const QString &fileName, const QString &reason)
{
    Q_UNUSED(fileName);
    if (m_activeIndex < 0 || m_activeIndex >= m_items.size())
        return;

    TransferItem &item = m_items[m_activeIndex];
    item.status = m_activeItemCancelled ? TransferStatus::Cancelled : TransferStatus::Failed;
    item.errorMessage = m_activeItemCancelled ? QString() : reason;
    m_activeItemCancelled = false;
    emit itemUpdated(item);

    m_activeIndex = -1;
    startNext();
}

int TransferManager::indexById(int id) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id == id)
            return i;
    }
    return -1;
}
