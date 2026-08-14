#include "LocalBackend.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QStandardPaths>
#include <QFutureWatcher>
#include <QtConcurrentRun>

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

// Prepares `destPath` to be safely overwritten: if something's already
// there, moves it aside to a same-directory sibling instead of deleting
// it outright — a real bug this closes, shared by downloadFile(),
// uploadFile(), and moveEntry(): all three used to delete any existing
// destination FIRST, then attempt the actual copy/rename; if that then
// failed (disk full, permission denied, source vanishing mid-copy), the
// original destination content was already gone, permanently, with only
// a generic failure message. Moving aside instead means a subsequent
// failure can restore the original exactly as it was (see each call
// site's own rollback), rather than losing it. Same-directory sibling
// specifically so both the aside-move and any rollback stay a cheap,
// same-filesystem rename on every platform, never a cross-filesystem
// copy. Returns true if the caller can proceed — either nothing was
// there (*backupPath left empty, nothing to roll back) or the existing
// entry was safely moved aside (*backupPath set to where it went);
// false only if something exists but couldn't even be moved aside (a
// permission issue), which the caller should treat as the destination
// being genuinely blocked.
bool prepareOverwrite(const QString &destPath, QString *backupPath)
{
    backupPath->clear();
    if (!QFile::exists(destPath))
        return true;
    const QString candidate = destPath + QStringLiteral(".zephyrftp-bak");
    QFile::remove(candidate);   // leftover from an earlier failed attempt, if any — safe to discard
    if (!QFile::rename(destPath, candidate))
        return false;
    *backupPath = candidate;
    return true;
}

struct CopyOutcome {
    bool ok = false;
    QString errorString;
};

// Runs OFF the GUI thread via QtConcurrent::run() — see downloadFile()/
// uploadFile()'s own doc comment for why. The actual blocking
// QFile::copy() call plus the same prepareOverwrite()-based rollback
// both callers already relied on; backupPath is resolved by the caller
// beforehand (prepareOverwrite() is a cheap same-filesystem rename, not
// worth backgrounding on its own).
CopyOutcome performCopy(const QString &srcPath, const QString &destPath, const QString &backupPath)
{
    QFile src(srcPath);
    if (!src.copy(destPath)) {
        if (!backupPath.isEmpty())
            QFile::rename(backupPath, destPath);   // roll back — restore the original exactly as it was
        return {false, src.errorString()};
    }
    if (!backupPath.isEmpty())
        QFile::remove(backupPath);   // copy succeeded — the backup is no longer needed
    return {true, QString()};
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
    // of the same file. prepareOverwrite() moves any existing destination
    // aside rather than deleting it outright — see its own comment for
    // the real data-loss bug this fixes — matching SftpBackend's
    // overwrite behavior (LIBSSH2_FXF_TRUNC on upload; QFile::open(WriteOnly)
    // truncates on download) once the copy has actually succeeded. Kept
    // synchronous/inline (unlike the copy itself below) — a same-
    // filesystem rename is cheap, not worth backgrounding on its own.
    QString backupPath;
    if (!prepareOverwrite(localPath, &backupPath)) {
        emit transferFailed(remotePath, QStringLiteral("Could not overwrite existing file: %1").arg(localPath));
        return;
    }

    // The actual copy runs on a background thread-pool task, not inline
    // here — a real, live-reported bug: this class's own header doc
    // comment assumed "local copies are fast enough in practice that
    // this hasn't mattered," which a large file (or a large batch
    // dispatched back-to-back) proved wrong — QFile::copy() is one
    // uninterruptible OS call with no chunking/yield points of its own,
    // so it blocked the GUI thread for the copy's ENTIRE duration; the
    // window couldn't even be moved until it returned. QFutureWatcher,
    // not a bare QtConcurrent::run() callback capturing `this` directly,
    // specifically because its finished signal is an ordinary, context-
    // connected QObject signal — if this LocalBackend is destroyed (pane
    // disconnected/swapped mid-transfer) before the copy finishes, Qt
    // automatically drops the connection instead of calling back into a
    // dangling backend. Every OTHER LocalBackend operation (listing,
    // delete, rename, ...) stays exactly as fast and synchronous as
    // before — only the actual data copy, the one operation now proven
    // slow enough to matter, moves off the GUI thread.
    auto *watcher = new QFutureWatcher<CopyOutcome>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, remotePath, size]() {
        const CopyOutcome outcome = watcher->result();
        watcher->deleteLater();
        if (!outcome.ok) {
            emit transferFailed(remotePath, outcome.errorString);
            return;
        }
        emit transferProgress(remotePath, size, size);
        emit transferFinished(remotePath);
    });
    watcher->setFuture(QtConcurrent::run(performCopy, remotePath, localPath, backupPath));
}

void LocalBackend::uploadFile(const QString &localPath, const QString &remotePath, qint64 resumeOffset)
{
    Q_UNUSED(resumeOffset);
    QFileInfo info(localPath);
    const qint64 size = info.size();
    emit transferProgress(localPath, 0, size);

    // See the matching comment in downloadFile() above — same fix, same reason.
    QString backupPath;
    if (!prepareOverwrite(remotePath, &backupPath)) {
        emit transferFailed(localPath, QStringLiteral("Could not overwrite existing file: %1").arg(remotePath));
        return;
    }

    // Backgrounded the same way and for the same reason as downloadFile()
    // above.
    auto *watcher = new QFutureWatcher<CopyOutcome>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, localPath, size]() {
        const CopyOutcome outcome = watcher->result();
        watcher->deleteLater();
        if (!outcome.ok) {
            emit transferFailed(localPath, outcome.errorString);
            return;
        }
        emit transferProgress(localPath, size, size);
        emit transferFinished(localPath);
    });
    watcher->setFuture(QtConcurrent::run(performCopy, localPath, remotePath, backupPath));
}

QString LocalBackend::currentPath() const
{
    return m_currentPath;
}

void LocalBackend::deleteEntry(const QString &path, bool isDirectory)
{
    // isDirectory comes from the caller's RemoteEntry::isDir, set via
    // QFileInfo::isDir() in listDirectory()/listDirectoryForEnumeration()
    // — which FOLLOWS symlinks, so a directory symlink also reports
    // isDirectory=true here. A real bug this guard fixes, confirmed
    // directly: QDir::rmdir() (POSIX rmdir(2)) deliberately does NOT
    // follow a symlink in its final path component — it requires the
    // path to literally BE a directory, not a symlink to one — so a
    // directory symlink could never actually be deleted this way,
    // always failing with the same misleading "may not be empty"
    // message no matter how genuinely removable it (or its target) was.
    // A symlink — to a directory or otherwise — is itself always a
    // single filesystem entry, removed the same way a plain file is
    // (QFile::remove(), POSIX unlink(2) semantics, confirmed to act on
    // the symlink entry itself and leave its target directory
    // completely untouched).
    if (isDirectory && !QFileInfo(path).isSymLink()) {
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
    // QFileInfo(oldPath) != QFileInfo(newPath) — not just
    // QFileInfo::exists(newPath) alone — a real bug this closes: a
    // case-ONLY rename (e.g. "readme.txt" -> "README.txt") on a
    // case-insensitive filesystem (the default on Windows/NTFS and
    // macOS/APFS, both real release targets) makes exists(newPath) true
    // even though newPath IS oldPath, just spelled differently — the old
    // check rejected this as "already exists" when it's actually a
    // no-conflict rename. QFileInfo::operator==() is Qt's own "do these
    // refer to the same file" comparison (confirmed directly: two
    // different path strings that resolve to the same underlying file —
    // e.g. via a symlink — compare equal, not just byte-identical
    // strings), so it correctly recognizes this case and still rejects a
    // GENUINE naming conflict.
    if (QFileInfo::exists(newPath) && QFileInfo(oldPath) != QFileInfo(newPath)) {
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
    // isn't attempted). A pre-existing FILE at newPath is moved aside via
    // prepareOverwrite() rather than deleted outright — see its own
    // comment for the real data-loss bug this fixes — so QDir::rename()
    // below succeeds instead of failing on an existing-destination check
    // of its own, with a real rollback available if the move then fails.
    const QFileInfo destInfo(newPath);
    QString backupPath;
    if (destInfo.exists() && destInfo.isFile() && !prepareOverwrite(newPath, &backupPath)) {
        emit entryMoveFailed(QStringLiteral("Could not move existing \"%1\" aside to overwrite it")
                              .arg(destInfo.fileName()), requestId);
        return;
    }

    if (!QDir().rename(oldPath, newPath)) {
        if (!backupPath.isEmpty())
            QFile::rename(backupPath, newPath);   // roll back — restore the original exactly as it was
        emit entryMoveFailed(QStringLiteral("Move failed"), requestId);
        return;
    }
    if (!backupPath.isEmpty())
        QFile::remove(backupPath);   // move succeeded — the backup is no longer needed
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

void LocalBackend::setPermissions(const QString &path, int mode)
{
    // The inverse of renderPermissions() above — POSIX octal bits in,
    // QFileDevice::Permissions flags out (Qt's own enum values use
    // entirely different bit positions, not the raw 0400/0200/... POSIX
    // ones, so this can't just be a static_cast).
    QFileDevice::Permissions permissions;
    if (mode & 0400) permissions |= QFileDevice::ReadOwner;
    if (mode & 0200) permissions |= QFileDevice::WriteOwner;
    if (mode & 0100) permissions |= QFileDevice::ExeOwner;
    if (mode & 0040) permissions |= QFileDevice::ReadGroup;
    if (mode & 0020) permissions |= QFileDevice::WriteGroup;
    if (mode & 0010) permissions |= QFileDevice::ExeGroup;
    if (mode & 0004) permissions |= QFileDevice::ReadOther;
    if (mode & 0002) permissions |= QFileDevice::WriteOther;
    if (mode & 0001) permissions |= QFileDevice::ExeOther;

    if (!QFile::setPermissions(path, permissions)) {
        emit fileOperationFailed(QStringLiteral("Change permissions"), path,
            QStringLiteral("Could not change permissions"));
        return;
    }
    listDirectory(m_currentPath);
}
