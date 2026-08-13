#include "EditSessionManager.h"
#include "../AppSettings.h"
#include "../transfer/TransferManager.h"
#include "../transfer/TransferItem.h"
#include "FilePaneWidget.h"
#include "../backends/RemoteBackend.h"
#include "../PathUtils.h"

#include <QFileSystemWatcher>
#include <QTimer>
#include <QDesktopServices>
#include <QProcess>
#include <QUrl>
#include <QMessageBox>
#include <QPushButton>
#include <QFile>

namespace {
// Long enough to coalesce the handful of duplicate fileChanged events
// some editors/OSes fire per save, short enough that a re-upload still
// feels immediate.
constexpr int kSaveDebounceMs = 400;
}

EditSessionManager::EditSessionManager(TransferManager *transferManager, AppSettings *settings, QObject *parent)
    : QObject(parent)
    , m_transferManager(transferManager)
    , m_settings(settings)
{
    connect(m_transferManager, &TransferManager::itemUpdated, this, &EditSessionManager::onTransferItemUpdated);
}

QString EditSessionManager::sessionKey(FilePaneWidget *pane, const QString &remotePath) const
{
    return QString::number(reinterpret_cast<quintptr>(pane)) + QLatin1Char('|') + remotePath;
}

void EditSessionManager::startEditing(FilePaneWidget *pane, const RemoteEntry &entry)
{
    const QString remotePath = joinPath(pane->currentDirectory(), entry.name);
    const QString key = sessionKey(pane, remotePath);

    const auto existing = m_sessions.find(key);
    if (existing != m_sessions.end()) {
        // Already editing this file — re-launch the editor on the
        // existing temp file rather than downloading it again.
        launchEditor(existing->localTempPath);
        return;
    }

    Session session;
    session.pane = pane;
    session.remotePath = remotePath;
    session.fileName = entry.name;
    session.backend = pane->backend();
    m_sessions.insert(key, session);

    const int id = m_transferManager->startEditDownload(pane, entry.name);
    m_downloadItemToSessionKey.insert(id, key);
}

void EditSessionManager::onTransferItemUpdated(const TransferItem &item)
{
    // Every other direction (ordinary transfers, moves) is irrelevant
    // here — only react to the two this class itself started. Also
    // ignore non-terminal updates (Queued/InProgress/Paused progress
    // ticks); this class only cares about Done/Failed/Cancelled.
    if (item.status != TransferStatus::Done && item.status != TransferStatus::Failed
        && item.status != TransferStatus::Cancelled)
        return;

    if (item.direction == TransferDirection::EditDownload) {
        const QString key = m_downloadItemToSessionKey.take(item.id);
        if (key.isEmpty() || !m_sessions.contains(key))
            return;
        if (item.status == TransferStatus::Done)
            onDownloadFinished(key, item.destPath);
        else
            onDownloadFailed(key, item.errorMessage);
    } else if (item.direction == TransferDirection::EditUpload) {
        const QString key = m_uploadItemToSessionKey.take(item.id);
        // Session may already be gone (endSessionsForPane() ran while
        // this upload was in flight) — nothing left to react against.
        if (key.isEmpty() || !m_sessions.contains(key))
            return;
        if (item.status == TransferStatus::Done)
            emit editUploadSucceeded(m_sessions.value(key).fileName);
        else
            onUploadFailed(key, item.errorMessage);
    }
}

void EditSessionManager::onDownloadFinished(const QString &key, const QString &tempPath)
{
    const auto it = m_sessions.find(key);
    if (it == m_sessions.end())
        return;
    it->localTempPath = tempPath;

    it->watcher = new QFileSystemWatcher(this);
    it->watcher->addPath(tempPath);
    connect(it->watcher, &QFileSystemWatcher::fileChanged, this, [this, key](const QString &path) {
        onFileChanged(key, path);
    });

    it->debounceTimer = new QTimer(this);
    it->debounceTimer->setSingleShot(true);
    connect(it->debounceTimer, &QTimer::timeout, this, [this, key]() {
        startUpload(key);
    });

    launchEditor(tempPath);
}

void EditSessionManager::onDownloadFailed(const QString &key, const QString &reason)
{
    const auto it = m_sessions.find(key);
    if (it == m_sessions.end())
        return;
    const QString fileName = it->fileName;
    m_sessions.erase(it);   // nothing was ever downloaded -- no temp file, no watcher, nothing to keep

    QMessageBox::warning(qobject_cast<QWidget *>(parent()), tr("Edit"),
        reason.isEmpty() ? tr("Couldn't download \"%1\" for editing.").arg(fileName)
                          : tr("Couldn't download \"%1\" for editing:\n%2").arg(fileName, reason));
}

void EditSessionManager::onFileChanged(const QString &key, const QString &path)
{
    const auto it = m_sessions.find(key);
    if (it == m_sessions.end())
        return;

    // Many editors save via write-to-temp-then-rename, which drops the
    // underlying OS watch after the first change — re-adding
    // defensively on every fire is the standard workaround. Harmless
    // even if the path is (briefly) gone: addPath() on a momentarily
    // missing file is a no-op, and the very next save recreates it.
    if (it->watcher && !it->watcher->files().contains(path))
        it->watcher->addPath(path);

    it->debounceTimer->start(kSaveDebounceMs);
}

void EditSessionManager::startUpload(const QString &key)
{
    const auto it = m_sessions.find(key);
    if (it == m_sessions.end())
        return;

    if (!it->backend) {
        QMessageBox::warning(qobject_cast<QWidget *>(parent()), tr("Edit"),
            tr("\"%1\" was edited, but the connection it came from is gone. "
               "Your edit is still saved locally at:\n%2").arg(it->fileName, it->localTempPath));
        return;
    }

    const int id = m_transferManager->startEditUpload(it->pane, it->localTempPath, it->remotePath, it->fileName);
    m_uploadItemToSessionKey.insert(id, key);
}

void EditSessionManager::onUploadFailed(const QString &key, const QString &reason)
{
    const auto it = m_sessions.find(key);
    if (it == m_sessions.end())
        return;

    QMessageBox box(QMessageBox::Warning, tr("Edit"),
        reason.isEmpty()
            ? tr("\"%1\" couldn't be saved back to the server. Your edit is still on disk at:\n%2")
                  .arg(it->fileName, it->localTempPath)
            : tr("\"%1\" couldn't be saved back to the server:\n%2\n\nYour edit is still on disk at:\n%3")
                  .arg(it->fileName, reason, it->localTempPath),
        QMessageBox::NoButton, qobject_cast<QWidget *>(parent()));
    QPushButton *retryButton = box.addButton(tr("Retry"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Close);
    box.exec();

    if (box.clickedButton() == retryButton)
        startUpload(key);
}

void EditSessionManager::launchEditor(const QString &localPath) const
{
    const QString command = m_settings->externalEditorCommand().trimmed();
    if (command.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
        return;
    }
    QProcess::startDetached(command, {localPath});
}

void EditSessionManager::endSession(const QString &key)
{
    const auto it = m_sessions.find(key);
    if (it == m_sessions.end())
        return;

    if (it->watcher)
        it->watcher->deleteLater();
    if (it->debounceTimer)
        it->debounceTimer->deleteLater();
    QFile::remove(it->localTempPath);   // safe no-op if empty/nonexistent

    m_sessions.erase(it);
}

void EditSessionManager::endSessionsForPane(FilePaneWidget *pane)
{
    QStringList keysToEnd;
    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        if (it->pane == pane)
            keysToEnd.append(it.key());
    }
    for (const QString &key : keysToEnd)
        endSession(key);
}

void EditSessionManager::endAllSessions()
{
    const QStringList keys = m_sessions.keys();
    for (const QString &key : keys)
        endSession(key);
}
