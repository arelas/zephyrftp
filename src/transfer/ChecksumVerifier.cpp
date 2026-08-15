#include "ChecksumVerifier.h"
#include "TransferManager.h"
#include "TransferItem.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

namespace {

struct HashOutcome {
    bool ok = false;
    QString hex;
    QString errorString;
};

// Runs off the GUI thread via QtConcurrent::run() — same reasoning as
// LocalBackend::downloadFile()/uploadFile()'s own performCopy(): reading
// a whole file (addData(QIODevice*) does this in one call) is a real,
// previously-shipped blocking-the-GUI-thread bug class for a large file,
// not a hypothetical one.
HashOutcome hashFileTask(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {false, QString(), file.errorString()};

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {false, QString(), file.errorString()};

    return {true, QString::fromLatin1(hash.result().toHex()), QString()};
}

} // namespace

ChecksumVerifier::ChecksumVerifier(TransferManager *manager, QObject *parent)
    : QObject(parent)
    , m_manager(manager)
{
    connect(m_manager, &TransferManager::itemUpdated, this, &ChecksumVerifier::onManagerItemUpdated);
}

void ChecksumVerifier::verify(int originalItemId)
{
    if (m_pending.contains(originalItemId))
        return;

    const TransferItem *found = nullptr;
    for (const TransferItem &candidate : m_manager->items()) {
        if (candidate.id == originalItemId) {
            found = &candidate;
            break;
        }
    }

    if (!found) {
        emit verificationFailed(originalItemId, tr("This transfer is no longer in the queue."));
        return;
    }
    if (found->status != TransferStatus::Done) {
        emit verificationFailed(originalItemId, tr("Only a completed transfer can be verified."));
        return;
    }

    // Copied by value, deliberately, rather than keeping the pointer
    // above alive across this switch: startRemoteReread() below calls
    // through to TransferManager::startVerificationDownload(), which
    // appends to TransferManager's own item list and can reallocate it —
    // `found` would otherwise risk dangling mid-switch.
    const TransferItem item = *found;

    switch (item.direction) {
    case TransferDirection::LocalToLocal:
        m_pending.insert(originalItemId, PendingVerification());
        startLocalHash(originalItemId, item.sourcePath);
        startOtherLocalHash(originalItemId, item.destPath);
        break;
    case TransferDirection::LocalToRemote:
    case TransferDirection::EditUpload:
        // The local source is real; the remote DESTINATION is the side
        // that needs an independent re-read.
        m_pending.insert(originalItemId, PendingVerification());
        startLocalHash(originalItemId, item.sourcePath);
        startRemoteReread(originalItemId, item.destPane, item.destPath, item.fileName);
        break;
    case TransferDirection::RemoteToLocal:
    case TransferDirection::EditDownload:
        // The local destination is real; the remote SOURCE is the side
        // that needs an independent re-read.
        m_pending.insert(originalItemId, PendingVerification());
        startLocalHash(originalItemId, item.destPath);
        startRemoteReread(originalItemId, item.sourcePane, item.sourcePath, item.fileName);
        break;
    default:
        // RemoteToRemote/Move/Unsupported — out of scope, see
        // ChecksumVerifier.h's own doc comment. TransferQueueTable
        // shouldn't offer the action for these in the first place; this
        // is the authoritative check, not a redundant belt-and-braces one.
        emit verificationFailed(originalItemId, tr("This transfer type can't be verified."));
        break;
    }
}

void ChecksumVerifier::cancel(int originalItemId)
{
    auto it = m_pending.find(originalItemId);
    if (it == m_pending.end())
        return;

    if (it->verifyItemId >= 0) {
        m_manager->cancelItem(it->verifyItemId);
        m_verifyItemIdToOriginalId.remove(it->verifyItemId);
    }
    m_pending.erase(it);
    emit verificationFailed(originalItemId, tr("Verification cancelled."));
}

void ChecksumVerifier::startLocalHash(int originalItemId, const QString &path)
{
    auto *watcher = new QFutureWatcher<HashOutcome>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, originalItemId]() {
        const HashOutcome outcome = watcher->result();
        watcher->deleteLater();
        onLocalHashFinished(originalItemId, outcome.ok, outcome.hex, outcome.errorString);
    });
    watcher->setFuture(QtConcurrent::run(hashFileTask, path));
}

void ChecksumVerifier::startOtherLocalHash(int originalItemId, const QString &path)
{
    auto *watcher = new QFutureWatcher<HashOutcome>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, originalItemId]() {
        const HashOutcome outcome = watcher->result();
        watcher->deleteLater();
        onOtherHashFinished(originalItemId, outcome.ok, outcome.hex, outcome.errorString);
    });
    watcher->setFuture(QtConcurrent::run(hashFileTask, path));
}

void ChecksumVerifier::startRemoteReread(int originalItemId, FilePaneWidget *remotePane,
                                          const QString &remotePath, const QString &fileName)
{
    const int verifyItemId = m_manager->startVerificationDownload(remotePane, remotePath, fileName);

    auto it = m_pending.find(originalItemId);
    if (it == m_pending.end())
        return;   // shouldn't happen — verify() always inserts before calling this
    it->verifyItemId = verifyItemId;
    m_verifyItemIdToOriginalId.insert(verifyItemId, originalItemId);
}

void ChecksumVerifier::finishWithRemoteFile(int originalItemId, const QString &tempPath)
{
    auto *watcher = new QFutureWatcher<HashOutcome>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, originalItemId, tempPath]() {
        const HashOutcome outcome = watcher->result();
        watcher->deleteLater();
        QFile::remove(tempPath);   // done with the temp download either way
        onOtherHashFinished(originalItemId, outcome.ok, outcome.hex, outcome.errorString);
    });
    watcher->setFuture(QtConcurrent::run(hashFileTask, tempPath));
}

void ChecksumVerifier::onLocalHashFinished(int originalItemId, bool ok, const QString &hex,
                                            const QString &errorString)
{
    auto it = m_pending.find(originalItemId);
    if (it == m_pending.end())
        return;   // cancelled (or already finished) before this callback arrived — safe no-op

    it->localFinished = true;
    it->localOk = ok;
    it->localHex = hex;
    it->localError = errorString;
    maybeFinish(originalItemId);
}

void ChecksumVerifier::onOtherHashFinished(int originalItemId, bool ok, const QString &hex,
                                            const QString &errorString)
{
    auto it = m_pending.find(originalItemId);
    if (it == m_pending.end())
        return;   // same as onLocalHashFinished() above

    it->otherFinished = true;
    it->otherOk = ok;
    it->otherHex = hex;
    it->otherError = errorString;
    maybeFinish(originalItemId);
}

void ChecksumVerifier::maybeFinish(int originalItemId)
{
    auto it = m_pending.find(originalItemId);
    if (it == m_pending.end())
        return;
    if (!it->localFinished || !it->otherFinished)
        return;   // still waiting on the other side

    const bool localOk = it->localOk;
    const bool otherOk = it->otherOk;
    const QString localHex = it->localHex;
    const QString otherHex = it->otherHex;
    const QString localError = it->localError;
    const QString otherError = it->otherError;
    const int verifyItemId = it->verifyItemId;

    if (verifyItemId >= 0)
        m_verifyItemIdToOriginalId.remove(verifyItemId);
    m_pending.erase(it);

    if (!localOk) {
        emit verificationFailed(originalItemId, localError.isEmpty()
            ? tr("Could not read the local file for hashing.") : localError);
        return;
    }
    if (!otherOk) {
        emit verificationFailed(originalItemId, otherError.isEmpty()
            ? tr("Could not verify the remote file.") : otherError);
        return;
    }

    const bool matched = localHex.compare(otherHex, Qt::CaseInsensitive) == 0;
    emit verificationFinished(originalItemId, matched, localHex, otherHex);
}

void ChecksumVerifier::onManagerItemUpdated(const TransferItem &item)
{
    const auto vit = m_verifyItemIdToOriginalId.constFind(item.id);
    if (vit == m_verifyItemIdToOriginalId.constEnd())
        return;   // not a temp-download this class started — nothing to do

    const int originalItemId = vit.value();

    if (item.status == TransferStatus::InProgress) {
        emit verificationProgress(originalItemId, item.bytesDone, item.bytesTotal);
        return;
    }
    if (item.status == TransferStatus::Done) {
        finishWithRemoteFile(originalItemId, item.destPath);
        return;
    }
    if (item.status == TransferStatus::Failed || item.status == TransferStatus::Cancelled) {
        QFile::remove(item.destPath);   // partial temp file, if any

        auto it = m_pending.find(originalItemId);
        const QString reason = (item.status == TransferStatus::Cancelled)
            ? tr("Verification cancelled.")
            : (item.errorMessage.isEmpty() ? tr("Could not verify the remote file.") : item.errorMessage);

        m_verifyItemIdToOriginalId.erase(vit);
        if (it != m_pending.end())
            m_pending.erase(it);

        emit verificationFailed(originalItemId, reason);
        return;
    }
    // Queued/Paused/Skipped/PendingReconnect: nothing to react to yet.
}
