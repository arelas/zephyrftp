#include "LocalBackend.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QStandardPaths>

namespace {
// Renders the full owner/group/other rwx string, same layout and
// reasoning as SftpBackend.cpp's parseSftpEntry() (factored out there
// for the same reason: two independent copies of this drifted once
// already — see listDirectory()/listDirectoryForEnumeration() below,
// which used to each hand-roll their own, and only ever showed the
// owner's READ bit as a single 'r'/'-', discarding write/execute and
// the entire group/other columns). Deliberately QFileDevice::Read/Write/
// ExeOwner (the file's actual mode bits), not the ...User variants,
// which report whether the CURRENT process can access the file —
// a different, effective-access question, not what this column means
// on the SFTP side it's displayed next to.
QString renderPermissions(QFileDevice::Permissions permissions)
{
    char perms[10] = "---------";
    if (permissions & QFileDevice::ReadOwner)  perms[0] = 'r';
    if (permissions & QFileDevice::WriteOwner) perms[1] = 'w';
    if (permissions & QFileDevice::ExeOwner)   perms[2] = 'x';
    if (permissions & QFileDevice::ReadGroup)  perms[3] = 'r';
    if (permissions & QFileDevice::WriteGroup) perms[4] = 'w';
    if (permissions & QFileDevice::ExeGroup)   perms[5] = 'x';
    if (permissions & QFileDevice::ReadOther)  perms[6] = 'r';
    if (permissions & QFileDevice::WriteOther) perms[7] = 'w';
    if (permissions & QFileDevice::ExeOther)   perms[8] = 'x';
    return QString::fromLatin1(perms, 9);
}
}

LocalBackend::LocalBackend(QObject *parent)
    : RemoteBackend(parent)
    , m_currentPath(QDir::homePath())
{
}

void LocalBackend::connectToHost()
{
    // Local side is always "connected" — emit immediately so MainWindow's
    // connection-state handling stays uniform across both panes.
    emit connected();
}

void LocalBackend::listDirectory(const QString &path)
{
    QDir dir(path.isEmpty() ? m_currentPath : path);
    if (!dir.exists()) {
        emit connectionFailed(QStringLiteral("No such directory: %1").arg(path));
        return;
    }

    m_currentPath = dir.absolutePath();

    QList<RemoteEntry> entries;
    // QDir::Hidden included so dotfiles reach FilePaneWidget at all —
    // filtering them here unconditionally (the previous behavior) meant
    // AppSettings::showHiddenFiles() had nothing to reveal for the local
    // pane specifically, while SftpBackend/FtpBackend never filtered them
    // out of their own listings in the first place. FilePaneWidget's
    // rebuildModel() is now the one place that decides visibility,
    // uniformly across all three backends.
    const auto infoList = dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot,
                                             QDir::DirsFirst | QDir::Name);
    for (const QFileInfo &info : infoList) {
        RemoteEntry e;
        e.name = info.fileName();
        e.isDir = info.isDir();
        e.isSymlink = info.isSymLink();
        e.size = info.isDir() ? 0 : info.size();
        e.modified = info.lastModified();
        e.permissions = renderPermissions(info.permissions());
        entries.append(e);
    }

    emit directoryListed(m_currentPath, entries);
}

void LocalBackend::listDirectoryForEnumeration(const QString &path, int requestId)
{
    QDir dir(path);
    if (!dir.exists()) {
        emit enumerationFailed(path, QStringLiteral("No such directory: %1").arg(path), requestId);
        return;
    }

    // Deliberately does NOT touch m_currentPath — see RemoteBackend's
    // doc comment on why this needs to be a separate call from
    // listDirectory() rather than reusing it.
    //
    // QDir::Hidden included here too, unconditionally — a whole-folder
    // transfer shouldn't silently skip a source folder's dotfiles just
    // because they're hidden from the *browsing* view; "show hidden
    // files" is a display preference (FilePaneWidget's concern, via
    // AppSettings, which this backend has no reference to and shouldn't),
    // never a "what actually gets transferred" one. This also matches
    // SftpBackend/FtpBackend, which never filtered dotfiles out of their
    // own enumeration to begin with.
    QList<RemoteEntry> entries;
    const auto infoList = dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot,
                                             QDir::DirsFirst | QDir::Name);
    for (const QFileInfo &info : infoList) {
        RemoteEntry e;
        e.name = info.fileName();
        e.isDir = info.isDir();
        e.isSymlink = info.isSymLink();
        e.size = info.isDir() ? 0 : info.size();
        e.modified = info.lastModified();
        e.permissions = renderPermissions(info.permissions());
        entries.append(e);
    }

    emit directoryEnumerated(dir.absolutePath(), entries, requestId);
}

void LocalBackend::checkExists(const QString &path, int requestId)
{
    const QFileInfo info(path);
    emit existsChecked(path, info.exists(), info.isDir(), requestId);
}

void LocalBackend::downloadFile(const QString &remotePath, const QString &localPath, qint64 resumeOffset)
{
    Q_UNUSED(resumeOffset);   // never nonzero in practice — see requestPause()'s doc comment
    // "download" here just means local-to-local copy, used when the local
    // pane is the destination of a transfer initiated from the remote pane.
    //
    // QFile::copy() is atomic from our side — it doesn't report incremental
    // progress. Emitting a 0-of-size / size-of-size pair around it is honest
    // about that (no fake intermediate percentages) while still giving the
    // transfer queue a real byte count to show instead of a blank column.
    QFileInfo info(remotePath);
    const qint64 size = info.size();
    emit transferProgress(remotePath, 0, size);

    // QFile::copy() specifically refuses to overwrite an existing
    // destination (unlike QFile::open(WriteOnly), which truncates) — found
    // via the transfer-queue-test's retry phase failing on a second copy
    // of the same file. Remove any existing destination first so this
    // matches SftpBackend's overwrite behavior (LIBSSH2_FXF_TRUNC on
    // upload; QFile::open(WriteOnly) truncates on download).
    if (QFile::exists(localPath) && !QFile::remove(localPath)) {
        emit transferFailed(remotePath, QStringLiteral("Could not overwrite existing file: %1").arg(localPath));
        return;
    }

    QFile src(remotePath);
    if (!src.copy(localPath)) {
        emit transferFailed(remotePath, src.errorString());
        return;
    }
    emit transferProgress(remotePath, size, size);
    emit transferFinished(remotePath);
}

void LocalBackend::uploadFile(const QString &localPath, const QString &remotePath, qint64 resumeOffset)
{
    Q_UNUSED(resumeOffset);
    QFileInfo info(localPath);
    const qint64 size = info.size();
    emit transferProgress(localPath, 0, size);

    // See the matching comment in downloadFile() above — same fix, same reason.
    if (QFile::exists(remotePath) && !QFile::remove(remotePath)) {
        emit transferFailed(localPath, QStringLiteral("Could not overwrite existing file: %1").arg(remotePath));
        return;
    }

    QFile src(localPath);
    if (!src.copy(remotePath)) {
        emit transferFailed(localPath, src.errorString());
        return;
    }
    emit transferProgress(localPath, size, size);
    emit transferFinished(localPath);
}

QString LocalBackend::currentPath() const
{
    return m_currentPath;
}

void LocalBackend::deleteEntry(const QString &path, bool isDirectory)
{
    if (isDirectory) {
        // Empty-only, matching plain POSIX rmdir / SFTP RMDIR semantics —
        // see RemoteBackend::deleteEntry()'s doc comment for why this is
        // deliberately not recursive.
        if (!QDir().rmdir(path)) {
            emit fileOperationFailed(QStringLiteral("Delete"), path,
                QStringLiteral("Could not remove folder — it may not be empty"));
            return;
        }
    } else {
        QFile file(path);
        if (!file.remove()) {
            emit fileOperationFailed(QStringLiteral("Delete"), path, file.errorString());
            return;
        }
    }
    listDirectory(m_currentPath);
}

void LocalBackend::renameEntry(const QString &oldPath, const QString &newPath)
{
    // Checked explicitly rather than relying on QDir::rename()'s own
    // failure to distinguish "name taken" from any other failure reason
    // in the error message shown to the person who triggered this.
    if (QFileInfo::exists(newPath)) {
        emit fileOperationFailed(QStringLiteral("Rename"), oldPath,
            QStringLiteral("\"%1\" already exists").arg(QFileInfo(newPath).fileName()));
        return;
    }
    // QDir::rename() (not QFile::rename()) specifically — QFile::rename()
    // isn't documented to reliably rename directories across platforms;
    // QDir::rename() is the correct portable call for both files and
    // directories. Works correctly on absolute paths regardless of which
    // QDir instance it's called on.
    if (!QDir().rename(oldPath, newPath)) {
        emit fileOperationFailed(QStringLiteral("Rename"), oldPath, QStringLiteral("Rename failed"));
        return;
    }
    listDirectory(m_currentPath);
}

void LocalBackend::moveEntry(const QString &oldPath, const QString &newPath, int requestId)
{
    // Deliberately NOT sharing renameEntry()'s body above — the two have
    // genuinely different pre-conditions, not just a different response
    // mechanism. renameEntry() REJECTS outright if newPath already
    // exists (the right behavior for the single-pane Rename context-menu
    // action, which never goes through any conflict-resolution flow).
    // moveEntry() is only ever called after TransferManager has already
    // resolved a destination conflict itself (Overwrite for a file; a
    // folder move never reaches here if something's already at newPath —
    // see TransferManager::moveFolder()'s own comment on why a merge
    // isn't attempted). A pre-existing FILE at newPath is removed first,
    // the same established convention uploadFile() already uses for an
    // Overwrite-resolved transfer, so QDir::rename() below succeeds
    // instead of failing on an existing-destination check of its own.
    const QFileInfo destInfo(newPath);
    if (destInfo.exists() && destInfo.isFile())
        QFile::remove(newPath);

    if (!QDir().rename(oldPath, newPath)) {
        emit entryMoveFailed(QStringLiteral("Move failed"), requestId);
        return;
    }
    emit entryMoved(requestId);
}

void LocalBackend::createDirectory(const QString &path)
{
    if (QFileInfo::exists(path)) {
        emit fileOperationFailed(QStringLiteral("Create folder"), path,
            QStringLiteral("Something with that name already exists"));
        return;
    }
    if (!QDir().mkdir(path)) {
        emit fileOperationFailed(QStringLiteral("Create folder"), path, QStringLiteral("Could not create folder"));
        return;
    }
    listDirectory(m_currentPath);
}

void LocalBackend::createFile(const QString &path)
{
    // QIODevice::WriteOnly alone would silently truncate an existing
    // file — checked explicitly first so "the name is already taken" is
    // reported as an error instead.
    if (QFileInfo::exists(path)) {
        emit fileOperationFailed(QStringLiteral("Create file"), path,
            QStringLiteral("Something with that name already exists"));
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        emit fileOperationFailed(QStringLiteral("Create file"), path, file.errorString());
        return;
    }
    file.close();
    listDirectory(m_currentPath);
}
