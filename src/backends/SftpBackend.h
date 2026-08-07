#pragma once

#include "RemoteBackend.h"
#include "SftpCredentials.h"
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <QAtomicInteger>

class QTcpSocket;
class HostKeyVerifier;

// SFTP backend built directly on libssh2. Connection setup, auth, host-key
// verification, and directory listing are all BLOCKING (libssh2's default
// mode) — this object must live on a QThread of its own, never the GUI
// thread. MainWindow is responsible for the moveToThread() + signal/slot
// wiring; this class assumes it's already off the main thread by the time
// connectToHost() runs.
//
// downloadFile()/uploadFile()'s actual read/write loop is the one
// exception: it temporarily switches the session to NON-blocking mode
// (see ScopedNonBlocking in the .cpp) to pipeline multiple outstanding
// SFTP read/write requests, restoring blocking mode afterward. This is
// a real, measured-elsewhere fix for a well-documented libssh2
// performance characteristic: in blocking mode, libssh2 sends one SFTP
// packet (capped at 32KB by the protocol itself) and waits for its ACK
// before sending the next, so throughput is bounded by round-trip time
// rather than bandwidth — independently confirmed via libssh2's own
// issue tracker and the curl maintainer's writeups to cause real-world
// slowdowns in roughly the 5-10x range, which is what prompted this fix
// (reported: ~40MB/s via FileZilla/Termius/SMB on the same connection,
// ~4MB/s via this app before this change). See the .cpp for the
// implementation, modeled on libssh2's own canonical
// sftp_write_nonblock.c example rather than improvised.
//
// Connection setup uses QTcpSocket rather than raw BSD sockets — portable
// across POSIX and Windows for free, since Qt Network is already linked.
// libssh2 only needs the native socket descriptor
// (QTcpSocket::socketDescriptor()) once the TCP connection is up; it
// doesn't care how that connection was established.
//
// Host-key verification: a real trust-on-first-use (TOFU) model against a
// persistent known_hosts file, backed by libssh2's knownhost API. Unknown
// or changed host keys are NOT silently accepted or silently rejected —
// they're routed to hostKeyVerifier (a GUI-thread object) via a blocking
// cross-thread call, so a real person makes that call, same as any other
// SSH client. See HostKeyVerifier.h for how that cross-thread call works.
//
// UNVERIFIED: none of this — host-key verification, public-key auth, or
// the pipelined-transfer performance fix — has been exercised against a
// real SFTP server from this environment. Password auth against a real
// server has been confirmed working (separately, by the person building
// this); these paths haven't.
class SftpBackend : public RemoteBackend {
    Q_OBJECT
public:
    SftpBackend(SftpCredentials credentials, HostKeyVerifier *hostKeyVerifier,
                QObject *parent = nullptr);
    ~SftpBackend() override;

    void connectToHost() override;
    void listDirectory(const QString &path) override;
    void downloadFile(const QString &remotePath, const QString &localPath, qint64 resumeOffset = 0) override;
    void uploadFile(const QString &localPath, const QString &remotePath, qint64 resumeOffset = 0) override;

    void deleteEntry(const QString &path, bool isDirectory) override;
    void renameEntry(const QString &oldPath, const QString &newPath) override;
    void moveEntry(const QString &oldPath, const QString &newPath, int requestId) override;
    void createDirectory(const QString &path) override;
    void createFile(const QString &path) override;
    void listDirectoryForEnumeration(const QString &path, int requestId) override;
    void checkExists(const QString &path, int requestId) override;

    QString currentPath() const override;
    bool isLocalFilesystem() const override { return false; }

    // "sftp://" + username@host:port — deliberately excludes any path
    // (starting directory is irrelevant to "is this the same server"),
    // and the scheme prefix keeps this from ever comparing equal to an
    // FtpBackend's identity even if host/port/username happened to match,
    // since a Move needs one single rename call within one protocol —
    // there's no way to rename across an SFTP session and an FTP session
    // even if they reach the identical physical machine.
    QString connectionIdentity() const override;

    // Thread-safe: just flips m_cancelRequested. Called directly from the
    // GUI thread while downloadFile()/uploadFile()'s read/write loop (on
    // the worker thread) polls the same flag each iteration — QAtomicInteger
    // handles the cross-thread visibility, no signal/slot marshaling needed
    // or possible here (see RemoteBackend::requestCancel's doc comment for
    // why a queued signal wouldn't work for this).
    void requestCancel() override;

    // Same mechanism as requestCancel() (thread-safe flag, polled in the
    // same loop) but the loop reports back via transferPaused() with the
    // current byte count instead of transferFailed() — see
    // RemoteBackend::requestPause()'s doc comment.
    void requestPause() override;

private:
    bool ensureSession();   // lazy TCP + libssh2 handshake + host-key check + auth
    void teardown();

    // Checks the server's host key against our known_hosts store. On a
    // first-seen or changed key, blocks (via m_hostKeyVerifier) for a real
    // user decision rather than assuming either "always trust" or "always
    // reject". Returns false (and emits connectionFailed) if the key is
    // rejected, unverifiable, or no verifier is wired up to ask.
    bool verifyHostKey();

    // Marshals a confirmHostKey() call onto the GUI thread and blocks
    // until it returns. Returns false if m_hostKeyVerifier is null (fails
    // safe rather than silently trusting an unaskable host).
    bool askUserToTrustHostKey(const QString &fingerprint, bool isMismatch);

    // Shared core of renameEntry()/moveEntry() — both ultimately issue the
    // identical libssh2_sftp_rename() call (its OVERWRITE|ATOMIC|NATIVE
    // flags already handle overwriting an existing destination, so unlike
    // LocalBackend there's no separate pre-removal step needed for
    // either caller); they differ only in what happens after
    // (self-refresh + fileOperationFailed vs. requestId-correlated
    // entryMoved/entryMoveFailed), which stays in each public method.
    bool performRename(const QString &oldPath, const QString &newPath, QString *errorReason);

    SftpCredentials m_credentials;
    HostKeyVerifier *m_hostKeyVerifier = nullptr;   // GUI-thread object, not owned
    QString m_currentPath = QStringLiteral("/");

    QTcpSocket *m_socket = nullptr;
    LIBSSH2_SESSION *m_session = nullptr;
    LIBSSH2_SFTP *m_sftp = nullptr;

    // Set from the GUI thread via requestCancel(), polled from the worker
    // thread inside downloadFile()/uploadFile()'s read/write loops.
    // QAtomicInteger<bool> (not a plain bool) specifically for the
    // cross-thread memory visibility guarantee — a plain bool has no such
    // guarantee and could be cached/reordered incorrectly across threads.
    QAtomicInteger<bool> m_cancelRequested{false};

    // Same pattern, same reasoning, for requestPause(). A single loop
    // iteration checks both flags — see downloadFile()/uploadFile().
    QAtomicInteger<bool> m_pauseRequested{false};
};
