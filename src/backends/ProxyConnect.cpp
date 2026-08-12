#include "ProxyConnect.h"

#include <QTcpSocket>

namespace {

// Blocks until exactly n bytes are available and returns them in *out
// — every proxy handshake reply below has either a fixed size or a
// fixed header whose contents dictate how many more bytes to read next,
// never a line-oriented shape, so this (not canReadLine()) is the right
// primitive here.
bool readExact(QTcpSocket *socket, QByteArray *out, int n, int timeoutMs)
{
    out->clear();
    while (out->size() < n) {
        if (socket->bytesAvailable() <= 0) {
            if (!socket->waitForReadyRead(timeoutMs))
                return false;
        }
        out->append(socket->read(n - out->size()));
    }
    return true;
}

// RFC 1928 (protocol) + RFC 1929 (username/password subnegotiation).
// Domain-name addressing (type 0x03) is used uniformly for the CONNECT
// request regardless of whether targetHost is a hostname or an IP
// literal — universally supported by real SOCKS5 proxies, and avoids
// this code needing to distinguish address families itself.
bool socks5Handshake(QTcpSocket *socket, const ProxyConfig &proxy, const QString &targetHost,
                      quint16 targetPort, int timeoutMs, QString *errorOut)
{
    const bool useAuth = !proxy.username.isEmpty();

    QByteArray greeting;
    greeting.append(char(0x05));
    if (useAuth) {
        greeting.append(char(0x02));
        greeting.append(char(0x00));   // no-auth
        greeting.append(char(0x02));   // username/password
    } else {
        greeting.append(char(0x01));
        greeting.append(char(0x00));   // no-auth only
    }
    socket->write(greeting);
    if (!socket->waitForBytesWritten(timeoutMs)) {
        *errorOut = QStringLiteral("SOCKS5: could not send greeting: %1").arg(socket->errorString());
        return false;
    }

    QByteArray methodReply;
    if (!readExact(socket, &methodReply, 2, timeoutMs)) {
        *errorOut = QStringLiteral("SOCKS5: no response to greeting");
        return false;
    }
    if (uchar(methodReply[0]) != 0x05) {
        *errorOut = QStringLiteral("SOCKS5: unexpected version in greeting reply");
        return false;
    }
    const uchar method = uchar(methodReply[1]);
    if (method == 0xFF) {
        *errorOut = QStringLiteral("SOCKS5: proxy rejected every offered authentication method");
        return false;
    }

    if (method == 0x02) {
        const QByteArray user = proxy.username.toUtf8();
        const QByteArray pass = proxy.password.toUtf8();
        if (user.size() > 255 || pass.size() > 255) {
            *errorOut = QStringLiteral("SOCKS5: username or password longer than 255 bytes");
            return false;
        }
        QByteArray authReq;
        authReq.append(char(0x01));
        authReq.append(char(user.size()));
        authReq.append(user);
        authReq.append(char(pass.size()));
        authReq.append(pass);
        socket->write(authReq);
        if (!socket->waitForBytesWritten(timeoutMs)) {
            *errorOut = QStringLiteral("SOCKS5: could not send auth: %1").arg(socket->errorString());
            return false;
        }
        QByteArray authReply;
        if (!readExact(socket, &authReply, 2, timeoutMs)) {
            *errorOut = QStringLiteral("SOCKS5: no response to authentication");
            return false;
        }
        if (uchar(authReply[1]) != 0x00) {
            *errorOut = QStringLiteral("SOCKS5: authentication rejected — check the proxy username/password");
            return false;
        }
    }

    const QByteArray hostBytes = targetHost.toUtf8();
    if (hostBytes.size() > 255) {
        *errorOut = QStringLiteral("SOCKS5: target host name longer than 255 bytes");
        return false;
    }
    QByteArray connectReq;
    connectReq.append(char(0x05));
    connectReq.append(char(0x01));   // CONNECT
    connectReq.append(char(0x00));   // reserved
    connectReq.append(char(0x03));   // address type: domain name
    connectReq.append(char(hostBytes.size()));
    connectReq.append(hostBytes);
    connectReq.append(char((targetPort >> 8) & 0xFF));
    connectReq.append(char(targetPort & 0xFF));
    socket->write(connectReq);
    if (!socket->waitForBytesWritten(timeoutMs)) {
        *errorOut = QStringLiteral("SOCKS5: could not send CONNECT: %1").arg(socket->errorString());
        return false;
    }

    QByteArray replyHeader;
    if (!readExact(socket, &replyHeader, 4, timeoutMs)) {
        *errorOut = QStringLiteral("SOCKS5: no response to CONNECT");
        return false;
    }
    if (uchar(replyHeader[0]) != 0x05) {
        *errorOut = QStringLiteral("SOCKS5: unexpected version in CONNECT reply");
        return false;
    }
    const uchar replyCode = uchar(replyHeader[1]);
    if (replyCode != 0x00) {
        // RFC 1928 §6 reply codes — spelled out since "code 3" alone
        // means nothing to whoever reads this failure message.
        static const char *const kReasons[] = {
            "succeeded", "general SOCKS server failure", "connection not allowed by ruleset",
            "network unreachable", "host unreachable", "connection refused", "TTL expired",
            "command not supported", "address type not supported",
        };
        const QString reason = replyCode < 9 ? QString::fromLatin1(kReasons[replyCode])
                                              : QStringLiteral("unknown (%1)").arg(replyCode);
        *errorOut = QStringLiteral("SOCKS5: CONNECT failed: %1").arg(reason);
        return false;
    }

    // The reply's own bound-address/port (variable length depending on
    // address type) — not used for anything, but must still be read off
    // the wire before the tunnel is usable for raw application data.
    const uchar addrType = uchar(replyHeader[3]);
    int addrLen = 0;
    switch (addrType) {
    case 0x01: addrLen = 4; break;    // IPv4
    case 0x04: addrLen = 16; break;   // IPv6
    case 0x03: {
        QByteArray lenByte;
        if (!readExact(socket, &lenByte, 1, timeoutMs)) {
            *errorOut = QStringLiteral("SOCKS5: truncated CONNECT reply (address length)");
            return false;
        }
        addrLen = uchar(lenByte[0]);
        break;
    }
    default:
        *errorOut = QStringLiteral("SOCKS5: unknown address type in CONNECT reply");
        return false;
    }
    QByteArray discard;
    if (!readExact(socket, &discard, addrLen + 2, timeoutMs)) {
        *errorOut = QStringLiteral("SOCKS5: truncated CONNECT reply (address/port)");
        return false;
    }
    return true;
}

// A minimal HTTP/1.1 CONNECT tunnel request — just enough to establish
// the tunnel, not a general HTTP client. Reads the status line plus
// headers up to the blank line terminator via canReadLine()/readLine(),
// the same line-oriented technique FtpBackend::readReply() already
// uses for a real-world protocol reply of unknown length.
bool httpConnectHandshake(QTcpSocket *socket, const ProxyConfig &proxy, const QString &targetHost,
                           quint16 targetPort, int timeoutMs, QString *errorOut)
{
    const QString hostPort = QStringLiteral("%1:%2").arg(targetHost).arg(targetPort);
    QByteArray request = QStringLiteral("CONNECT %1 HTTP/1.1\r\nHost: %1\r\n").arg(hostPort).toUtf8();
    if (!proxy.username.isEmpty()) {
        const QByteArray credentials =
            (proxy.username + QStringLiteral(":") + proxy.password).toUtf8().toBase64();
        request += "Proxy-Authorization: Basic " + credentials + "\r\n";
    }
    request += "\r\n";

    socket->write(request);
    if (!socket->waitForBytesWritten(timeoutMs)) {
        *errorOut = QStringLiteral("HTTP CONNECT: could not send request: %1").arg(socket->errorString());
        return false;
    }

    auto readLine = [&](QByteArray *line) -> bool {
        while (!socket->canReadLine()) {
            if (!socket->waitForReadyRead(timeoutMs))
                return false;
        }
        *line = socket->readLine();
        return true;
    };

    QByteArray statusLine;
    if (!readLine(&statusLine)) {
        *errorOut = QStringLiteral("HTTP CONNECT: no response from proxy");
        return false;
    }
    const QList<QByteArray> parts = statusLine.split(' ');
    const int statusCode = parts.size() >= 2 ? parts[1].toInt() : 0;
    if (statusCode < 200 || statusCode >= 300) {
        *errorOut = QStringLiteral("HTTP CONNECT failed: %1")
            .arg(QString::fromUtf8(statusLine).trimmed());
        return false;
    }

    // Drain the remaining response headers up to the blank-line
    // terminator — required before the tunnel is usable for raw
    // application data (anything still buffered here would otherwise
    // be misread as the target protocol's own first bytes).
    for (;;) {
        QByteArray line;
        if (!readLine(&line)) {
            *errorOut = QStringLiteral("HTTP CONNECT: truncated response headers");
            return false;
        }
        if (line == "\r\n" || line == "\n")
            break;
    }
    return true;
}

}   // namespace

bool connectThroughProxy(QTcpSocket *socket, const ProxyConfig &proxy, const QString &targetHost,
                          quint16 targetPort, int timeoutMs, QString *errorOut)
{
    if (proxy.type == ProxyType::None) {
        socket->connectToHost(targetHost, targetPort);
        if (!socket->waitForConnected(timeoutMs)) {
            if (errorOut) *errorOut = socket->errorString();
            return false;
        }
        return true;
    }

    socket->connectToHost(proxy.host, static_cast<quint16>(proxy.port));
    if (!socket->waitForConnected(timeoutMs)) {
        if (errorOut) {
            *errorOut = QStringLiteral("could not reach proxy %1:%2: %3")
                .arg(proxy.host).arg(proxy.port).arg(socket->errorString());
        }
        return false;
    }

    QString handshakeError;
    const bool ok = proxy.type == ProxyType::Socks5
        ? socks5Handshake(socket, proxy, targetHost, targetPort, timeoutMs, &handshakeError)
        : httpConnectHandshake(socket, proxy, targetHost, targetPort, timeoutMs, &handshakeError);
    if (!ok) {
        if (errorOut) *errorOut = handshakeError;
        socket->abort();
        return false;
    }
    return true;
}
