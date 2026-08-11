#pragma once

#include "FtpSocket.h"

class QTcpSocket;
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_session_st SSL_SESSION;

// FTPS's real answer to a genuinely strict server's TLS-session-reuse
// requirement (RFC 4217's anti-hijacking check some servers, e.g.
// proftpd's mod_tls by default, enforce on the data connection) — see
// FtpBackend.h's class comment and ARCHITECTURE.md's own entry for the
// full story of why QSslSocket can't do this: its public API only
// exposes TLS 1.3's post-handshake ticket mechanism
// (newSessionTicketReceived()/QSslConfiguration::sessionTicket()),
// which a real proftpd container's own TLSLog confirms does NOT read
// as genuine reuse to mod_tls's check — and forcing TLS 1.2 through
// QSslSocket's own API doesn't help either, since that signal simply
// never fires under TLS 1.2, leaving nothing to even attempt reusing.
// A real, working fix — confirmed via a disposable raw-OpenSSL
// probe against that same container, both a positive control (session
// genuinely reused, real data received) and a negative one (the exact
// documented rejection) — needs actual OpenSSL SSL_get1_session()/
// SSL_set_session() calls, which have no QSslSocket equivalent at all.
// This class is the production version of that probe: forces TLS 1.2
// (the version the probe confirmed proftpd's reuse check actually
// recognizes), captures a reusable SSL_SESSION after its own
// handshake, and accepts one to attempt resumption with.
//
// Used only for FTPS (FtpsMode::Explicit) control and data
// connections — plain FTP is untouched, still QSslSocket-as-TCP via
// QtSocketAdapter exactly as before (see FtpBackend.cpp).
//
// Blocking I/O with timeouts, same contract as every other blocking
// call in this backend (it runs on its own worker thread) — internally
// implemented via a poll()/WSAPoll() retry loop directly on the raw
// socket descriptor, the same shape SftpBackend.cpp's
// waitForSocketReady() already uses for libssh2's non-blocking I/O
// (there via libssh2_poll(), which isn't available here since this
// class has no libssh2 involvement at all).
//
// TLS is upgraded IN PLACE on an already-connected plaintext socket,
// same as QSslSocket::startClientEncryption() — the control
// connection's plaintext banner and "AUTH TLS" exchange happen before
// handshake() is ever called, so every I/O method here has to work
// correctly both before and after the handshake (see the .cpp).
class FtpTlsSocket : public FtpSocket {
public:
    FtpTlsSocket();
    ~FtpTlsSocket() override;

    // Everything FtpBackend::verifyTlsPeer() needs to reproduce
    // verifyPeerCertificate()'s existing TOFU trust logic, sourced
    // from OpenSSL's C API instead of QSslCertificate/QSslError.
    struct PeerInfo {
        QString fingerprintSha256Hex;   // lowercase hex — same format QCryptographicHash::Sha256(...).toHex() produces elsewhere in this file
        QString subject;
        QString issuer;
        bool systemTrusted = false;     // SSL_get_verify_result() == X509_V_OK
        QString verifyError;            // human-readable reason, set when !systemTrusted
    };

    // Fresh outbound TCP connect — control connections, and PASV data
    // connections (mirrors QSslSocket::connectToHost()+
    // waitForConnected() in the existing plain-FTP code path). Does
    // NOT perform the TLS handshake yet — see handshake().
    bool connectToHost(const QString &host, quint16 port, QString *errorOut);

    // Adopts an already-accepted TCP connection (active/PORT data
    // connections) — takes ownership of qtSocket, which must already
    // hold a connected native descriptor (see SslAcceptingTcpServer).
    void adopt(QTcpSocket *qtSocket);

    // Upgrades the connection to TLS, forced to exactly TLS 1.2 (see
    // this class's own doc comment for why). sessionToReuse: non-null
    // to attempt resumption (data connections, passed the control
    // connection's own sessionForReuse()); null for a fresh handshake
    // (the control connection's own first handshake). On success,
    // *peerInfoOut describes the peer certificate for the caller's
    // trust decision — the handshake itself always completes
    // regardless of what OpenSSL's own chain validation thinks
    // (SSL_VERIFY_NONE): this project's real trust model is TOFU
    // fingerprint pinning (FtpBackend::verifyTlsPeer()), not CA-chain
    // validation, so OpenSSL's built-in verify callback isn't the
    // right place to fail closed — same "capture the problem, let the
    // caller decide" shape the existing QSslSocket::sslErrors()-based
    // code already uses for plain FTP's own TLS path.
    bool handshake(SSL_SESSION *sessionToReuse, PeerInfo *peerInfoOut, QString *errorOut);

    // Valid only after a successful handshake(); the returned pointer
    // is owned by this object (freed in the destructor) — callers pass
    // it to another FtpTlsSocket's handshake() as sessionToReuse, which
    // internally up-refs it (SSL_set_session()), so it's safe for the
    // pointee to keep living here for as long as the control
    // connection itself does.
    SSL_SESSION *sessionForReuse() const { return m_session; }

    // Replaces the session offered to the NEXT handshake() call that
    // passes this as sessionToReuse — up-refs session (OpenSSL's own
    // refcounting; safe for the caller to keep using/freeing its own
    // reference independently). Needed because a strict server can
    // rotate the session/ticket on each successful resumption (a
    // standard TLS anti-replay measure): reusing the CONTROL
    // connection's original, now-superseded session for a SECOND data
    // connection is rejected even though the first data connection's
    // reuse of that same original session just succeeded — confirmed
    // directly against a real proftpd container's TLSLog. The control
    // connection's own FtpTlsSocket calls this with each data
    // connection's freshly negotiated session right after a successful
    // handshake (see FtpBackend::finalizeDataChannel()), so the NEXT
    // data connection always offers the most recently negotiated
    // session, not the original one.
    void setSessionForReuse(SSL_SESSION *session);

    bool canReadLine() const override;
    QByteArray readLine() override;
    bool waitForReadyRead(int timeoutMs) override;
    qint64 bytesAvailable() const override;
    qint64 read(char *buf, qint64 maxSize) override;
    QByteArray readAll() override;
    qint64 write(const char *data, qint64 len) override;
    bool waitForBytesWritten(int timeoutMs) override;

    bool isConnected() const override;
    QString errorString() const override;
    QHostAddress localAddress() const override;
    void setNoDelay() override;

    void disconnectGracefully() override;
    void closeAbruptly() override;
    bool waitForFullyDisconnected(int timeoutMs) override;

private:
    qintptr fd() const;
    // Blocks (up to timeoutMs) until the raw fd is ready for the given
    // direction, or returns false on timeout/error.
    bool pollFd(bool forWrite, int timeoutMs) const;
    // Polls in whichever direction an SSL_ERROR_WANT_READ/WANT_WRITE
    // result asked for — OpenSSL can legitimately ask to poll for
    // writability during what's logically a read (and vice versa,
    // during renegotiation-style exchanges), so the retry loops below
    // always defer to what SSL_get_error() actually says rather than
    // assuming "reading polls for read".
    bool pollForSslError(int sslError, int timeoutMs) const;
    // Pulls whatever's currently available (pre- or post-TLS) into
    // m_pending without blocking — used by canReadLine()/read()/
    // bytesAvailable()/waitForReadyRead() so they can work against a
    // plain buffer despite TLS records not lining up with FTP's
    // line-oriented control protocol. A no-op, not an error, if
    // nothing is available right now.
    void pump();

    QTcpSocket *m_qtSocket = nullptr;
    SSL_CTX *m_ctx = nullptr;
    SSL *m_ssl = nullptr;   // null until handshake() succeeds — see the class comment on pre-TLS I/O
    SSL_SESSION *m_session = nullptr;
    QByteArray m_pending;   // plaintext already read off the wire, not yet consumed by the caller
    QString m_lastError;
    bool m_eof = false;     // set once a clean or abrupt close is actually observed — see isConnected()'s own comment on why this isn't QAbstractSocket::state()
};
