#include "FtpTlsSocket.h"

#include <QTcpSocket>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#ifdef Q_OS_WIN
#include <winsock2.h>
#else
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#endif

FtpTlsSocket::FtpTlsSocket() = default;

FtpTlsSocket::~FtpTlsSocket()
{
    if (m_session)
        SSL_SESSION_free(m_session);
    if (m_ssl)
        SSL_free(m_ssl);
    if (m_ctx)
        SSL_CTX_free(m_ctx);
    delete m_qtSocket;
}

bool FtpTlsSocket::connectToHost(const QString &host, quint16 port, QString *errorOut)
{
    m_qtSocket = new QTcpSocket();
    m_qtSocket->connectToHost(host, port);
    if (!m_qtSocket->waitForConnected(10000)) {
        if (errorOut) *errorOut = m_qtSocket->errorString();
        delete m_qtSocket;
        m_qtSocket = nullptr;
        return false;
    }
    m_qtSocket->setSocketOption(QAbstractSocket::LowDelayOption, QVariant(1));
    return true;
}

void FtpTlsSocket::adopt(QTcpSocket *qtSocket)
{
    m_qtSocket = qtSocket;
    m_qtSocket->setSocketOption(QAbstractSocket::LowDelayOption, QVariant(1));
}

qintptr FtpTlsSocket::fd() const
{
    return m_qtSocket ? m_qtSocket->socketDescriptor() : -1;
}

bool FtpTlsSocket::pollFd(bool forWrite, int timeoutMs) const
{
    const qintptr descriptor = fd();
    if (descriptor < 0)
        return false;
#ifdef Q_OS_WIN
    WSAPOLLFD pfd;
    pfd.fd = static_cast<SOCKET>(descriptor);
    pfd.events = forWrite ? POLLOUT : POLLIN;
    pfd.revents = 0;
    const int rc = WSAPoll(&pfd, 1, timeoutMs);
    return rc > 0 && (pfd.revents & (forWrite ? POLLOUT : POLLIN)) != 0;
#else
    struct pollfd pfd;
    pfd.fd = static_cast<int>(descriptor);
    pfd.events = forWrite ? POLLOUT : POLLIN;
    pfd.revents = 0;
    const int rc = ::poll(&pfd, 1, timeoutMs);
    return rc > 0 && (pfd.revents & (forWrite ? POLLOUT : POLLIN)) != 0;
#endif
}

bool FtpTlsSocket::pollForSslError(int sslError, int timeoutMs) const
{
    if (sslError == SSL_ERROR_WANT_READ)
        return pollFd(false, timeoutMs);
    if (sslError == SSL_ERROR_WANT_WRITE)
        return pollFd(true, timeoutMs);
    return false;
}

bool FtpTlsSocket::handshake(SSL_SESSION *sessionToReuse, PeerInfo *peerInfoOut, QString *errorOut)
{
    if (!m_qtSocket) {
        if (errorOut) *errorOut = QStringLiteral("Not connected");
        return false;
    }

    m_ctx = SSL_CTX_new(TLS_client_method());
    if (!m_ctx) {
        if (errorOut) *errorOut = QStringLiteral("SSL_CTX_new failed");
        return false;
    }
    // Forced to exactly TLS 1.2 — confirmed via a real proftpd
    // container (see this class's own doc comment and ARCHITECTURE.md)
    // that this is what makes classic session-ID/ticket resumption
    // possible at all; TLS 1.3's own resumption mechanism doesn't read
    // as reuse to mod_tls's check.
    SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(m_ctx, TLS1_2_VERSION);
    // Loads the OS's default CA trust store (respecting the standard
    // SSL_CERT_FILE/SSL_CERT_DIR env var overrides OpenSSL itself
    // defines) — without this there are no trust anchors loaded at
    // all, so SSL_get_verify_result() below would report a failure for
    // EVERY certificate, even a genuinely public CA-signed one, forcing
    // a TOFU prompt every single time instead of only for genuinely
    // unverifiable certificates. This is what lets verifyTlsPeer()'s
    // systemTrusted branch actually mean something, matching
    // QSslSocket's own default (system trust store) behavior from
    // before this class existed.
    SSL_CTX_set_default_verify_paths(m_ctx);
    // Accept whatever the handshake negotiates at the TLS layer
    // itself — this project's real trust model is TOFU fingerprint
    // pinning (FtpBackend::verifyTlsPeer()), not CA-chain validation,
    // so OpenSSL's own go/no-go verify callback isn't the right place
    // to fail closed. SSL_get_verify_result() below still reports what
    // OpenSSL's internal check found either way.
    SSL_CTX_set_verify(m_ctx, SSL_VERIFY_NONE, nullptr);

    m_ssl = SSL_new(m_ctx);
    if (!m_ssl) {
        if (errorOut) *errorOut = QStringLiteral("SSL_new failed");
        return false;
    }
    SSL_set_fd(m_ssl, static_cast<int>(fd()));
    if (sessionToReuse) {
        // A DEEP DUPLICATE (DER serialize/deserialize), not
        // sessionToReuse directly and not just an up-ref — confirmed
        // directly, via a raw non-Qt probe against a real proftpd
        // container, to be load-bearing: handing the SAME SSL_SESSION*
        // object (even up-ref'd) to SSL_set_session()/SSL_connect() a
        // SECOND time makes the resumption silently fail
        // (SSL_session_reused() == 0) even though the exact same
        // session content, freshly parsed into an independent object,
        // resumes successfully every time. This points at OpenSSL's
        // client-side SSL_SESSION object picking up some internal
        // "already used in a handshake" state on first use — a real,
        // observed behavior, not one documented anywhere obvious, so
        // this comment IS the record of it. Without this, only the
        // FIRST data connection to ever reuse a given control
        // connection's session would succeed; every one after it would
        // silently fall back to a full (unreused) handshake and get
        // rejected by a strict server.
        unsigned char *der = nullptr;
        const int derLen = i2d_SSL_SESSION(sessionToReuse, &der);
        if (derLen > 0) {
            const unsigned char *derPtr = der;
            SSL_SESSION *duplicate = d2i_SSL_SESSION(nullptr, &derPtr, derLen);
            if (duplicate) {
                SSL_set_session(m_ssl, duplicate);   // up-refs internally
                SSL_SESSION_free(duplicate);          // drop our own temporary ref
            }
        }
        OPENSSL_free(der);
    }

    for (;;) {
        const int ret = SSL_connect(m_ssl);
        if (ret == 1)
            break;
        const int sslError = SSL_get_error(m_ssl, ret);
        if (pollForSslError(sslError, 15000))
            continue;
        if (errorOut) {
            *errorOut = QStringLiteral("TLS handshake failed (OpenSSL error %1)").arg(sslError);
        }
        return false;
    }

    X509 *peerCert = SSL_get1_peer_certificate(m_ssl);
    if (!peerCert) {
        if (errorOut) *errorOut = QStringLiteral("Server presented no certificate");
        return false;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    X509_digest(peerCert, EVP_sha256(), digest, &digestLen);
    const QByteArray fingerprintBytes(reinterpret_cast<const char *>(digest), static_cast<int>(digestLen));

    char subjectBuf[512];
    char issuerBuf[512];
    X509_NAME_oneline(X509_get_subject_name(peerCert), subjectBuf, sizeof(subjectBuf));
    X509_NAME_oneline(X509_get_issuer_name(peerCert), issuerBuf, sizeof(issuerBuf));

    const long verifyResult = SSL_get_verify_result(m_ssl);

    if (peerInfoOut) {
        peerInfoOut->fingerprintSha256Hex = QString::fromLatin1(fingerprintBytes.toHex());
        peerInfoOut->subject = QString::fromUtf8(subjectBuf);
        peerInfoOut->issuer = QString::fromUtf8(issuerBuf);
        peerInfoOut->systemTrusted = (verifyResult == X509_V_OK);
        if (!peerInfoOut->systemTrusted)
            peerInfoOut->verifyError = QString::fromUtf8(X509_verify_cert_error_string(verifyResult));
    }
    X509_free(peerCert);

    // Captured regardless of sessionToReuse — every control connection
    // needs to offer ITS OWN session for its data connections to
    // reuse, and there is no downside to also capturing one for a data
    // connection (simply never queried again in that case).
    if (!m_session)
        m_session = SSL_get1_session(m_ssl);

    return true;
}

void FtpTlsSocket::setSessionForReuse(SSL_SESSION *session)
{
    if (m_session)
        SSL_SESSION_free(m_session);
    if (session)
        SSL_SESSION_up_ref(session);
    m_session = session;
}

void FtpTlsSocket::pump()
{
    if (!m_qtSocket || m_eof)
        return;

    if (m_ssl) {
        for (;;) {
            if (SSL_pending(m_ssl) <= 0 && !pollFd(false, 0))
                return;
            char chunk[16384];
            const int ret = SSL_read(m_ssl, chunk, sizeof(chunk));
            if (ret > 0) {
                m_pending.append(chunk, ret);
                continue;
            }
            const int sslError = SSL_get_error(m_ssl, ret);
            if (sslError == SSL_ERROR_ZERO_RETURN || sslError == SSL_ERROR_SYSCALL)
                m_eof = true;
            return;
        }
    }

    // Pre-TLS: plain recv() on the same fd — the control connection's
    // plaintext banner and "AUTH TLS" reply have to go through this
    // path, since handshake() hasn't run yet at that point (see the
    // class doc comment).
    if (!pollFd(false, 0))
        return;
    char chunk[16384];
#ifdef Q_OS_WIN
    const int n = ::recv(static_cast<SOCKET>(fd()), chunk, sizeof(chunk), 0);
    if (n <= 0) {
        if (n == 0)
            m_eof = true;
        return;
    }
    m_pending.append(chunk, n);
#else
    const ssize_t n = ::recv(static_cast<int>(fd()), chunk, sizeof(chunk), 0);
    if (n <= 0) {
        if (n == 0)
            m_eof = true;
        return;
    }
    m_pending.append(chunk, static_cast<int>(n));
#endif
}

bool FtpTlsSocket::canReadLine() const
{
    const_cast<FtpTlsSocket *>(this)->pump();
    return m_pending.contains('\n');
}

QByteArray FtpTlsSocket::readLine()
{
    pump();
    const int idx = m_pending.indexOf('\n');
    if (idx < 0) {
        QByteArray result = m_pending;
        m_pending.clear();
        return result;
    }
    QByteArray result = m_pending.left(idx + 1);
    m_pending.remove(0, idx + 1);
    return result;
}

bool FtpTlsSocket::waitForReadyRead(int timeoutMs)
{
    pump();
    if (!m_pending.isEmpty())
        return true;
    if (!pollFd(false, timeoutMs))
        return false;
    pump();
    return !m_pending.isEmpty();
}

qint64 FtpTlsSocket::bytesAvailable() const
{
    const_cast<FtpTlsSocket *>(this)->pump();
    return m_pending.size();
}

qint64 FtpTlsSocket::read(char *buf, qint64 maxSize)
{
    pump();
    const qint64 n = qMin(maxSize, static_cast<qint64>(m_pending.size()));
    if (n <= 0)
        return 0;
    memcpy(buf, m_pending.constData(), static_cast<size_t>(n));
    m_pending.remove(0, static_cast<int>(n));
    return n;
}

QByteArray FtpTlsSocket::readAll()
{
    pump();
    QByteArray result = m_pending;
    m_pending.clear();
    return result;
}

qint64 FtpTlsSocket::write(const char *data, qint64 len)
{
    if (!m_qtSocket || m_eof)
        return -1;

    qint64 total = 0;
    while (total < len) {
        if (m_ssl) {
            const int ret = SSL_write(m_ssl, data + total, static_cast<int>(len - total));
            if (ret > 0) {
                total += ret;
                continue;
            }
            const int sslError = SSL_get_error(m_ssl, ret);
            if (pollForSslError(sslError, 15000))
                continue;
            m_lastError = QStringLiteral("TLS write failed (OpenSSL error %1)").arg(sslError);
            return total > 0 ? total : -1;
        }

#ifdef Q_OS_WIN
        const int n = ::send(static_cast<SOCKET>(fd()), data + total, static_cast<int>(len - total), 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                pollFd(true, 15000);
                continue;
            }
            m_lastError = QStringLiteral("Socket write failed");
            return total > 0 ? total : -1;
        }
#else
        const ssize_t n = ::send(static_cast<int>(fd()), data + total, static_cast<size_t>(len - total), 0);
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                pollFd(true, 15000);
                continue;
            }
            m_lastError = QStringLiteral("Socket write failed");
            return total > 0 ? total : -1;
        }
#endif
        total += n;
    }
    return total;
}

bool FtpTlsSocket::waitForBytesWritten(int /*timeoutMs*/)
{
    // write() above always blocks until every requested byte is
    // actually handed to the OS (or fails outright) — there is never
    // anything left to flush by the time it returns, unlike
    // QIODevice's async-buffered write model.
    return m_qtSocket && !m_eof;
}

bool FtpTlsSocket::isConnected() const
{
    // Deliberately NOT m_qtSocket->state(): once TLS (or even pre-TLS
    // raw recv()/send()) I/O bypasses Qt's own socket engine entirely,
    // Qt never gets a chance to observe the peer closing and its state
    // can go stale — same reasoning SftpBackend.cpp's libssh2 code
    // already relies on its own protocol-level return codes for this
    // rather than the QTcpSocket it borrowed a descriptor from. m_eof
    // is set the moment OUR OWN read path actually observes a clean or
    // abrupt close (see pump()).
    return m_qtSocket && !m_eof;
}

QString FtpTlsSocket::errorString() const
{
    if (!m_lastError.isEmpty())
        return m_lastError;
    return m_qtSocket ? m_qtSocket->errorString() : QString();
}

QHostAddress FtpTlsSocket::localAddress() const
{
    return m_qtSocket ? m_qtSocket->localAddress() : QHostAddress();
}

void FtpTlsSocket::setNoDelay()
{
    if (m_qtSocket)
        m_qtSocket->setSocketOption(QAbstractSocket::LowDelayOption, QVariant(1));
}

void FtpTlsSocket::disconnectGracefully()
{
    if (m_ssl)
        SSL_shutdown(m_ssl);   // best-effort close_notify — not waiting for the peer's own reply, matching QSslSocket::disconnectFromHost()'s non-blocking contract used elsewhere in this file
    if (m_qtSocket)
        m_qtSocket->disconnectFromHost();
}

void FtpTlsSocket::closeAbruptly()
{
    if (m_qtSocket)
        m_qtSocket->close();
}

bool FtpTlsSocket::waitForFullyDisconnected(int timeoutMs)
{
    if (!m_qtSocket)
        return true;
    return m_qtSocket->state() == QAbstractSocket::UnconnectedState || m_qtSocket->waitForDisconnected(timeoutMs);
}
