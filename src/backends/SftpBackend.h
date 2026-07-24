#pragma once

#include "RemoteBackend.h"
#include "SftpCredentials.h"
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <QAtomicInteger>

class QTcpSocket;
class HostKeyVerifier;

// SFTP backend built directly on libssh2. All calls here are BLOCKING
// (that's how libssh2's sync API works) — this object must live on a
// QThread of its own, never the GUI thread. MainWindow is responsible for
// the moveToThread() + signal/slot wiring; this class assumes it's already
// off the main thread by the time connectToHost() runs.
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
// UNVERIFIED: none of this — host-key verification or public-key auth —
// has been exercised against a real SFTP server from this environment.
// Password auth against a real server has been confirmed working
// (separately, by the person building this); these paths haven't.
class SftpBackend : public RemoteBackend {
    Q_OBJECT
public:
    SftpBackend(SftpCredentials credentials, HostKeyVerifier *hostKeyVerifier,
                QObject *parent = nullptr);
    ~SftpBackend() override;

    void connectToHost() override;
    void listDirectory(const QString &path) override;
    void downloadFile(const QString &remotePath, const QString &localPath) override;
    void uploadFile(const QString &localPath, const QString &remotePath) override;

    QString currentPath() const override;
    bool isLocalFilesystem() const override { return false; }

    // Thread-safe: just flips m_cancelRequested. Called directly from the
    // GUI thread while downloadFile()/uploadFile()'s read/write loop (on
    // the worker thread) polls the same flag each iteration — QAtomicInteger
    // handles the cross-thread visibility, no signal/slot marshaling needed
    // or possible here (see RemoteBackend::requestCancel's doc comment for
    // why a queued signal wouldn't work for this).
    void requestCancel() override;

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
};
