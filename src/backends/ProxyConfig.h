#pragma once

#include <QString>

// A single, global proxy configuration applied to every SFTP/FTP/FTPS
// connection (see AppSettings::resolvedProxyConfig() and
// PreferencesDialog) — no per-site override exists, matching every
// other AppSettings field's "one value, used everywhere" shape.
// Deliberately never touches LocalBackend, which has no network
// connection to proxy.
//
// Applying it is just QAbstractSocket::setProxy(QNetworkProxy(...))
// before the existing connectToHost() call in each backend — Qt's own
// SOCKS5/HTTP-CONNECT proxy support handles the tunnel entirely before
// libssh2/OpenSSL ever see the resulting socket descriptor, so neither
// backend needs any proxy-protocol code of its own. See
// SftpBackend::ensureSession(), FtpBackend::ensureConnected()/
// openDataChannel(), and FtpTlsSocket::setProxy().
enum class ProxyType {
    None,
    Socks5,
    Http
};

struct ProxyConfig {
    ProxyType type = ProxyType::None;
    QString host;
    int port = 1080;
    QString username;   // optional, sent as SOCKS5 auth or HTTP Proxy-Authorization
    QString password;   // optional; NEVER persisted here or in settings.json —
                         // see AppSettings::proxyPassword()'s own doc comment.
                         // Only ever held in memory for the duration of a
                         // connection attempt.
};

// Stable string keys for JSON persistence — same idiom as Protocol.h's
// protocolToKey()/protocolFromKey(), deliberately not the raw enum's
// integer values.
inline QString proxyTypeToKey(ProxyType type)
{
    switch (type) {
    case ProxyType::Socks5: return QStringLiteral("socks5");
    case ProxyType::Http:   return QStringLiteral("http");
    case ProxyType::None:   return QStringLiteral("none");
    }
    return QStringLiteral("none");
}

// Anything unrecognized — including the key being absent entirely,
// which is exactly what every settings.json written before this field
// existed looks like — reads back as None. That's the correct
// migration: no proxy was ever configurable before this existed.
inline ProxyType proxyTypeFromKey(const QString &key)
{
    if (key == QStringLiteral("socks5")) return ProxyType::Socks5;
    if (key == QStringLiteral("http"))   return ProxyType::Http;
    return ProxyType::None;
}
