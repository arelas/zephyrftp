#pragma once

#include "RemoteBackend.h"
#include "FtpCredentials.h"
#include <QAtomicInteger>

class QSslSocket;
class QTcpSocket;

// FTP/FTPS backend, hand-rolled directly on QTcpSocket/QSslSocket rather
// than a bundled all-in-one library — mirrors SftpBackend's direct-on-
// libssh2 approach: build on well-understood building blocks, not a big
// abstraction layer that (for FTP specifically) wouldn't actually save
// the hardest part anyway — see the notes below on directory listings.
// Runs on a dedicated worker thread, same threading contract as
// SftpBackend: this object must never touch the GUI thread directly.
//
// PASSIVE MODE ONLY (not active/PORT) — see FtpCredentials.h's doc
// comment for why. There is no fallback to active mode; a server that
// somehow requires it won't work with this backend.
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
// existing plaintext connection) — see FtpCredentials.h. Data
// connections get a FRESH TLS handshake rather than reusing the control
// connection's TLS session; RFC 4217 permits servers to require session
// reuse as an anti-hijacking measure, and some strict server
// configurations may reject a data connection that doesn't reuse it.
// Not implemented — flagged as a known gap, not silently ignored.
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
    explicit FtpBackend(FtpCredentials credentials, QObject *parent = nullptr);
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

    // Sends PASV, parses the "227 ... (h1,h2,h3,h4,p1,p2)" reply, opens
    // a connection (TLS-upgraded if PROT P is active) to the given
    // host/port, and returns it — connected, but with no FTP command
    // issued yet. The caller still has to send the actual transfer or
    // listing command (RETR/STOR/LIST/MLSD) on the CONTROL connection
    // afterward, per RFC 959's ordering, then read/write the data
    // connection this returns. Returns nullptr and sets *errorOut on
    // any failure; ownership transfers to the caller.
    QTcpSocket *openPassiveDataConnection(QString *errorOut);

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
    QSslSocket *m_controlSocket = nullptr;
    bool m_connected = false;
    bool m_dataProtected = false;   // true once PROT P has succeeded (FTPS data-channel encryption)
    QString m_currentPath;

    QAtomicInteger<bool> m_cancelRequested{false};
    QAtomicInteger<bool> m_pauseRequested{false};
};
