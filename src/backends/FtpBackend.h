#pragma once

#include "RemoteBackend.h"
#include "FtpCredentials.h"
#include "FtpTlsSocket.h"
#include <QAtomicInteger>
#include <QByteArray>
#include <QMutex>

class QTcpServer;
class CertificateVerifier;

// FTP/FTPS backend, hand-rolled directly on QTcpSocket/QSslSocket rather
// than a bundled all-in-one library — mirrors SftpBackend's direct-on-
// libssh2 approach: build on well-understood building blocks, not a big
// abstraction layer that (for FTP specifically) wouldn't actually save
// the hardest part anyway — see the notes below on directory listings.
// Runs on a dedicated worker thread, same threading contract as
// SftpBackend: this object must never touch the GUI thread directly.
//
// DATA CONNECTIONS: PASV (passive mode) is always tried first — the
// NAT/firewall-friendly default essentially every modern FTP client
// uses. If the server outright REFUSES the PASV command (a real, if
// rare, sign it requires active mode), this falls back to active/PORT:
// a local QTcpServer is opened and the server is told to connect back
// to it. This is a fallback only, not a user-facing mode toggle —
// PASV's NAT-friendly behavior is unchanged for every server that
// accepts it. See openDataChannel()/finalizeDataChannel().
//
// DIRECTORY LISTINGS: tries MLSD (RFC 3659, a standardized,
// machine-parseable format) first; if the server doesn't support it
// (a 500/502 reply), falls back to guessing at LIST's output format,
// which is NOT standardized across server implementations — this is
// the single biggest real-world fragility source for any FTP client,
// hand-rolled or not (confirmed directly against libcurl's own
// documentation before choosing this architecture at all: libcurl
// doesn't parse LIST output either, callers still have to). See
// parseListLine()'s own comment in the .cpp for exactly what's covered
// and what isn't.
//
// FTPS: explicit mode only (AUTH TLS on the normal port, upgrading an
// existing plaintext connection) — see FtpCredentials.h. Certificate
// verification is a real trust-on-first-use (TOFU) model, the same
// shape as SftpBackend's host-key TOFU: an unverifiable certificate
// (self-signed, unknown CA, ...) is NOT silently accepted or silently
// rejected — it's routed to certificateVerifier (a GUI-thread object)
// via a blocking cross-thread call, so a real person makes that call.
// The accepted certificate's SHA-256 fingerprint is persisted
// (AppConfigLocation/known_certs.json) and re-checked on every future
// connection to the same host:port; a certificate that later CHANGES is
// a strong-warning prompt (defaults to no), same as a changed SSH host
// key. Data connections reuse the control connection's already-trusted
// fingerprint rather than prompting again — a data connection
// presenting a DIFFERENT certificate than the one just trusted is
// treated as suspicious and fails closed with no prompt. See
// verifyTlsPeer().
//
// FTPS's control AND data connections are handled by FtpTlsSocket (raw
// OpenSSL, forced to exactly TLS 1.2), NOT QSslSocket — see
// FtpTlsSocket.h's own doc comment for why: a genuinely strict server's
// RFC 4217 anti-hijacking check (data connections must demonstrably
// reuse the control connection's TLS session) is NOT satisfied by
// anything QSslSocket's public API can drive, confirmed directly
// against a real proftpd container. Plain FTP is entirely unaffected —
// its control and data connections still use QSslSocket purely as TCP,
// via QtSocketAdapter (see FtpSocket.h), exactly as before FtpTlsSocket
// existed.
//
// FTPS (and plain FTP) against real vendor servers (vsftpd, proftpd) is
// exercised end to end by verify-ftp-vendors/verify-ftps-trust against
// real local containers (tools/local-test-servers/) — see
// ARCHITECTURE.md's Known Gaps entry for the full story, including the
// one real, permanent, server-side limitation found there (vsftpd's
// require_ssl_reuse=YES deadlocking under this specific container
// environment, unrelated to this class's own TLS session handling).
class FtpBackend : public RemoteBackend {
    Q_OBJECT
public:
    explicit FtpBackend(FtpCredentials credentials, CertificateVerifier *certificateVerifier = nullptr,
                         QObject *parent = nullptr);
    ~FtpBackend() override;

    QString currentPath() const override;
    bool isLocalFilesystem() const override { return false; }

    // "ftp://" + username@host:port — same reasoning as SftpBackend's
    // connectionIdentity(): no path component, and the scheme prefix keeps
    // this from ever comparing equal to an SftpBackend's identity even for
    // a matching host/port/username. Deliberately does NOT distinguish
    // plain FTP from FTPS (FtpsMode) — RNFR/RNTO are control-connection
    // commands unaffected by whether that connection happens to be
    // TLS-wrapped, so encryption status has no bearing on whether a
    // server-side rename between two sessions would work.
    QString connectionIdentity() const override;

    // Same thread-safe-flag contract as SftpBackend's identically-named
    // methods — see RemoteBackend's doc comments on requestCancel()/
    // requestPause() for the full reasoning. Polled inside the
    // download/upload data-connection read/write loop.
    void requestCancel() override;
    void requestPause() override;

    // Parsers for the two directory-listing formats — public
    // specifically so they can be tested directly and precisely. This
    // sandbox has no reachable FTP server, so a real MLSD/LIST response
    // can never be exercised through the full
    // connectToHost()->listDirectory() stack here; testing the pure
    // parsing logic against realistic sample lines is the only way to
    // verify the single highest-risk part of this whole feature in this
    // environment. Same reasoning FilePaneWidget::parentOfPath() was
    // made public for.
    static bool parseMlsdLine(const QString &line, RemoteEntry *entry);
    static bool parseListLine(const QString &line, RemoteEntry *entry);

public slots:
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

private:
    struct FtpReply {
        int code = 0;
        QString text;   // full reply text; multi-line replies joined with '\n'
        bool isValid() const { return code > 0; }
    };

    // Holds either a PASV data connection (already connected — socket
    // set, server null) or an active/PORT one (listening, awaiting the
    // server's connect-back — server set, socket null) between
    // openDataChannel() and finalizeDataChannel(). See those methods'
    // doc comments for why the two modes need genuinely different
    // sequencing relative to the RETR/STOR/LIST/MLSD command.
    struct DataChannel {
        FtpSocket *socket = nullptr;
        QTcpServer *server = nullptr;
        bool active = false;
    };

    void teardown();

    // Shared core of renameEntry()/moveEntry() — both issue the identical
    // RNFR/RNTO command pair; they differ only in what happens after
    // (self-refresh + fileOperationFailed vs. requestId-correlated
    // entryMoved/entryMoveFailed), which stays in each public method.
    bool performRename(const QString &oldPath, const QString &newPath, QString *errorReason);

    // Establishes the control connection if not already up: TCP
    // connect, (if FTPS) AUTH TLS + PBSZ 0 + PROT P, USER/PASS login,
    // TYPE I (binary mode, always — this app has no use for FTP's
    // ASCII/text transfer mode, which would corrupt anything that isn't
    // plain text). Safe to call repeatedly; a no-op if already
    // connected. Returns false and emits connectionFailed() on any
    // failure, leaving the connection torn down rather than in a
    // half-initialized state.
    bool ensureConnected();

    // Sends "text\r\n" on the control connection, then reads and returns
    // the reply. Blocking — this backend runs on its own worker thread,
    // same contract as every blocking call in SftpBackend.
    FtpReply sendCommand(const QString &text);

    // Reads a (possibly multi-line) reply without sending anything
    // first — used right after the TCP connect (for the welcome
    // banner) and right after a TLS handshake completes.
    FtpReply readReply();

    // Applies the TOFU trust decision (control connections: against the
    // known_certs.json store; data connections: must match this
    // session's already-trusted fingerprint, no prompt — see the class
    // doc comment) to an already-completed FtpTlsSocket::handshake()'s
    // PeerInfo. Returns whether the certificate is trusted; *errorOut is
    // set to a human-readable reason on failure. Called after the raw
    // TLS handshake itself already succeeded (FtpTlsSocket always
    // completes the handshake regardless of certificate trust — see
    // FtpTlsSocket.h) — a false return here is what actually fails the
    // connection closed.
    bool verifyTlsPeer(const FtpTlsSocket::PeerInfo &info, bool isDataConnection, QString *errorOut);

    // Marshals a confirmCertificate() call onto the GUI thread and
    // blocks until it returns. Returns false if m_certificateVerifier is
    // null (fails safe rather than silently trusting an unaskable
    // certificate) — same contract as SftpBackend::askUserToTrustHostKey().
    bool askUserToTrustCertificate(const QString &fingerprint, const QString &details, bool isMismatch);

    // Opens a data channel for the next listing/transfer command: tries
    // PASV first (existing behavior, unchanged for any server that
    // accepts it); if the PASV command itself is refused, falls back to
    // active/PORT (see the class doc comment). Does NOT perform the
    // TLS handshake — for PASV the connection is already open at this
    // point, but for active/PORT the server hasn't connected back yet
    // (it only does so once it processes the RETR/STOR/LIST/MLSD
    // command, which per RFC 959 must be sent on the control connection
    // AFTER this returns) — so the handshake has to wait for
    // finalizeDataChannel(). Returns false and sets *errorOut on any
    // failure; ownership of whatever channel resources were opened
    // transfers to the caller via *channel.
    bool openDataChannel(DataChannel *channel, QString *errorOut);

    // PASV-refusal fallback for openDataChannel(): opens a local
    // QTcpServer bound to the control connection's own local address,
    // sends PORT with its ephemeral port, and returns with *channel set
    // to the listening server (not yet connected).
    bool openActiveDataChannel(DataChannel *channel, QString *errorOut);

    // Completes a data channel opened by openDataChannel(), called
    // AFTER the caller has sent the actual RETR/STOR/LIST/MLSD command
    // on the control connection: for PASV, just returns the
    // already-connected socket; for active/PORT, blocks
    // (waitForNewConnection()) until the server connects back, then
    // adopts that connection. Either way, then performs the TLS
    // handshake (via FtpTlsSocket::handshake() + verifyTlsPeer()) if
    // the session is PROT-P-protected. Returns nullptr and sets
    // *errorOut on any failure; ownership of the returned socket
    // transfers to the caller.
    FtpSocket *finalizeDataChannel(DataChannel *channel, QString *errorOut);

    // Lists a directory via MLSD (tried first) or LIST (fallback),
    // returning parsed entries. Shared by listDirectory(),
    // listDirectoryForEnumeration(), and checkExists() (which lists the
    // parent and checks membership there, rather than needing a
    // separate existence-check command FTP doesn't cleanly provide for
    // both files and directories uniformly). *ok set to false only on a
    // real failure (connection/protocol problem) — an empty directory
    // is not an error and produces an empty, successful list.
    QList<RemoteEntry> listDirectoryInternal(const QString &path, bool *ok, QString *errorOut);

    FtpCredentials m_credentials;
    CertificateVerifier *m_certificateVerifier = nullptr;   // GUI-thread object, not owned
    FtpSocket *m_controlSocket = nullptr;
    bool m_connected = false;
    bool m_dataProtected = false;   // true once PROT P has succeeded (FTPS data-channel encryption)

    // Same real bug, same fix as SftpBackend's identical member: read
    // cross-thread by currentPath() (called from the GUI thread via
    // FilePaneWidget::currentDirectory()) while written from this
    // backend's own worker thread inside listDirectory() — see
    // SftpBackend.h's m_currentPathMutex doc comment for the full
    // reasoning. Same-thread reads elsewhere in this file (e.g.
    // listDirectory()'s own `path.isEmpty() ? m_currentPath : path`)
    // don't need it, only currentPath()'s cross-thread read does.
    mutable QMutex m_currentPathMutex;
    QString m_currentPath;

    // The control connection's own accepted certificate fingerprint
    // (SHA-256, hex) for this session — set once verifyTlsPeer() trusts
    // it, compared against for every subsequent data connection rather
    // than prompting again. Empty for plain FTP or before the control
    // connection's handshake completes.
    QString m_trustedCertFingerprint;

    QAtomicInteger<bool> m_cancelRequested{false};
    QAtomicInteger<bool> m_pauseRequested{false};
};
