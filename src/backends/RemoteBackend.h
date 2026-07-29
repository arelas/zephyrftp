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

signals:
    void connected();
    void connectionFailed(const QString &reason);
    void directoryListed(const QString &path, const QList<RemoteEntry> &entries);
    void transferProgress(const QString &fileName, qint64 bytesDone, qint64 bytesTotal);
    void transferFinished(const QString &fileName);
    void transferFailed(const QString &fileName, const QString &reason);
    // Emitted instead of transferFailed when requestPause() actually
    // catches the transfer mid-flight. bytesDone at the moment of pause
    // becomes the resume offset for the next uploadFile()/downloadFile()
    // call on this same file.
    void transferPaused(const QString &fileName, qint64 bytesDone);
};
