#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include "RemoteEntry.h"

// Common interface for anything a FilePaneWidget can browse: the local
// filesystem, an SFTP session, or (later) plain FTP/FTPS. Each backend
// runs its blocking I/O on a worker thread and reports back via signals —
// nothing in here should ever block the GUI thread.
class RemoteBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~RemoteBackend() override = default;

    virtual QString currentPath() const = 0;

    // An opaque, comparable identity for "which underlying filesystem/
    // server this backend actually talks to" — used by TransferManager to
    // decide whether a cross-pane Move can be done via a single server-side
    // moveEntry() (rename) instead of the much slower stage-through-local-
    // disk transfer path. Two backends with equal, non-empty identities are
    // considered safe to rename directly between. LocalBackend returns a
    // fixed constant (any two local panes are trivially the same real
    // filesystem); SftpBackend/FtpBackend build one from host/port/username
    // — deliberately including protocol (via each concrete class's own
    // scheme prefix), since an SFTP session and an FTP session to the same
    // hostname can never be renamed between via one call regardless of
    // whether they're "really" the same box.
    virtual QString connectionIdentity() const = 0;

    // Lets callers (the transfer queue, mainly) pick upload vs. download
    // semantics and argument order without needing dynamic_cast or an enum
    // of backend types. True only for LocalBackend.
    virtual bool isLocalFilesystem() const = 0;

    // Asks the currently-running download/upload to stop as soon as it can
    // safely check. Deliberately NOT a slot needing queued dispatch —
    // implementations must make this safe to call directly from any thread
    // (a thread-safe flag set here, polled inside the transfer loop there),
    // since the whole point is interrupting a blocking libssh2 call that's
    // already in progress on the worker thread; a queued signal wouldn't be
    // processed until that blocking call returns on its own, which defeats
    // the purpose. No-op for backends with nothing worth interrupting
    // (LocalBackend's QFile::copy is a single un-interruptible OS call).
    virtual void requestCancel() = 0;

    // Same contract as requestCancel() (thread-safe, direct-callable, not
    // a queued slot) but stops the transfer WITHOUT discarding progress —
    // the backend reports back via transferPaused(fileName, bytesDone)
    // rather than transferFailed, and that bytesDone becomes the resume
    // offset for a later uploadFile()/downloadFile() call. No-op for
    // LocalBackend, same reasoning as requestCancel(): QFile::copy() is
    // atomic, there's no partial-progress state to preserve or resume
    // from, so pause on a local transfer would be indistinguishable from
    // cancel — the UI simply doesn't offer Pause for local-to-local
    // transfers rather than pretending to support it (see
    // TransferQueueWidget's context menu).
    virtual void requestPause() = 0;

    // Declared as slots (not just virtual methods) so QMetaObject::invokeMethod
    // can reach them by name via a queued connection when the backend lives on
    // a worker thread. Qt's "virtual slot" pattern: the base class's moc-generated
    // thunk dispatches through the vtable, so the derived override still runs —
    // callers never need to know which concrete backend they're invoking.
public slots:
    virtual void connectToHost() = 0;   // no-op for LocalBackend
    virtual void listDirectory(const QString &path) = 0;

    // resumeOffset > 0 means "continue a previously paused transfer from
    // this byte offset" rather than starting fresh — SftpBackend seeks
    // both sides to it and skips truncating the destination; LocalBackend
    // ignores it (see requestPause()'s doc comment for why local
    // transfers never produce a nonzero offset to resume from in the
    // first place). Always pass all three arguments explicitly through
    // QMetaObject::invokeMethod — default arguments don't apply across
    // that kind of runtime dispatch.
    virtual void downloadFile(const QString &remotePath, const QString &localPath, qint64 resumeOffset = 0) = 0;
    virtual void uploadFile(const QString &localPath, const QString &remotePath, qint64 resumeOffset = 0) = 0;

    // File management: delete/rename/create, both local and remote. All
    // four are "fire and refresh" — on success, the implementation
    // re-lists the current directory itself (reusing the existing
    // directoryListed signal FilePaneWidget already handles) rather than
    // needing a separate success signal; on failure, fileOperationFailed
    // below carries the reason. isDirectory on deleteEntry() matters
    // because the two operations are genuinely different at the protocol
    // level (unlink vs. rmdir for SFTP, QFile::remove vs. QDir::rmdir
    // locally) — there's no single "delete anything" call to fall back
    // to. Directory deletion is deliberately NOT recursive: both
    // backends only remove an empty directory (matching plain POSIX
    // rmdir / SFTP RMDIR semantics), and a non-empty one fails with a
    // clear reason rather than either silently doing nothing or wiping
    // out a whole tree — recursive delete is a meaningfully bigger, more
    // dangerous feature that wasn't asked for.
    virtual void deleteEntry(const QString &path, bool isDirectory) = 0;
    virtual void renameEntry(const QString &oldPath, const QString &newPath) = 0;

    // A server-side move (rename) between two DIFFERENT panes' directories
    // — deliberately NOT the same slot as renameEntry() above, even though
    // both ultimately issue the identical underlying rename call. Two real
    // differences TransferManager needs that renameEntry()'s "fire and
    // refresh its own directory, implicit success" contract doesn't give:
    // an explicit, request-id-correlated success signal (entryMoved) to
    // drive a TransferItem's status off of — renameEntry() has no success
    // signal at all, only the ABSENCE of fileOperationFailed, too indirect
    // to build UI state on; and no self-triggered directory refresh, since
    // a cross-pane move needs BOTH panes refreshed (TransferManager does
    // this itself via the existing transferSucceeded signal once this
    // succeeds), not just the one directory renameEntry() already knows
    // about. Only ever called when the source and destination backends'
    // connectionIdentity() already compared equal — implementations don't
    // need to re-check that themselves.
    virtual void moveEntry(const QString &oldPath, const QString &newPath, int requestId) = 0;

    virtual void createDirectory(const QString &path) = 0;
    // Creates an empty (zero-byte) file. Fails if a file already exists
    // at that path rather than silently truncating it — both backends
    // treat "the name is already taken" as an error to report, not
    // something to overwrite quietly.
    virtual void createFile(const QString &path) = 0;

    // Same "fire and refresh" contract as deleteEntry()/renameEntry()/
    // createDirectory()/createFile() above — on success, re-lists the
    // current directory itself; on failure, fileOperationFailed below
    // carries the reason (label "Change permissions", same convention
    // "Delete"/"Rename" already use). mode is the raw POSIX permission
    // bits (e.g. 0644, 0755) — NOT a QFileDevice::Permissions bitmask,
    // whose enum values use entirely different bit positions; each
    // implementation converts internally. Deliberately just the
    // standard 9 rwxrwxrwx bits — no setuid/setgid/sticky, a rare,
    // advanced, high-consequence-if-fat-fingered tier every mainstream
    // GUI chmod dialog also keeps separate. FtpBackend's implementation
    // uses SITE CHMOD, a widely-supported (vsftpd, proftpd) but
    // non-standard extension — a server that doesn't support it
    // surfaces as an ordinary fileOperationFailed, not a special case.
    virtual void setPermissions(const QString &path, int mode) = 0;

    // Lists a directory's contents WITHOUT updating currentPath() or
    // being treated as pane navigation — listDirectory() has that side
    // effect deliberately (it's what drives FilePaneWidget's own display
    // and history), which is exactly wrong for recursive folder-transfer
    // enumeration: walking into a dozen subdirectories to build a
    // manifest would otherwise hijack the pane's visible listing and
    // corrupt its navigation history mid-walk. requestId is echoed back
    // in directoryEnumerated() so a caller managing several outstanding
    // requests (see FolderEnumerator) can match responses to requests.
    virtual void listDirectoryForEnumeration(const QString &path, int requestId) = 0;

    // Checks whether something already exists at path, without listing a
    // whole directory — used by TransferManager to detect a destination
    // conflict (a file or folder already there) before actually starting
    // a transfer or creating a directory, so it can ask Overwrite/Skip
    // (files) or Write Into/Skip (folders) instead of silently
    // overwriting, which was the previous behavior. requestId is echoed
    // back in existsChecked(), same reason as listDirectoryForEnumeration()'s.
    virtual void checkExists(const QString &path, int requestId) = 0;

signals:
    void connected();
    void connectionFailed(const QString &reason);

    // One line of human-readable protocol/operation traffic, for the
    // Commands pane's live log (read-only, modeled on FileZilla's message
    // log — see MainWindow's doc comment on m_commandsDock). FtpBackend
    // emits genuine raw command/reply lines straight off the control
    // connection (with PASS's argument masked — see its own emission site
    // for why); SftpBackend has no textual wire protocol to show, so it
    // emits a human-readable description of each high-level operation
    // instead (matching FileZilla's own "Command: get ..." style for
    // SFTP). LocalBackend never emits this at all — nothing protocol-like
    // happens for a purely local pane.
    void commandLogged(const QString &line);
    void directoryListed(const QString &path, const QList<RemoteEntry> &entries);
    void transferProgress(const QString &fileName, qint64 bytesDone, qint64 bytesTotal);
    void transferFinished(const QString &fileName);
    void transferFailed(const QString &fileName, const QString &reason);
    // Emitted instead of transferFailed when requestPause() actually
    // catches the transfer mid-flight. bytesDone at the moment of pause
    // becomes the resume offset for the next uploadFile()/downloadFile()
    // call on this same file.
    void transferPaused(const QString &fileName, qint64 bytesDone);

    // Failure path for deleteEntry()/renameEntry()/createDirectory()/
    // createFile() — operation is a short human-readable label ("Delete",
    // "Rename", "Create folder", "Create file"), not a machine-parsed
    // code, since the only consumer is a message shown directly to the
    // person who triggered it.
    void fileOperationFailed(const QString &operation, const QString &path, const QString &reason);

    // Response to listDirectoryForEnumeration() — same entry data as
    // directoryListed(), but routed separately so FilePaneWidget's own
    // listener (which drives the visible pane) never sees these.
    void directoryEnumerated(const QString &path, const QList<RemoteEntry> &entries, int requestId);
    // Enumeration hit a real error (e.g. permission denied partway
    // through a walk) — carries requestId for the same reason as
    // directoryEnumerated(), and path identifies which directory failed,
    // since a recursive walk has many outstanding/attempted paths.
    void enumerationFailed(const QString &path, const QString &reason, int requestId);

    // Response to checkExists(). isDir is meaningless when exists is
    // false — nothing to have a type.
    void existsChecked(const QString &path, bool exists, bool isDir, int requestId);

    // Response to moveEntry() — requestId-correlated the same way
    // existsChecked()/directoryEnumerated() already are. entryMoveFailed's
    // reason is a human-readable string, same convention
    // fileOperationFailed() already uses.
    void entryMoved(int requestId);
    void entryMoveFailed(const QString &reason, int requestId);
};
