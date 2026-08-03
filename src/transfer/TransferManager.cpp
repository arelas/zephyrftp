#include "TransferManager.h"
#include "../ui/FilePaneWidget.h"
#include "../backends/RemoteBackend.h"

#include <QMetaObject>
#include <QMessageBox>
#include <QCheckBox>
#include <QPushButton>
#include <QFileInfo>
#include <QWidget>

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

void TransferManager::enqueueFolder(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                                     const QString &folderName)
{
    RemoteBackend *srcBackend = sourcePane->backend();
    RemoteBackend *dstBackend = destPane->backend();

    // Same limitation as single-file transfers — no stage-through-a-temp
    // fallback exists for either backend yet.
    if (!srcBackend->isLocalFilesystem() && !dstBackend->isLocalFilesystem()) {
        emit folderTransferFailed(folderName, tr("Remote-to-remote transfers aren't supported yet"));
        return;
    }

    // Check for a destination conflict on the folder's own root BEFORE
    // paying for a full recursive enumeration of the source — if this is
    // going to be skipped, there's no point walking the source tree
    // first. Nested subdirectories that happen to already exist on the
    // destination are NOT checked individually; "Write Into" here means
    // exactly that — merge into whatever's already there, the same way
    // most file managers handle copying a folder onto an existing one.
    // Only files actually get their own per-item conflict prompt (see
    // startNext()), since asking about every nested subdirectory
    // individually would be far more prompting than this is worth.
    const QString destFolderPath = joinPath(destPane->currentDirectory(), folderName);
    ensureExistsCheckConnected(dstBackend);
    m_pendingFolderConflictCheckId = m_nextConflictCheckId++;
    m_pendingFolderSourcePane = sourcePane;
    m_pendingFolderDestPane = destPane;
    m_pendingFolderName = folderName;

    QMetaObject::invokeMethod(dstBackend, "checkExists", Qt::QueuedConnection,
                               Q_ARG(QString, destFolderPath), Q_ARG(int, m_pendingFolderConflictCheckId));
}

void TransferManager::startFolderEnumeration(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                                              const QString &folderName)
{
    RemoteBackend *srcBackend = sourcePane->backend();
    const QString rootPath = joinPath(sourcePane->currentDirectory(), folderName);
    auto *enumerator = new FolderEnumerator(srcBackend, rootPath, folderName, this);

    emit folderTransferStarted(folderName);

    connect(enumerator, &FolderEnumerator::finished, this,
            [this, enumerator, sourcePane, destPane, folderName](const QList<EnumeratedItem> &items) {
        enumerator->deleteLater();
        startFolderFileTransfers(sourcePane, destPane, folderName, items);
    });
    connect(enumerator, &FolderEnumerator::failed, this,
            [this, enumerator, folderName](const QString &reason) {
        enumerator->deleteLater();
        emit folderTransferFailed(folderName, reason);
    });

    enumerator->start();
}

void TransferManager::startFolderFileTransfers(FilePaneWidget *sourcePane, FilePaneWidget *destPane,
                                                const QString &folderName, const QList<EnumeratedItem> &items)
{
    RemoteBackend *dstBackend = destPane->backend();

    // Create every directory first, in FolderEnumerator's guaranteed
    // parent-before-child order. Not waited on individually — see this
    // method's doc comment in the header for why the queued-connection
    // FIFO ordering alone is sufficient here. "Already exists" (e.g. the
    // destination happens to already have some of this structure —
    // expected now that Write Into is an explicit, deliberate choice) is
    // not treated as an error for this bulk path — createDirectory()'s
    // fileOperationFailed signal for that case simply isn't listened to
    // here at all, unlike the single right-click "New Folder" action.
    for (const EnumeratedItem &item : items) {
        if (!item.isDir)
            continue;
        const QString destDirPath = joinPath(destPane->currentDirectory(), item.relativePath);
        QMetaObject::invokeMethod(dstBackend, "createDirectory", Qt::QueuedConnection,
                                   Q_ARG(QString, destDirPath));
    }

    // Every file goes through the ordinary enqueue() — its existing
    // joinPath(pane->currentDirectory(), fileName) logic already builds
    // the correct nested path when fileName is actually a relative path
    // like "photos/subdir/photo.jpg", so no separate file-transfer
    // mechanism is needed for folders at all. Each file still gets its
    // own overwrite/skip conflict check individually when its turn comes
    // up in startNext() — Write Into only resolved the TOP-LEVEL
    // folder's own conflict, not every file that might already exist
    // inside it.
    int fileCount = 0;
    for (const EnumeratedItem &item : items) {
        if (item.isDir)
            continue;
        enqueue(sourcePane, destPane, item.relativePath);
        fileCount++;
    }

    // fileCount == 0 is a real, valid outcome (a folder tree containing
    // only empty subdirectories) — not a failure, the directories were
    // still created above, there's just nothing left for the visible
    // transfer queue to do.
    emit folderTransferFinished(folderName, fileCount);
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

        // Check the destination for a conflict before doing anything
        // visible (no InProgress status yet, nothing dispatched to a
        // backend) — dispatchActiveItem() only runs once this comes back
        // clean, or the conflict is resolved as Overwrite.
        RemoteBackend *destBackend = item.destPane->backend();
        ensureExistsCheckConnected(destBackend);
        m_pendingFileConflictCheckId = m_nextConflictCheckId++;
        QMetaObject::invokeMethod(destBackend, "checkExists", Qt::QueuedConnection,
                                   Q_ARG(QString, item.destPath), Q_ARG(int, m_pendingFileConflictCheckId));
        return;
    }

    // Nothing left to run — a fresh batch of transfers should get fresh
    // conflict decisions rather than silently inheriting a choice from
    // an unrelated earlier transfer.
    m_activeIndex = -1;
    m_fileConflictResolution = ConflictResolution::Ask;
    m_directoryConflictResolution = ConflictResolution::Ask;
}

void TransferManager::dispatchActiveItem()
{
    if (m_activeIndex < 0 || m_activeIndex >= m_items.size())
        return;

    TransferItem &item = m_items[m_activeIndex];
    item.status = TransferStatus::InProgress;
    item.speedBytesPerSec = 0;
    emit itemUpdated(item);

    m_speedSampleTimer.start();
    m_speedSampleBytesAtLastSample = item.bytesDone;   // nonzero when resuming a Paused item
    m_smoothedSpeedBytesPerSec = 0.0;
    m_hasSpeedSample = false;

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
        startNext();   // try the next queued item instead of getting stuck
        return;
    }

    connectToBackend(executor);
    // item.bytesDone doubles as the resume offset — 0 for a fresh
    // item, nonzero when re-starting a previously Paused one (see
    // resumeItem(), which deliberately does NOT reset bytesDone the
    // way retryItem() resets it to 0).
    QMetaObject::invokeMethod(executor, methodName, Qt::QueuedConnection,
                               Q_ARG(QString, argA), Q_ARG(QString, argB),
                               Q_ARG(qint64, item.bytesDone));
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

    if (item.status == TransferStatus::Queued || item.status == TransferStatus::Paused) {
        // Neither is actively running — Queued never started, Paused
        // already stopped and isn't the active item anymore (m_activeIndex
        // moved on when it paused) — so there's nothing to interrupt,
        // just mark it done.
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
    if (item.status != TransferStatus::Failed && item.status != TransferStatus::Cancelled
        && item.status != TransferStatus::Skipped)
        return;

    item.status = TransferStatus::Queued;
    item.bytesDone = 0;
    item.bytesTotal = 0;
    item.errorMessage.clear();
    emit itemUpdated(item);

    startNext();
}

void TransferManager::pauseItem(int id)
{
    const int idx = indexById(id);
    if (idx < 0 || idx != m_activeIndex)
        return;   // only the currently-running item can be paused

    if (m_currentBackend)
        m_currentBackend->requestPause();
    // No status change here — onBackendPaused() makes that transition
    // once the backend actually confirms it stopped. Setting Paused
    // eagerly here would show a status the transfer hasn't reached yet.
}

void TransferManager::resumeItem(int id)
{
    const int idx = indexById(id);
    if (idx < 0)
        return;

    TransferItem &item = m_items[idx];
    if (item.status != TransferStatus::Paused)
        return;

    // Deliberately NOT resetting bytesDone — that's the resume offset
    // startNext() will pass through to the backend. This is the one
    // place this differs from retryItem().
    item.status = TransferStatus::Queued;
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
    connect(backend, &RemoteBackend::transferPaused, this, &TransferManager::onBackendPaused);

    m_currentBackend = backend;
}

void TransferManager::ensureExistsCheckConnected(RemoteBackend *backend)
{
    connect(backend, &RemoteBackend::existsChecked, this, &TransferManager::onDestinationExistsChecked,
            Qt::UniqueConnection);
}

bool TransferManager::askConflict(const QString &name, bool isDirectory, bool &applyToAll)
{
    QMessageBox box(qobject_cast<QWidget *>(parent()));
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(isDirectory ? tr("Folder Already Exists") : tr("File Already Exists"));
    box.setText(isDirectory
        ? tr("A folder named \"%1\" already exists at the destination.").arg(name)
        : tr("A file named \"%1\" already exists at the destination.").arg(name));

    QPushButton *proceedButton = box.addButton(
        isDirectory ? tr("Write Into") : tr("Overwrite"), QMessageBox::AcceptRole);
    QPushButton *skipButton = box.addButton(tr("Skip"), QMessageBox::RejectRole);
    box.setDefaultButton(skipButton);   // the safer choice if Enter is pressed without reading

    auto *checkbox = new QCheckBox(
        isDirectory ? tr("Do this for all remaining folder conflicts in this transfer")
                    : tr("Do this for all remaining file conflicts in this transfer"));
    box.setCheckBox(checkbox);

    box.exec();

    applyToAll = checkbox->isChecked();
    return box.clickedButton() == proceedButton;
}

void TransferManager::onDestinationExistsChecked(const QString &path, bool exists, bool isDir, int requestId)
{
    Q_UNUSED(path);
    Q_UNUSED(isDir);   // a genuine type mismatch (e.g. uploading a file where a folder of that
                       // name already exists) isn't specifically detected here — it surfaces as
                       // a normal backend error when the transfer is actually attempted, same as
                       // it always has, rather than trying to guess the right resolution for a
                       // case Overwrite/Skip doesn't really describe anyway

    if (requestId == m_pendingFolderConflictCheckId) {
        m_pendingFolderConflictCheckId = -1;
        FilePaneWidget *sourcePane = m_pendingFolderSourcePane;
        FilePaneWidget *destPane = m_pendingFolderDestPane;
        const QString folderName = m_pendingFolderName;

        if (!exists) {
            startFolderEnumeration(sourcePane, destPane, folderName);
            return;
        }

        bool proceed;
        if (m_directoryConflictResolution == ConflictResolution::AlwaysOverwrite) {
            proceed = true;
        } else if (m_directoryConflictResolution == ConflictResolution::AlwaysSkip) {
            proceed = false;
        } else {
            bool applyToAll = false;
            proceed = askConflict(folderName, /*isDirectory=*/true, applyToAll);
            if (applyToAll) {
                m_directoryConflictResolution =
                    proceed ? ConflictResolution::AlwaysOverwrite : ConflictResolution::AlwaysSkip;
            }
        }

        if (proceed)
            startFolderEnumeration(sourcePane, destPane, folderName);
        else
            emit folderTransferSkipped(folderName);
        return;
    }

    if (requestId == m_pendingFileConflictCheckId) {
        m_pendingFileConflictCheckId = -1;
        if (m_activeIndex < 0 || m_activeIndex >= m_items.size())
            return;   // active item disappeared somehow — defensive, shouldn't happen

        TransferItem &item = m_items[m_activeIndex];

        if (!exists) {
            dispatchActiveItem();
            return;
        }

        bool proceed;
        if (m_fileConflictResolution == ConflictResolution::AlwaysOverwrite) {
            proceed = true;
        } else if (m_fileConflictResolution == ConflictResolution::AlwaysSkip) {
            proceed = false;
        } else {
            bool applyToAll = false;
            proceed = askConflict(QFileInfo(item.destPath).fileName(), /*isDirectory=*/false, applyToAll);
            if (applyToAll) {
                m_fileConflictResolution =
                    proceed ? ConflictResolution::AlwaysOverwrite : ConflictResolution::AlwaysSkip;
            }
        }

        if (proceed) {
            dispatchActiveItem();
        } else {
            item.status = TransferStatus::Skipped;
            item.speedBytesPerSec = 0;
            emit itemUpdated(item);
            m_activeIndex = -1;
            startNext();
        }
        return;
    }

    // requestId matches neither pending check — a stale/unrelated
    // response (e.g. from a check that's since been superseded). Nothing
    // to do; not an error, just ignored.
}

void TransferManager::onBackendProgress(const QString &fileName, qint64 bytesDone, qint64 bytesTotal)
{
    Q_UNUSED(fileName);   // only one transfer active at a time, so no need to match by name
    if (m_activeIndex < 0 || m_activeIndex >= m_items.size())
        return;

    TransferItem &item = m_items[m_activeIndex];
    item.bytesDone = bytesDone;
    item.bytesTotal = bytesTotal;

    // Recompute speed roughly every 250ms rather than on every single
    // progress signal — SFTP's read/write loop emits one per 32KB chunk,
    // which on a fast connection would be far too frequent to be a
    // meaningful "live" number rather than noise.
    const qint64 elapsedMs = m_speedSampleTimer.isValid() ? m_speedSampleTimer.elapsed() : 0;
    if (elapsedMs >= 250) {
        const qint64 bytesSinceLastSample = bytesDone - m_speedSampleBytesAtLastSample;
        const double rawSpeed = (bytesSinceLastSample * 1000.0) / elapsedMs;

        // Exponential moving average across samples — see m_smoothedSpeedBytesPerSec's
        // doc comment for why. alpha = 0.3 is a fairly standard middle
        // ground (e.g. in the same range curl and several download
        // managers use for their own live rate display): responsive
        // enough to reflect a real speed change within a couple of
        // seconds, calm enough that single-window noise doesn't dominate
        // what's shown. The very first sample after a (re)start has
        // nothing to blend with, so it's taken as-is rather than
        // artificially damped toward zero.
        if (!m_hasSpeedSample) {
            m_smoothedSpeedBytesPerSec = rawSpeed;
            m_hasSpeedSample = true;
        } else {
            constexpr double kSmoothingAlpha = 0.3;
            m_smoothedSpeedBytesPerSec = kSmoothingAlpha * rawSpeed
                + (1.0 - kSmoothingAlpha) * m_smoothedSpeedBytesPerSec;
        }
        item.speedBytesPerSec = static_cast<qint64>(m_smoothedSpeedBytesPerSec);
        m_speedSampleBytesAtLastSample = bytesDone;
        m_speedSampleTimer.restart();
    }

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
    item.speedBytesPerSec = 0;   // not meaningful once finished — avoid showing a stale number
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
    item.speedBytesPerSec = 0;
    m_activeItemCancelled = false;
    emit itemUpdated(item);

    m_activeIndex = -1;
    startNext();
}

void TransferManager::onBackendPaused(const QString &fileName, qint64 bytesDone)
{
    Q_UNUSED(fileName);
    if (m_activeIndex < 0 || m_activeIndex >= m_items.size())
        return;

    TransferItem &item = m_items[m_activeIndex];
    item.status = TransferStatus::Paused;
    item.bytesDone = bytesDone;   // becomes the resume offset the next time this item runs
    item.speedBytesPerSec = 0;
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
