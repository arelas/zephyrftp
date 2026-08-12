#pragma once

#include <QString>
#include "ProxyConfig.h"

class QTcpSocket;

// Manually performs a SOCKS5 (RFC 1928/1929) or HTTP CONNECT proxy
// handshake on `socket` (freshly constructed, not yet connected) to
// reach targetHost:targetPort — needed anywhere the caller will later
// extract socket->socketDescriptor() for raw use (libssh2,
// FtpTlsSocket's raw OpenSSL).
//
// Why this exists instead of just QAbstractSocket::setProxy(): that
// mechanism DOES complete a genuinely working proxy tunnel, but only
// through Qt's own read()/write() API (QSocks5SocketEngine) — a
// standalone probe confirmed socketDescriptor() afterward returns an
// unusable value (observed: a small, clearly-bogus fd number, not the
// real underlying socket), not a raw pass-through pipe to the target.
// That's a hard blocker for anything that needs the real native fd —
// exactly what SftpBackend::ensureSession() (hands it to
// libssh2_session_handshake()) and FtpTlsSocket (hands it to OpenSSL's
// SSL_set_fd(), plus its own raw poll()/recv()/send() loops) both do.
// setProxy() is still used, and still correct, for FtpBackend's plain-
// FTP control/data connections (QtSocketAdapter over QSslSocket-as-TCP)
// — that path never extracts a raw descriptor, it stays entirely
// within Qt's own socket API from connect through teardown.
//
// If proxy.type is ProxyType::None, this is just a plain
// connectToHost()+waitForConnected() — the common, unproxied case.
// Blocking, same contract as every other connection-setup call in
// SftpBackend/FtpTlsSocket (both run on their own worker thread).
bool connectThroughProxy(QTcpSocket *socket, const ProxyConfig &proxy,
                          const QString &targetHost, quint16 targetPort,
                          int timeoutMs, QString *errorOut);
