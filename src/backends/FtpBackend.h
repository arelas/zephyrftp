#pragma once

#include "RemoteBackend.h"
#include "FtpCredentials.h"
#include <QAtomicInteger>
#include <QByteArray>

class QSslSocket;
class QTcpSocket;
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
// verifyPeerCertificate().
//
// Data connections also attempt real TLS session-ticket reuse (RFC 4217
// permits servers to require it as an anti-hijacking measure): the
// control connection's session ticket is captured and applied to each
// data connection's QSslConfiguration before its own handshake. This is
// a best-effort resumption attempt, not a guarantee — whether it
// satisfies a genuinely strict server is unverified in this
// environment (see ARCHITECTURE.md's Known gaps entry).
//
// UNVERIFIED: none of this has been exercised against a real FTP or
// FTPS server from this environment — no live server is reachable here,
// the same limitation that applies to every SFTP-specific path in this
// project. What IS verified directly: the protocol-level parsing logic
// (multi-line reply parsing, MLSD parsing, best-effort LIST parsing)
// against realistic sample data, in isolation from any actual network
// I/O — see the dedicated test for exactly what that covers.
class FtpBackend : public RemoteBackend {
    Q_OBJECT
public:
    explicit FtpBackend(FtpCredentials credentials, CertificateVerifier *certificateVerifier = nullptr,
                         QObject *parent = nullptr);
    ~FtpBackend() override;

    QString currentPath() const override;
    bool isLocalFilesystem() const override { return false; }

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
        QTcpSocket *socket = nullptr;
        QTcpServer *server = nullptr;
        bool active = false;
    };

    void teardown();

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

    // Verifies socket's peer certificate against the TOFU cert store
    // (control connections) or against this session's already-trusted
    // fingerprint (data connections — see the class doc comment), then
    // performs the TLS handshake. Returns whether the connection is
    // now genuinely encrypted; *errorOut is set to a human-readable
    // reason on failure (preferring the actual certificate problem over
    // a generic socket error, same philosophy as ensureConnected()'s
    // existing error messages). isDataConnection selects between the
    // two trust models described above.
    bool verifyPeerCertificate(QSslSocket *socket, bool isDataConnection, QString *errorOut);

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
    // handshake via verifyPeerCertificate() if the session is
    // PROT-P-protected. Returns nullptr and sets *errorOut on any
    // failure; ownership of the returned socket transfers to the
    // caller.
    QTcpSocket *finalizeDataChannel(DataChannel *channel, QString *errorOut);

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
    QSslSocket *m_controlSocket = nullptr;
    bool m_connected = false;
    bool m_dataProtected = false;   // true once PROT P has succeeded (FTPS data-channel encryption)
    QString m_currentPath;

    // The control connection's own accepted certificate fingerprint
    // (SHA-256, hex) for this session — set once verifyPeerCertificate()
    // trusts it, compared against for every subsequent data connection
    // rather than prompting again. Empty for plain FTP or before the
    // control connection's handshake completes.
    QString m_trustedCertFingerprint;

    // Captured from the control connection right after its handshake,
    // and refreshed on QSslSocket::newSessionTicketReceived() (TLS 1.3
    // tickets commonly arrive asynchronously, after the handshake
    // completes) — applied to each data connection's QSslConfiguration
    // before its own handshake, a real attempt at the session reuse
    // RFC 4217 permits servers to require. Empty until the control
    // connection has a ticket to offer.
    QByteArray m_controlSessionTicket;

    QAtomicInteger<bool> m_cancelRequested{false};
    QAtomicInteger<bool> m_pauseRequested{false};
};
