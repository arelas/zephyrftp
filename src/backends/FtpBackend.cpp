#include "FtpBackend.h"
#include "../ui/CertificateVerifier.h"
#include "BandwidthThrottle.h"

#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkProxy>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QHash>
#include <QMetaObject>
#include <QStandardPaths>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

namespace {
// Persists accepted certificate fingerprints across runs — the FTPS
// equivalent of SftpBackend.cpp's knownHostsFilePath(), but a plain JSON
// array (this project's own convention for its own non-secret state; see
// SiteStore) rather than libssh2's OpenSSH-dictated known_hosts format,
// since nothing here is bound to a third-party API's file format. A
// certificate fingerprint is public information, not a secret — same
// status as an SSH host-key fingerprint, which this project already
// stores in plaintext.
QString knownCertsFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/known_certs.json");
}

QString loadTrustedFingerprint(const QString &host, int port)
{
    QFile file(knownCertsFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return QString();   // no file yet — expected on first run

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return QString();   // corrupt/unexpected content — fail soft, treat as unknown

    for (const QJsonValue &value : doc.array()) {
        if (!value.isObject())
            continue;
        const QJsonObject obj = value.toObject();
        if (obj.value(QStringLiteral("host")).toString() == host
            && obj.value(QStringLiteral("port")).toInt() == port) {
            return obj.value(QStringLiteral("fingerprint")).toString();
        }
    }
    return QString();
}

void saveTrustedFingerprint(const QString &host, int port, const QString &fingerprint)
{
    QJsonArray array;
    QFile readFile(knownCertsFilePath());
    if (readFile.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(readFile.readAll());
        if (doc.isArray())
            array = doc.array();
        readFile.close();
    }

    // Replace any existing entry for this host:port rather than
    // appending a duplicate — this is the "certificate changed" update
    // path as well as the first-trust path.
    QJsonArray updated;
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;
        const QJsonObject obj = value.toObject();
        if (obj.value(QStringLiteral("host")).toString() == host
            && obj.value(QStringLiteral("port")).toInt() == port) {
            continue;
        }
        updated.append(obj);
    }
    QJsonObject entry;
    entry[QStringLiteral("host")] = host;
    entry[QStringLiteral("port")] = port;
    entry[QStringLiteral("fingerprint")] = fingerprint;
    updated.append(entry);

    QFile writeFile(knownCertsFilePath());
    if (writeFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        writeFile.write(QJsonDocument(updated).toJson(QJsonDocument::Indented));
}

// Makes a QTcpServer hand out QSslSocket instances (adopted via
// setSocketDescriptor()) from nextPendingConnection() instead of plain
// QTcpSocket ones — needed for active/PORT-mode data connections, which
// are TCP-accepted (the server dials back to us) but still need this
// side to act as the TLS CLIENT once PROT P is active: per RFC 4217 the
// FTP client/server TLS roles don't flip just because the TCP dial
// direction did. Qt has no built-in "accept, but hand me a QSslSocket"
// server (QSslServer, added in Qt 6.4, does the opposite — it makes
// accepted connections act as the TLS SERVER, which is wrong for this
// use), so this is the documented manual pattern instead.
//
// rawForTls: true for FTPS data connections — hands out plain
// QTcpSocket instances instead, which finalizeDataChannel() adopts
// into an FtpTlsSocket (raw OpenSSL) rather than a QSslSocket, since
// QSslSocket's own TLS machinery can't do the TLS-1.2 session reuse
// FtpTlsSocket exists for (see FtpTlsSocket.h). Plain FTP (and any
// FTPS connection before PROT P succeeds — never actually reached
// today, but this class doesn't assume otherwise) keeps handing out
// QSslSocket, used purely as TCP, exactly as before FtpTlsSocket
// existed.
class SslAcceptingTcpServer : public QTcpServer {
public:
    explicit SslAcceptingTcpServer(bool rawForTls) : m_rawForTls(rawForTls) {}

protected:
    void incomingConnection(qintptr handle) override
    {
        // No parent: nextPendingConnection() transfers ownership to
        // whoever calls it (per QTcpServer's own documented contract) —
        // parenting this to the server itself would mean deleting the
        // server (finalizeDataChannel() does, right after taking this
        // pointer) also deletes this socket out from under its new
        // owner, a real dangling-pointer crash caught by actually
        // running this against a live server, not just reasoning about
        // it.
        if (m_rawForTls) {
            auto *socket = new QTcpSocket();
            if (socket->setSocketDescriptor(handle))
                addPendingConnection(socket);
            else
                delete socket;
            return;
        }
        auto *socket = new QSslSocket();
        if (socket->setSocketDescriptor(handle))
            addPendingConnection(socket);
        else
            delete socket;
    }

private:
    bool m_rawForTls;
};
}

FtpBackend::FtpBackend(FtpCredentials credentials, CertificateVerifier *certificateVerifier,
                        QObject *parent)
    : RemoteBackend(parent)
    , m_credentials(std::move(credentials))
    , m_certificateVerifier(certificateVerifier)
{
}

FtpBackend::~FtpBackend()
{
    teardown();
}

void FtpBackend::teardown()
{
    if (m_controlSocket) {
        // QUIT is a courtesy, not load-bearing — best-effort only. Not
        // waiting for its reply: this runs during teardown, and a slow
        // or unresponsive server shouldn't delay destruction.
        if (m_controlSocket->isConnected())
            m_controlSocket->write("QUIT\r\n", 6);
        m_controlSocket->disconnectGracefully();
        delete m_controlSocket;
        m_controlSocket = nullptr;
    }
    m_connected = false;
    m_dataProtected = false;
    m_trustedCertFingerprint.clear();
}

QString FtpBackend::currentPath() const
{
    QMutexLocker locker(&m_currentPathMutex);
    return m_currentPath;
}

void FtpBackend::requestCancel()
{
    m_cancelRequested.storeRelaxed(true);
}

void FtpBackend::requestPause()
{
    m_pauseRequested.storeRelaxed(true);
}

// ---------------------------------------------------------------------
// Protocol core
// ---------------------------------------------------------------------

FtpBackend::FtpReply FtpBackend::readReply()
{
    FtpReply reply;
    if (!m_controlSocket)
        return reply;

    auto readOneLine = [this](bool *ok) -> QString {
        *ok = true;
        while (!m_controlSocket->canReadLine()) {
            if (!m_controlSocket->waitForReadyRead(15000)) {
                *ok = false;
                return QString();
            }
        }
        QByteArray raw = m_controlSocket->readLine();
        while (raw.endsWith('\n') || raw.endsWith('\r'))
            raw.chop(1);
        const QString stripped = QString::fromUtf8(raw);
        emit commandLogged(stripped);   // Commands pane — see RemoteBackend's doc comment
        return stripped;
    };

    bool ok = true;
    const QString firstLine = readOneLine(&ok);
    if (!ok || firstLine.length() < 4)
        return reply;   // code stays 0 — isValid() reports failure

    bool codeOk = false;
    const int code = firstLine.left(3).toInt(&codeOk);
    if (!codeOk)
        return reply;

    QStringList lines;
    lines.append(firstLine.mid(4));

    if (firstLine.at(3) == QLatin1Char('-')) {
        // Multi-line reply: RFC 959 says the terminator is a line that
        // starts with the SAME code followed by a space — intermediate
        // lines are otherwise unconstrained (some servers repeat the
        // code with a dash, some don't; either is fine, not relied on).
        const QString terminator = QString::number(code) + QLatin1Char(' ');
        for (;;) {
            const QString line = readOneLine(&ok);
            if (!ok)
                return FtpReply{};
            if (line.startsWith(terminator)) {
                lines.append(line.mid(terminator.length()));
                break;
            }
            lines.append(line);
        }
    }

    reply.code = code;
    reply.text = lines.join(QLatin1Char('\n'));
    return reply;
}

FtpBackend::FtpReply FtpBackend::sendCommand(const QString &text)
{
    if (!m_controlSocket || !m_controlSocket->isConnected())
        return FtpReply{};

    // The Commands pane's log gets a masked copy — text can be a PASS
    // command carrying the raw password, which must never reach a
    // visible (and copy-pasteable) UI log. Masking is purely for that
    // log line below; the real, unmasked `text` is still what's actually
    // written to the wire two lines down.
    QString logText = text;
    if (logText.startsWith(QStringLiteral("PASS "), Qt::CaseInsensitive))
        logText = QStringLiteral("PASS ***");
    emit commandLogged(QStringLiteral("> %1").arg(logText));

    const QByteArray line = (text + QStringLiteral("\r\n")).toUtf8();
    if (m_controlSocket->write(line.constData(), line.size()) < 0)
        return FtpReply{};
    if (!m_controlSocket->waitForBytesWritten(15000))
        return FtpReply{};

    return readReply();
}

bool FtpBackend::askUserToTrustCertificate(const QString &fingerprint, const QString &details, bool isMismatch)
{
    if (!m_certificateVerifier) {
        // Fails safe: with no way to ask a person, refuse rather than
        // silently trust. Should never happen in practice (MainWindow
        // always wires one up), but if it did, silently trusting an
        // unverifiable certificate would defeat the entire point of
        // this feature.
        return false;
    }

    bool accepted = false;
    QMetaObject::invokeMethod(m_certificateVerifier, "confirmCertificate", Qt::BlockingQueuedConnection,
                               Q_RETURN_ARG(bool, accepted),
                               Q_ARG(QString, m_credentials.host),
                               Q_ARG(int, m_credentials.port),
                               Q_ARG(QString, fingerprint),
                               Q_ARG(QString, details),
                               Q_ARG(bool, isMismatch));
    return accepted;
}

bool FtpBackend::verifyTlsPeer(const FtpTlsSocket::PeerInfo &info, bool isDataConnection, QString *errorOut)
{
    // FtpTlsSocket::handshake() already completed by the time this is
    // called regardless of certificate trust (SSL_VERIFY_NONE — see
    // its own doc comment for why); this is the actual trust decision
    // that fails the connection closed. Same TOFU shape the previous
    // QSslSocket::sslErrors()-based verifyPeerCertificate() used, now
    // sourced from OpenSSL's own verify result instead of Qt's
    // QSslError list.
    if (isDataConnection) {
        // No prompt for a data connection — the control connection
        // already established trust for this session. A DIFFERENT
        // certificate here is treated as suspicious, not as something
        // to ask about. Checked regardless of whether OpenSSL's own
        // chain validation considered the certificate independently
        // valid — "independently valid" isn't the bar for a data
        // connection, matching the control connection's own
        // already-trusted certificate is (a real bug found by code
        // review in the previous QSslSocket-based implementation, see
        // git history — preserved here deliberately).
        const bool trusted = !m_trustedCertFingerprint.isEmpty()
            && info.fingerprintSha256Hex == m_trustedCertFingerprint;
        if (!trusted && errorOut) {
            *errorOut = QStringLiteral(
                "Certificate not trusted:\nSubject: %1\nIssuer: %2\nProblem: certificate differs from the "
                "control connection's already-trusted certificate").arg(info.subject, info.issuer);
        }
        return trusted;
    }

    if (info.systemTrusted) {
        // A genuinely valid certificate (system trust store, e.g. a
        // real CA-issued cert) — record the fingerprint without
        // prompting. This session's data connections compare against
        // it (see above), and a LATER connection presenting something
        // unverifiable is correctly treated as a change rather than a
        // first-ever sighting.
        m_trustedCertFingerprint = info.fingerprintSha256Hex;
        return true;
    }

    const QString details = QStringLiteral("Subject: %1\nIssuer: %2\nProblem: %3")
        .arg(info.subject, info.issuer, info.verifyError);

    bool trusted = false;
    const QString stored = loadTrustedFingerprint(m_credentials.host, m_credentials.port);
    if (stored.isEmpty()) {
        trusted = askUserToTrustCertificate(info.fingerprintSha256Hex, details, /*isMismatch=*/false);
        if (trusted)
            saveTrustedFingerprint(m_credentials.host, m_credentials.port, info.fingerprintSha256Hex);
    } else if (stored == info.fingerprintSha256Hex) {
        trusted = true;
    } else {
        trusted = askUserToTrustCertificate(info.fingerprintSha256Hex, details, /*isMismatch=*/true);
        if (trusted)
            saveTrustedFingerprint(m_credentials.host, m_credentials.port, info.fingerprintSha256Hex);
    }

    if (trusted) {
        m_trustedCertFingerprint = info.fingerprintSha256Hex;
    } else if (errorOut) {
        *errorOut = QStringLiteral("Certificate not trusted:\n%1").arg(details);
    }

    return trusted;
}

QNetworkProxy FtpBackend::qtProxy() const
{
    if (m_credentials.proxy.type == ProxyType::None)
        return QNetworkProxy(QNetworkProxy::NoProxy);
    const QNetworkProxy::ProxyType qtType = m_credentials.proxy.type == ProxyType::Socks5
        ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy;
    return QNetworkProxy(qtType, m_credentials.proxy.host,
                          static_cast<quint16>(m_credentials.proxy.port),
                          m_credentials.proxy.username, m_credentials.proxy.password);
}

bool FtpBackend::ensureConnected()
{
    if (m_connected)
        return true;

    const bool isFtps = m_credentials.ftpsMode == FtpsMode::Explicit;

    if (isFtps) {
        // FtpTlsSocket (raw OpenSSL, forced to TLS 1.2) — needed so the
        // control connection's session is genuinely reusable by its data
        // connections; see FtpTlsSocket.h's own doc comment for why
        // QSslSocket can't do this.
        m_controlSocket = new FtpTlsSocket();
        auto *tlsSocket = static_cast<FtpTlsSocket *>(m_controlSocket);
        tlsSocket->setProxy(m_credentials.proxy);
        QString connectError;
        if (!tlsSocket->connectToHost(m_credentials.host, static_cast<quint16>(m_credentials.port), &connectError)) {
            emit connectionFailed(QStringLiteral("TCP connect to %1:%2 failed: %3")
                                   .arg(m_credentials.host).arg(m_credentials.port).arg(connectError));
            teardown();
            return false;
        }
    } else {
        // QSslSocket used purely as TCP for plain FTP — startClientEncryption()
        // is simply never called for this mode.
        auto *qtSocket = new QSslSocket();
        m_controlSocket = new QtSocketAdapter(qtSocket);
        qtSocket->setProxy(qtProxy());
        qtSocket->connectToHost(m_credentials.host, static_cast<quint16>(m_credentials.port));
        if (!qtSocket->waitForConnected(10000)) {
            emit connectionFailed(QStringLiteral("TCP connect to %1:%2 failed: %3")
                                   .arg(m_credentials.host).arg(m_credentials.port).arg(qtSocket->errorString()));
            teardown();
            return false;
        }
        // TCP_NODELAY — same reasoning as SftpBackend's identical setting:
        // Nagle's batching behavior is the wrong trade for a request/reply
        // protocol like FTP's control connection.
        qtSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    }

    const FtpReply welcome = readReply();
    if (!welcome.isValid() || welcome.code >= 400) {
        emit connectionFailed(QStringLiteral("Server did not send a valid welcome banner"));
        teardown();
        return false;
    }

    if (isFtps) {
        const FtpReply authReply = sendCommand(QStringLiteral("AUTH TLS"));
        if (!authReply.isValid() || authReply.code != 234) {
            emit connectionFailed(QStringLiteral("Server refused AUTH TLS: %1").arg(authReply.text));
            teardown();
            return false;
        }

        // Upgrades the EXISTING plaintext connection in place — this is
        // what makes explicit FTPS "explicit" (as opposed to implicit
        // FTPS, which is TLS from the very first byte on a separate
        // port — not implemented here, see the class doc comment). No
        // session to reuse yet — this is the control connection's own
        // first handshake; it's what DATA connections reuse afterward
        // (see finalizeDataChannel()).
        auto *tlsSocket = static_cast<FtpTlsSocket *>(m_controlSocket);
        FtpTlsSocket::PeerInfo peerInfo;
        QString tlsError;
        const bool handshakeOk = tlsSocket->handshake(nullptr, &peerInfo, &tlsError)
            && verifyTlsPeer(peerInfo, /*isDataConnection=*/false, &tlsError);
        if (!handshakeOk) {
            emit connectionFailed(QStringLiteral(
                "TLS handshake failed: %1. ZephyrFTP will not connect to a server whose "
                "certificate it cannot verify without your explicit confirmation.").arg(tlsError));
            teardown();
            return false;
        }
    }

    const FtpReply userReply = sendCommand(QStringLiteral("USER %1").arg(m_credentials.username));
    if (!userReply.isValid()) {
        emit connectionFailed(QStringLiteral("No response to USER command"));
        teardown();
        return false;
    }
    if (userReply.code == 331) {   // "user name okay, need password"
        const FtpReply passReply = sendCommand(QStringLiteral("PASS %1").arg(m_credentials.password));
        if (!passReply.isValid() || passReply.code >= 400) {
            emit connectionFailed(QStringLiteral("Login failed: %1").arg(passReply.text));
            teardown();
            return false;
        }
    } else if (userReply.code >= 400) {
        emit connectionFailed(QStringLiteral("Login failed: %1").arg(userReply.text));
        teardown();
        return false;
    }
    // else: 230 directly, no password needed — proceed.

    if (isFtps) {
        // Protects the DATA channel too, not just control — required by
        // RFC 4217 for a properly-encrypted FTPS session. Treated as a
        // hard failure rather than silently falling back to unprotected
        // data connections: someone who explicitly asked for FTPS
        // should never get a plaintext data channel without being told.
        const FtpReply pbszReply = sendCommand(QStringLiteral("PBSZ 0"));
        if (!pbszReply.isValid() || pbszReply.code >= 400) {
            emit connectionFailed(QStringLiteral("Server refused PBSZ 0: %1").arg(pbszReply.text));
            teardown();
            return false;
        }
        const FtpReply protReply = sendCommand(QStringLiteral("PROT P"));
        if (!protReply.isValid() || protReply.code >= 400) {
            emit connectionFailed(
                QStringLiteral("Server refused PROT P (encrypted data channel): %1").arg(protReply.text));
            teardown();
            return false;
        }
        m_dataProtected = true;
    }

    // Binary mode, always — this app has no use for FTP's ASCII/text
    // transfer mode, which translates line endings in flight and would
    // silently corrupt anything that isn't plain text.
    const FtpReply typeReply = sendCommand(QStringLiteral("TYPE I"));
    if (!typeReply.isValid() || typeReply.code >= 400) {
        emit connectionFailed(QStringLiteral("Server refused TYPE I (binary mode): %1").arg(typeReply.text));
        teardown();
        return false;
    }

    // Starting directory — same convention as SftpBackend::ensureSession():
    // resolve the server's own idea of "home" via PWD, or use a
    // configured starting directory.
    if (m_credentials.useHomeDirectory || m_credentials.startingDirectory.isEmpty()) {
        const FtpReply pwdReply = sendCommand(QStringLiteral("PWD"));
        if (pwdReply.isValid() && pwdReply.code == 257) {
            // Reply text looks like: "/home/user" is the current directory
            const int firstQuote = pwdReply.text.indexOf(QLatin1Char('"'));
            const int lastQuote = pwdReply.text.lastIndexOf(QLatin1Char('"'));
            if (firstQuote >= 0 && lastQuote > firstQuote) {
                QString path = pwdReply.text.mid(firstQuote + 1, lastQuote - firstQuote - 1);
                path.replace(QStringLiteral("\"\""), QStringLiteral("\""));   // RFC 959: embedded quotes are doubled
                QMutexLocker locker(&m_currentPathMutex);
                m_currentPath = path;
            }
        }
        {
            QMutexLocker locker(&m_currentPathMutex);
            if (m_currentPath.isEmpty())
                m_currentPath = QStringLiteral("/");
        }
    } else {
        QMutexLocker locker(&m_currentPathMutex);
        m_currentPath = m_credentials.startingDirectory;
    }

    m_connected = true;
    emit connected();
    return true;
}

bool FtpBackend::openDataChannel(DataChannel *channel, QString *errorOut)
{
    const FtpReply pasvReply = sendCommand(QStringLiteral("PASV"));
    if (!pasvReply.isValid()) {
        // No reply at all — sendCommand() already checked the control
        // socket's state, so this means the control connection itself
        // is gone (dropped, idle-timed-out server-side), not a server
        // actively refusing PASV. Falling back to active mode here would
        // just fail again for the same underlying reason, but with a far
        // more confusing error — openActiveDataChannel()'s IPv4 check
        // reads a disconnected socket's (empty) localAddress() as "not
        // IPv4" and reports that instead of the real problem. Surface
        // the real problem directly instead of masking it.
        if (errorOut) *errorOut = QStringLiteral("Lost connection to the server");
        return false;
    }
    if (pasvReply.code != 227) {
        // The server replied but refused PASV outright — a real, if
        // rare, sign it requires active mode. A malformed 227 reply
        // (parsed below) stays a hard error rather than silently trying
        // active mode for a genuinely different problem.
        return openActiveDataChannel(channel, errorOut);
    }

    // Reply text looks like: Entering Passive Mode (192,168,1,5,200,15).
    const int openParen = pasvReply.text.indexOf(QLatin1Char('('));
    const int closeParen = pasvReply.text.indexOf(QLatin1Char(')'), openParen);
    if (openParen < 0 || closeParen < 0) {
        if (errorOut) *errorOut = QStringLiteral("Could not parse PASV reply: %1").arg(pasvReply.text);
        return false;
    }
    const QStringList parts = pasvReply.text.mid(openParen + 1, closeParen - openParen - 1).split(QLatin1Char(','));
    if (parts.size() != 6) {
        if (errorOut) *errorOut = QStringLiteral("Could not parse PASV reply: %1").arg(pasvReply.text);
        return false;
    }

    // Each field is range-checked to [0, 255] — a real gap found by code
    // review: toInt() alone accepts any valid integer string ("300",
    // "-1", ...), which used to fall through as a malformed dotted-quad
    // host ("300.1.1.1", triggering a confusing DNS-lookup failure
    // instead of the clear "Could not parse PASV reply" this code
    // already intends for a malformed reply) or, worse, a negative
    // octets[4]/octets[5] feeding `octets[4] << 8` below — a left shift
    // of a negative signed value, undefined behavior in C++. A malformed
    // or actively malicious PASV reply (a compromised/rogue server, or a
    // MITM on the control connection) is exactly the input this should
    // be hardened against, not trusted to be well-formed.
    bool ok = true;
    int octets[6];
    for (int i = 0; i < 6 && ok; ++i) {
        octets[i] = parts[i].trimmed().toInt(&ok);
        if (ok && (octets[i] < 0 || octets[i] > 255))
            ok = false;
    }
    if (!ok) {
        if (errorOut) *errorOut = QStringLiteral("Could not parse PASV reply: %1").arg(pasvReply.text);
        return false;
    }

    const QString dataHost = QStringLiteral("%1.%2.%3.%4")
        .arg(octets[0]).arg(octets[1]).arg(octets[2]).arg(octets[3]);
    const quint16 dataPort = static_cast<quint16>((octets[4] << 8) | octets[5]);

    // TLS handshake (FTPS only) is deferred to finalizeDataChannel() —
    // see that method's own doc comment for why, same for both PASV and
    // active/PORT.
    if (m_credentials.ftpsMode == FtpsMode::Explicit) {
        auto *tlsSocket = new FtpTlsSocket();
        tlsSocket->setProxy(m_credentials.proxy);
        QString connectError;
        if (!tlsSocket->connectToHost(dataHost, dataPort, &connectError)) {
            if (errorOut) {
                *errorOut = QStringLiteral("Data connection to %1:%2 failed: %3")
                    .arg(dataHost).arg(dataPort).arg(connectError);
            }
            delete tlsSocket;
            return false;
        }
        channel->socket = tlsSocket;
    } else {
        auto *dataSocket = new QSslSocket();
        dataSocket->setProxy(qtProxy());
        dataSocket->connectToHost(dataHost, dataPort);
        if (!dataSocket->waitForConnected(10000)) {
            if (errorOut) {
                *errorOut = QStringLiteral("Data connection to %1:%2 failed: %3")
                    .arg(dataHost).arg(dataPort).arg(dataSocket->errorString());
            }
            delete dataSocket;
            return false;
        }
        dataSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        channel->socket = new QtSocketAdapter(dataSocket);
    }

    channel->active = false;
    return true;
}

bool FtpBackend::openActiveDataChannel(DataChannel *channel, QString *errorOut)
{
    if (!m_controlSocket) {
        if (errorOut) *errorOut = QStringLiteral("No control connection");
        return false;
    }

    // IPv4 only, matching PASV's existing scope — the PORT command
    // itself (RFC 959) is IPv4-only regardless. Checked via
    // toIPv4Address()'s own success flag, NOT protocol() ==
    // IPv4Protocol — a real bug found by code review: on a dual-stack
    // host, the control connection's local address can be surfaced as
    // an IPv4-MAPPED IPv6 address (protocol() reports IPv6Protocol) even
    // though it's really an IPv4 address underneath, one
    // toIPv4Address() still correctly extracts. The old strict-protocol()
    // check rejected that case outright with no real fallback attempt,
    // even though a genuine active/PORT session over that same address
    // would have worked.
    const QHostAddress localAddress = m_controlSocket->localAddress();
    bool localAddressIsIPv4 = false;
    const quint32 ip = localAddress.toIPv4Address(&localAddressIsIPv4);
    if (!localAddressIsIPv4) {
        if (errorOut) *errorOut = QStringLiteral("Active mode requires an IPv4 control connection");
        return false;
    }

    auto *server = new SslAcceptingTcpServer(/*rawForTls=*/m_credentials.ftpsMode == FtpsMode::Explicit);
    if (!server->listen(localAddress)) {
        if (errorOut) *errorOut = QStringLiteral("Could not open a local port for active mode: %1")
            .arg(server->errorString());
        delete server;
        return false;
    }

    const quint16 port = server->serverPort();
    const QString portCommand = QStringLiteral("PORT %1,%2,%3,%4,%5,%6")
        .arg((ip >> 24) & 0xFF).arg((ip >> 16) & 0xFF).arg((ip >> 8) & 0xFF).arg(ip & 0xFF)
        .arg((port >> 8) & 0xFF).arg(port & 0xFF);

    const FtpReply portReply = sendCommand(portCommand);
    if (!portReply.isValid() || portReply.code >= 400) {
        if (errorOut) *errorOut = QStringLiteral("PORT failed: %1").arg(portReply.text);
        delete server;
        return false;
    }

    channel->server = server;
    channel->active = true;
    return true;
}

FtpSocket *FtpBackend::finalizeDataChannel(DataChannel *channel, QString *errorOut)
{
    const bool isFtps = m_credentials.ftpsMode == FtpsMode::Explicit;
    FtpSocket *rawSocket = nullptr;

    if (channel->active) {
        if (!channel->server->hasPendingConnections() && !channel->server->waitForNewConnection(15000)) {
            if (errorOut) *errorOut = QStringLiteral(
                "Timed out waiting for the server to connect back (active/PORT mode)");
            delete channel->server;
            channel->server = nullptr;
            return nullptr;
        }
        QTcpSocket *pending = channel->server->nextPendingConnection();
        delete channel->server;
        channel->server = nullptr;

        // SslAcceptingTcpServer hands out a plain QTcpSocket for FTPS
        // (adopted below into an FtpTlsSocket) or a QSslSocket-used-as-
        // TCP otherwise — see its own comment.
        if (isFtps) {
            auto *tlsSocket = new FtpTlsSocket();
            tlsSocket->adopt(pending);
            rawSocket = tlsSocket;
        } else {
            rawSocket = new QtSocketAdapter(pending);
        }
    } else {
        rawSocket = channel->socket;
        channel->socket = nullptr;
    }

    if (m_dataProtected) {
        auto *tlsSocket = dynamic_cast<FtpTlsSocket *>(rawSocket);
        if (!tlsSocket) {
            if (errorOut) *errorOut = QStringLiteral("Data connection setup failed internally");
            delete rawSocket;
            return nullptr;
        }
        // A FRESH TLS handshake per data connection, reusing the control
        // connection's own session (see FtpTlsSocket.h) — this is what
        // actually satisfies a genuinely strict server's RFC 4217
        // anti-hijacking check, confirmed against a real proftpd
        // container (see ARCHITECTURE.md).
        auto *controlTlsSocket = dynamic_cast<FtpTlsSocket *>(m_controlSocket);
        FtpTlsSocket::PeerInfo peerInfo;
        QString tlsError;
        const bool handshakeOk = tlsSocket->handshake(
                controlTlsSocket ? controlTlsSocket->sessionForReuse() : nullptr, &peerInfo, &tlsError)
            && verifyTlsPeer(peerInfo, /*isDataConnection=*/true, &tlsError);
        if (!handshakeOk) {
            if (errorOut) *errorOut = QStringLiteral("Data connection TLS handshake failed: %1").arg(tlsError);
            delete rawSocket;
            return nullptr;
        }
        // Rotates the control connection's own offered session to this
        // data connection's freshly negotiated one — a strict server can
        // (and a real proftpd container's TLSLog confirms does) reject a
        // SECOND data connection's attempt to reuse the same, now-
        // superseded original session even though the FIRST data
        // connection's reuse of that exact session just succeeded, a
        // standard TLS anti-replay session-rotation behavior. Without
        // this, only the first data connection opened after the control
        // handshake would ever successfully satisfy the reuse check.
        if (controlTlsSocket)
            controlTlsSocket->setSessionForReuse(tlsSocket->sessionForReuse());
    }

    return rawSocket;
}

// ---------------------------------------------------------------------
// Directory listing parsing
// ---------------------------------------------------------------------

bool FtpBackend::parseMlsdLine(const QString &line, RemoteEntry *entry)
{
    // Format: "fact1=value1;fact2=value2;...; filename" (RFC 3659) —
    // the filename is everything after the LAST semicolon, which keeps
    // filenames containing spaces intact without needing to guess at a
    // field-count-based split.
    const int lastSemicolon = line.lastIndexOf(QLatin1Char(';'));
    if (lastSemicolon < 0)
        return false;

    const QString factsPart = line.left(lastSemicolon);
    const QString name = line.mid(lastSemicolon + 1).trimmed();
    if (name.isEmpty())
        return false;

    QHash<QString, QString> facts;
    const QStringList factTokens = factsPart.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &token : factTokens) {
        const int eq = token.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        facts.insert(token.left(eq).trimmed().toLower(), token.mid(eq + 1).trimmed());
    }

    const QString type = facts.value(QStringLiteral("type")).toLower();
    // "cdir"/"pdir" are MLSD's equivalents of "." and ".." — not real
    // entries, filtered the same way every other backend already
    // filters those out of its own listings.
    if (type == QStringLiteral("cdir") || type == QStringLiteral("pdir"))
        return false;

    entry->name = name;
    entry->isDir = (type == QStringLiteral("dir"));
    entry->isSymlink = false;   // MLSD's OS.unix=symlink fact is a nonstandard extension, not relied on here
    entry->size = facts.value(QStringLiteral("size")).toLongLong();

    const QString modify = facts.value(QStringLiteral("modify"));   // YYYYMMDDHHMMSS[.sss], always UTC per RFC 3659
    if (modify.length() >= 14) {
        // Converted to local time before storing — LocalBackend
        // (QFileInfo::lastModified()) and SftpBackend
        // (QDateTime::fromSecsSinceEpoch(), which defaults to local
        // time) both display local time, and FilePaneWidget's
        // modified-column rendering just calls toString() on whatever's
        // stored with no per-backend conversion of its own. Without
        // this, MLSD-sourced entries would show the file's real UTC
        // wall-clock time labeled as if it were local (plus a trailing
        // "Z" from Qt::ISODate formatting a UTC-tagged QDateTime) —
        // visibly wrong and inconsistent with the other two panes for
        // the exact same underlying instant. Confirmed directly: an
        // entry timestamped 2025-01-01T00:00:00Z showed as
        // 2025-01-01T00:00:00Z here but 2024-12-31T19:00:00 in
        // Local/SFTP under America/New_York before this fix.
        //
        // Qt::UTC (not QTimeZone::UTC, Qt 6.7+ only) deliberately — this
        // codebase's CI target (ubuntu-latest's packaged qt6-base-dev)
        // ships an older Qt6 without QTimeZone's Initialization enum at
        // all, confirmed the hard way when a first attempt at this fix
        // built fine locally (a newer Qt6) but failed to COMPILE at all
        // in CI ("'UTC' is not a member of 'QTimeZone'") — a real gap
        // between two "Qt6" builds, not a version this codebase can
        // assume. Qt::UTC is deprecated as of newer Qt but still
        // compiles everywhere Qt6 does; the deprecation warning is a
        // fully acceptable trade for that portability.
        entry->modified = QDateTime(
            QDate(modify.mid(0, 4).toInt(), modify.mid(4, 2).toInt(), modify.mid(6, 2).toInt()),
            QTime(modify.mid(8, 2).toInt(), modify.mid(10, 2).toInt(), modify.mid(12, 2).toInt()),
            Qt::UTC).toLocalTime();
    }

    // MLSD's "perm" fact is a compact permission-CAPABILITY code
    // ("adfr", "elcm", ...), not the familiar "rwxr-xr-x" string SFTP
    // provides — not translated here, since the two aren't equivalent
    // representations of the same thing (perm describes what the
    // CURRENT user can do, not the file's actual mode bits). Left as a
    // placeholder; a real, deliberate display limitation for FTP
    // entries, not an oversight.
    entry->permissions = QStringLiteral("-");

    return true;
}

bool FtpBackend::parseListLine(const QString &line, RemoteEntry *entry)
{
    if (line.isEmpty())
        return false;

    // Unix `ls -l`-style — by far the most common LIST format in
    // practice, including from many non-Unix servers that deliberately
    // emulate it for compatibility.
    static const QRegularExpression unixRegex(
        QStringLiteral("^([\\-dlbcps])[\\-rwxsStT]{9}\\+?\\s+\\d+\\s+\\S+\\s+\\S+\\s+(\\d+)\\s+"
                       "\\w{3}\\s+\\d{1,2}\\s+(?:\\d{4}|\\d{1,2}:\\d{2})\\s+(.+)$"));
    QRegularExpressionMatch m = unixRegex.match(line);
    if (m.hasMatch()) {
        const QString typeChar = m.captured(1);
        entry->isDir = (typeChar == QStringLiteral("d"));
        entry->isSymlink = (typeChar == QStringLiteral("l"));
        entry->size = m.captured(2).toLongLong();
        entry->name = m.captured(3);
        // Symlink lines look like "name -> target" — keep just the
        // link's own name, matching how every other backend's listing
        // treats an entry (the thing IN this directory, not what it
        // resolves to).
        const int arrowIndex = entry->name.indexOf(QStringLiteral(" -> "));
        if (arrowIndex >= 0)
            entry->name = entry->name.left(arrowIndex);
        entry->permissions = QStringLiteral("-");   // see this function's limitations note below
        return !entry->name.isEmpty();
    }

    // DOS/Windows IIS style: "01-15-23  10:30AM       <DIR>          name"
    static const QRegularExpression dosRegex(
        QStringLiteral("^\\d{2}-\\d{2}-\\d{2,4}\\s+\\d{2}:\\d{2}(?:AM|PM)\\s+(<DIR>|\\d+)\\s+(.+)$"));
    m = dosRegex.match(line);
    if (m.hasMatch()) {
        const QString sizeOrDir = m.captured(1);
        entry->isDir = (sizeOrDir == QStringLiteral("<DIR>"));
        entry->size = entry->isDir ? 0 : sizeOrDir.toLongLong();
        entry->isSymlink = false;
        entry->name = m.captured(2);
        entry->permissions = QStringLiteral("-");
        return !entry->name.isEmpty();
    }

    // Deliberate limitations of this whole function: (1) modification
    // dates from LIST are not parsed at all (left as an invalid/default
    // QDateTime) — the format is genuinely locale- and server-dependent
    // (month names in different languages, ambiguity between a time and
    // a year for the same field position) in a way that's much higher
    // risk to get subtly wrong than it's worth for a display-only
    // column, especially unverifiable against a real server from this
    // environment; MLSD-sourced entries DO get a real modified date,
    // since that format is actually standardized. (2) A line matching
    // neither pattern (e.g. a leading "total 24" summary line Unix
    // LIST output often starts with) returns false — the caller skips
    // it rather than aborting the whole listing or fabricating a
    // garbage entry.
    return false;
}

QList<RemoteEntry> FtpBackend::listDirectoryInternal(const QString &path, bool *ok, QString *errorOut)
{
    QList<RemoteEntry> entries;
    *ok = false;

    // The data channel MUST be opened (PASV) or announced (PORT) before
    // the listing command per RFC 959's ordering — see
    // openDataChannel()/finalizeDataChannel()'s doc comments for why
    // active mode's finalize step has to come AFTER the command below,
    // unlike PASV's.
    QString dataError;
    DataChannel channel;
    if (!openDataChannel(&channel, &dataError)) {
        if (errorOut) *errorOut = dataError;
        return entries;
    }

    // MLSD first — standardized and unambiguous. Falls back to LIST if
    // the server doesn't understand MLSD at all (500/502, "command not
    // implemented") OR administratively refuses it (550, "action not
    // taken") — found for real, not designed in from a spec reading:
    // this project's own proftpd test container (mod_facts compiled in,
    // MLSD/MLST explicitly denied via a <Limit> block — see
    // tools/local-test-servers/containers/proftpd.conf) replies 550 to
    // a denied MLSD, not 500/502, and until this was caught, FtpBackend
    // would have treated that as a hard listing failure instead of
    // falling back to LIST, on a real, plausible server configuration
    // (MLSD present but disabled), not just a hypothetical one. Not
    // treated as a fallback trigger for any OTHER kind of failure — a
    // fresh data channel is needed for the retry, since the one just
    // opened was tied to the now-rejected MLSD attempt.
    FtpReply listReply = sendCommand(QStringLiteral("MLSD %1").arg(path));
    bool usingMlsd = true;
    if (!listReply.isValid() || listReply.code == 500 || listReply.code == 502
        || listReply.code == 550) {
        usingMlsd = false;
        delete channel.socket;
        delete channel.server;
        channel = DataChannel{};
        if (!openDataChannel(&channel, &dataError)) {
            if (errorOut) *errorOut = dataError;
            return entries;
        }
        listReply = sendCommand(QStringLiteral("LIST %1").arg(path));
    }

    if (!listReply.isValid() || (listReply.code != 150 && listReply.code != 125)) {
        if (errorOut) *errorOut = QStringLiteral("Cannot list %1: %2").arg(path, listReply.text);
        delete channel.socket;
        delete channel.server;
        return entries;
    }

    FtpSocket *dataSocket = finalizeDataChannel(&channel, &dataError);
    if (!dataSocket) {
        if (errorOut) *errorOut = dataError;
        return entries;
    }

    // Polls the data connection in short slices (rather than one 15s
    // wait) so the control connection can also be checked in between,
    // AND so a real, root-caused gap can be worked around: a real
    // vsftpd's FTPS data connection was confirmed (strace on the server
    // side, against a live container) to send the complete listing and
    // then close the raw TCP connection WITHOUT a TLS close_notify alert
    // first. QSslSocket never surfaces that as a state change or a
    // distinguishable error — state() stays ConnectedState and
    // waitForReadyRead() just times out (SocketTimeoutError) forever,
    // even though every byte already arrived; downloadFile()'s own
    // version of this loop has a precise fix available (the server's own
    // SIZE reply gives an exact expected byte count), but a directory
    // listing has no equivalent size to check against. Once any data has
    // actually arrived, one full additional idle poll with nothing more
    // coming is treated as "done" — safe here specifically because a
    // real directory listing is always small and arrives essentially all
    // at once (confirmed directly, not assumed), unlike a large file
    // transfer where a legitimate multi-second pause is plausible and
    // this same heuristic would be the wrong trade.
    QByteArray raw;
    qint64 waitedMs = 0;
    constexpr qint64 pollIntervalMs = 1000;
    constexpr qint64 totalTimeoutMs = 15000;
    for (;;) {
        if (!dataSocket->isConnected()) {
            raw.append(dataSocket->readAll());
            break;
        }
        if (dataSocket->waitForReadyRead(pollIntervalMs)) {
            raw.append(dataSocket->readAll());
            continue;
        }
        if (!raw.isEmpty())
            break;   // already got the (necessarily small) listing; nothing more is coming
        if (m_controlSocket && m_controlSocket->waitForReadyRead(0))
            break;   // a decisive reply already arrived — readReply() below will report it
        if (!dataSocket->isConnected())
            continue;   // loop once more to drain and hit the branch above
        waitedMs += pollIntervalMs;
        if (waitedMs >= totalTimeoutMs) {
            if (errorOut) *errorOut = QStringLiteral("Timed out waiting for directory listing");
            delete dataSocket;
            return entries;
        }
    }
    delete dataSocket;

    const FtpReply finalReply = readReply();
    if (!finalReply.isValid() || finalReply.code >= 400) {
        if (errorOut) *errorOut = QStringLiteral("Listing failed: %1").arg(finalReply.text);
        return entries;
    }

    const QStringList lines = QString::fromUtf8(raw).split(QStringLiteral("\r\n"), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        RemoteEntry entry;
        const bool parsed = usingMlsd ? parseMlsdLine(line, &entry) : parseListLine(line, &entry);
        if (parsed)
            entries.append(entry);
        // Unparseable lines (a "total N" summary line, something
        // matching neither known LIST format) are skipped, not treated
        // as a listing failure — see parseListLine()'s own comment.
    }

    *ok = true;
    return entries;
}

// ---------------------------------------------------------------------
// RemoteBackend interface
// ---------------------------------------------------------------------

void FtpBackend::connectToHost()
{
    ensureConnected();
}

void FtpBackend::listDirectory(const QString &path)
{
    if (!ensureConnected())
        return;

    const QString target = path.isEmpty() ? m_currentPath : path;
    bool ok = false;
    QString error;
    const QList<RemoteEntry> entries = listDirectoryInternal(target, &ok, &error);
    if (!ok) {
        emit connectionFailed(error);
        return;
    }

    {
        QMutexLocker locker(&m_currentPathMutex);
        m_currentPath = target;
    }
    emit directoryListed(target, entries);
}

void FtpBackend::listDirectoryForEnumeration(const QString &path, int requestId)
{
    if (!ensureConnected())
        return;

    bool ok = false;
    QString error;
    const QList<RemoteEntry> entries = listDirectoryInternal(path, &ok, &error);
    if (!ok) {
        emit enumerationFailed(path, error, requestId);
        return;
    }
    emit directoryEnumerated(path, entries, requestId);
}

void FtpBackend::checkExists(const QString &path, int requestId)
{
    if (!ensureConnected()) {
        emit existsChecked(path, false, false, requestId);
        return;
    }

    // FTP has no single command that cleanly reports existence AND type
    // for both files and directories (SIZE works for files on most
    // servers but commonly errors on directories) — listing the parent
    // and checking membership reuses the same, already-tested listing
    // path instead of adding a second, less certain code path.
    const int lastSlash = path.lastIndexOf(QLatin1Char('/'));
    const QString parentPath = lastSlash > 0 ? path.left(lastSlash) : QStringLiteral("/");
    const QString name = path.mid(lastSlash + 1);

    bool ok = false;
    QString error;
    const QList<RemoteEntry> entries = listDirectoryInternal(parentPath, &ok, &error);
    if (!ok) {
        // Can't list the parent — treat as "doesn't exist" rather than
        // failing the whole operation; matches SftpBackend::checkExists()'s
        // same reasoning (a stat/list failure almost always does mean
        // not-found for our purposes here).
        emit existsChecked(path, false, false, requestId);
        return;
    }

    for (const RemoteEntry &entry : entries) {
        if (entry.name == name) {
            emit existsChecked(path, true, entry.isDir, requestId);
            return;
        }
    }
    emit existsChecked(path, false, false, requestId);
}

void FtpBackend::downloadFile(const QString &remotePath, const QString &localPath, qint64 resumeOffset)
{
    if (!ensureConnected())
        return;

    m_cancelRequested.storeRelaxed(false);
    m_pauseRequested.storeRelaxed(false);

    QFile out(localPath);
    const QIODevice::OpenMode openMode = resumeOffset > 0
        ? QIODevice::ReadWrite
        : (QIODevice::WriteOnly | QIODevice::Truncate);
    if (!out.open(openMode)) {
        emit transferFailed(remotePath, QStringLiteral("Cannot open local file for write: %1").arg(localPath));
        return;
    }

    // Same offset-clamping reasoning as SftpBackend::downloadFile(): trust
    // what's actually on disk over a possibly-stale requested offset.
    qint64 effectiveOffset = 0;
    if (resumeOffset > 0) {
        effectiveOffset = qMin(resumeOffset, out.size());
        if (out.size() > effectiveOffset)
            out.resize(effectiveOffset);
        out.seek(effectiveOffset);
    }

    // SIZE for progress reporting — reliable in binary (TYPE I) mode per
    // RFC 3659; this app always uses binary mode (see ensureConnected()).
    qint64 totalSize = 0;
    const FtpReply sizeReply = sendCommand(QStringLiteral("SIZE %1").arg(remotePath));
    if (sizeReply.isValid() && sizeReply.code == 213)
        totalSize = sizeReply.text.trimmed().toLongLong();

    // Data channel before REST/RETR — same command-ordering requirement
    // as listDirectoryInternal().
    QString dataError;
    DataChannel channel;
    if (!openDataChannel(&channel, &dataError)) {
        emit transferFailed(remotePath, dataError);
        return;
    }

    if (effectiveOffset > 0) {
        const FtpReply restReply = sendCommand(QStringLiteral("REST %1").arg(effectiveOffset));
        if (!restReply.isValid() || restReply.code != 350) {
            emit transferFailed(remotePath, QStringLiteral("Server refused REST (resume): %1").arg(restReply.text));
            delete channel.socket;
            delete channel.server;
            return;
        }
    }

    const FtpReply retrReply = sendCommand(QStringLiteral("RETR %1").arg(remotePath));
    if (!retrReply.isValid() || (retrReply.code != 150 && retrReply.code != 125)) {
        emit transferFailed(remotePath, QStringLiteral("RETR failed: %1").arg(retrReply.text));
        delete channel.socket;
        delete channel.server;
        return;
    }

    FtpSocket *dataSocket = finalizeDataChannel(&channel, &dataError);
    if (!dataSocket) {
        emit transferFailed(remotePath, dataError);
        return;
    }

    qint64 done = effectiveOffset;
    bool cancelled = false;
    bool paused = false;
    char buf[256 * 1024];
    BandwidthThrottle throttle(m_credentials.bandwidthLimitKBps);

    for (;;) {
        if (m_cancelRequested.loadRelaxed()) { cancelled = true; break; }
        if (m_pauseRequested.loadRelaxed()) { paused = true; break; }

        if (!dataSocket->isConnected() && dataSocket->bytesAvailable() == 0)
            break;   // server closed the data connection — transfer complete

        // Known-size early exit — found for real, not a defensive
        // nicety: a real vsftpd's data connection was confirmed (via
        // strace on the server side) to send the complete file and then
        // close the raw TCP connection WITHOUT a TLS close_notify alert
        // first. QSslSocket never surfaces that as a state change or a
        // distinguishable error here — state() stays ConnectedState and
        // waitForReadyRead() below just times out (SocketTimeoutError)
        // forever, even though every byte already arrived. Once we've
        // received at least as many bytes as the server's own SIZE
        // reply promised, the transfer genuinely is complete regardless
        // of what the connection's close behavior does or doesn't
        // signal — closing our own end here (below, after the loop) is
        // also what unblocks the server's own pending final reply, per
        // the same strace session.
        if (totalSize > 0 && done >= totalSize)
            break;

        // Short (1s) poll rather than one long wait — lets the
        // cancel/pause checks above actually run periodically during a
        // stalled transfer, not just between chunks. No overall timeout
        // here (unlike listDirectoryInternal()'s bounded wait) — a large,
        // legitimately slow transfer can stall for a while without that
        // meaning anything is wrong, so this keeps polling indefinitely
        // UNLESS the control connection has something to say (checked
        // below), which during an active transfer should never happen
        // except when it genuinely matters.
        if (!dataSocket->waitForReadyRead(1000) && dataSocket->bytesAvailable() == 0) {
            if (!dataSocket->isConnected())
                break;
            // Found for real (strace against a live vsftpd container,
            // require_ssl_reuse=YES): the server can reject the data
            // connection's failure to demonstrate TLS session reuse by
            // reporting it over the CONTROL channel — "522 SSL connection
            // failed: session reuse required" — arriving anywhere from
            // seconds to (confirmed directly) several minutes after the
            // data connection's own handshake completes, having sent
            // nothing at all on the data connection itself and never
            // transitioning it out of ConnectedState either — see
            // ARCHITECTURE.md's Known Gaps for why that delay is so
            // unpredictable (a real vsftpd-side stall this project's own
            // container ships worked around rather than papered over).
            // Without this check, downloadFile() had no way to ever
            // notice that reply and would poll the silent data socket
            // forever, however long the server eventually took.
            if (m_controlSocket && m_controlSocket->waitForReadyRead(0))
                break;
            continue;
        }

        const qint64 n = dataSocket->read(buf, sizeof(buf));
        if (n <= 0)
            continue;

        out.write(buf, n);
        done += n;
        emit transferProgress(remotePath, done, totalSize > 0 ? totalSize : done);

        // Same reasoning as SftpBackend::downloadFile()'s identical
        // call — see that comment.
        throttle.pace(done - effectiveOffset, [this]() {
            return m_cancelRequested.loadRelaxed() || m_pauseRequested.loadRelaxed();
        });
    }

    out.close();
    dataSocket->closeAbruptly();
    delete dataSocket;

    if (paused) {
        emit transferPaused(remotePath, done);
        return;
    }
    if (cancelled) {
        emit transferFailed(remotePath, QStringLiteral("Cancelled"));
        // Drain the final control-connection reply the server still
        // sends after the data connection closes early — leaving it
        // unread would corrupt the NEXT command's reply parsing.
        readReply();
        return;
    }

    const FtpReply finalReply = readReply();
    if (finalReply.isValid() && finalReply.code < 400)
        emit transferFinished(remotePath);
    else
        emit transferFailed(remotePath, QStringLiteral("Transfer did not complete cleanly: %1").arg(finalReply.text));
}

void FtpBackend::uploadFile(const QString &localPath, const QString &remotePath, qint64 resumeOffset)
{
    if (!ensureConnected())
        return;

    m_cancelRequested.storeRelaxed(false);
    m_pauseRequested.storeRelaxed(false);

    QFile in(localPath);
    if (!in.open(QIODevice::ReadOnly)) {
        emit transferFailed(localPath, QStringLiteral("Cannot open local file: %1").arg(localPath));
        return;
    }

    qint64 effectiveOffset = 0;
    if (resumeOffset > 0) {
        // Same clamping reasoning as SftpBackend::uploadFile(): don't
        // seek past EOF if the local source shrank since the pause.
        effectiveOffset = qMin(resumeOffset, in.size());
        in.seek(effectiveOffset);
    }

    QString dataError;
    DataChannel channel;
    if (!openDataChannel(&channel, &dataError)) {
        emit transferFailed(localPath, dataError);
        return;
    }

    if (effectiveOffset > 0) {
        const FtpReply restReply = sendCommand(QStringLiteral("REST %1").arg(effectiveOffset));
        if (!restReply.isValid() || restReply.code != 350) {
            emit transferFailed(localPath, QStringLiteral("Server refused REST (resume): %1").arg(restReply.text));
            delete channel.socket;
            delete channel.server;
            return;
        }
    }

    const FtpReply storReply = sendCommand(QStringLiteral("STOR %1").arg(remotePath));
    if (!storReply.isValid() || (storReply.code != 150 && storReply.code != 125)) {
        emit transferFailed(localPath, QStringLiteral("STOR failed: %1").arg(storReply.text));
        delete channel.socket;
        delete channel.server;
        return;
    }

    FtpSocket *dataSocket = finalizeDataChannel(&channel, &dataError);
    if (!dataSocket) {
        emit transferFailed(localPath, dataError);
        return;
    }

    const qint64 totalSize = in.size();
    qint64 done = effectiveOffset;
    bool cancelled = false;
    bool paused = false;
    char buf[256 * 1024];
    BandwidthThrottle throttle(m_credentials.bandwidthLimitKBps);

    while (!in.atEnd()) {
        if (m_cancelRequested.loadRelaxed()) { cancelled = true; break; }
        if (m_pauseRequested.loadRelaxed()) { paused = true; break; }

        const qint64 n = in.read(buf, sizeof(buf));
        if (n <= 0)
            break;

        qint64 written = 0;
        while (written < n) {
            const qint64 w = dataSocket->write(buf + written, n - written);
            if (w < 0) {
                const QString writeError = dataSocket->errorString();
                dataSocket->closeAbruptly();
                delete dataSocket;
                emit transferFailed(localPath, QStringLiteral("Write error during transfer: %1").arg(writeError));
                return;
            }
            written += w;
            // Ensures the write actually leaves this process rather than
            // sitting in Qt's internal buffer indefinitely.
            dataSocket->waitForBytesWritten(15000);
        }
        done += n;
        emit transferProgress(localPath, done, totalSize);

        // Same reasoning as SftpBackend::downloadFile()'s identical
        // call — see that comment.
        throttle.pace(done - effectiveOffset, [this]() {
            return m_cancelRequested.loadRelaxed() || m_pauseRequested.loadRelaxed();
        });
    }

    // disconnectFromHost() (graceful — flushes and sends a proper TLS
    // close_notify before the underlying TCP connection actually closes),
    // NOT close() (abrupt — found for real, not assumed: a plain close()
    // here against a real vsftpd container made every otherwise-correct
    // upload fail with "Failure reading network stream", vsftpd's own
    // error for a client that stopped writing without a clean TLS-level
    // shutdown — the same class of problem as this project's own FTPS
    // vendor-container work found on the download side, just triggered
    // from the opposite direction; see ARCHITECTURE.md's Known Gaps).
    dataSocket->disconnectGracefully();
    dataSocket->waitForFullyDisconnected(3000);
    delete dataSocket;

    if (paused) {
        emit transferPaused(localPath, done);
        return;
    }
    if (cancelled) {
        emit transferFailed(localPath, QStringLiteral("Cancelled"));
        readReply();   // drain the final reply, same reasoning as downloadFile()
        return;
    }

    const FtpReply finalReply = readReply();
    if (finalReply.isValid() && finalReply.code < 400)
        emit transferFinished(localPath);
    else
        emit transferFailed(localPath, QStringLiteral("Transfer did not complete cleanly: %1").arg(finalReply.text));
}

void FtpBackend::deleteEntry(const QString &path, bool isDirectory)
{
    if (!ensureConnected())
        return;

    // RMD is empty-only per FTP semantics on essentially every server —
    // matches RemoteBackend::deleteEntry()'s documented non-recursive
    // contract without this function needing to do anything extra to
    // enforce it.
    const FtpReply reply = isDirectory
        ? sendCommand(QStringLiteral("RMD %1").arg(path))
        : sendCommand(QStringLiteral("DELE %1").arg(path));

    if (!reply.isValid() || reply.code >= 400) {
        emit fileOperationFailed(QStringLiteral("Delete"), path,
            reply.isValid() ? reply.text : QStringLiteral("No response from server"));
        return;
    }
    listDirectory(m_currentPath);
}

bool FtpBackend::performRename(const QString &oldPath, const QString &newPath, QString *errorReason)
{
    if (!ensureConnected()) {
        if (errorReason)
            *errorReason = QStringLiteral("Not connected");
        return false;
    }

    const FtpReply rnfrReply = sendCommand(QStringLiteral("RNFR %1").arg(oldPath));
    if (!rnfrReply.isValid() || rnfrReply.code != 350) {
        if (errorReason)
            *errorReason = rnfrReply.isValid() ? rnfrReply.text : QStringLiteral("No response from server");
        return false;
    }

    // Unlike SftpBackend's libssh2_sftp_rename() (OVERWRITE flag baked
    // in), FTP's RNTO has no standardized overwrite behavior — whether a
    // pre-existing destination is silently replaced or rejected is
    // server-dependent. No pre-emptive DELE/RMD attempt is made here
    // (moveEntry() doesn't know whether the destination, if any, is a
    // file or directory), so a server that refuses to overwrite will
    // surface as a normal failure with its own reply text — an honest
    // limitation, not silently papered over. Matches this project's
    // existing acknowledgment that "not every server honors" requested
    // overwrite-on-conflict behavior (see SftpBackend::performRename()'s
    // own comment).
    const FtpReply rntoReply = sendCommand(QStringLiteral("RNTO %1").arg(newPath));
    if (!rntoReply.isValid() || rntoReply.code >= 400) {
        if (errorReason)
            *errorReason = rntoReply.isValid() ? rntoReply.text : QStringLiteral("No response from server");
        return false;
    }
    return true;
}

void FtpBackend::renameEntry(const QString &oldPath, const QString &newPath)
{
    QString error;
    if (!performRename(oldPath, newPath, &error)) {
        emit fileOperationFailed(QStringLiteral("Rename"), oldPath, error);
        return;
    }
    listDirectory(m_currentPath);
}

void FtpBackend::moveEntry(const QString &oldPath, const QString &newPath, int requestId)
{
    QString error;
    if (!performRename(oldPath, newPath, &error)) {
        emit entryMoveFailed(error, requestId);
        return;
    }
    emit entryMoved(requestId);
}

QString FtpBackend::connectionIdentity() const
{
    return QStringLiteral("ftp://%1@%2:%3")
        .arg(m_credentials.username, m_credentials.host)
        .arg(m_credentials.port);
}

void FtpBackend::createDirectory(const QString &path)
{
    if (!ensureConnected())
        return;

    const FtpReply reply = sendCommand(QStringLiteral("MKD %1").arg(path));
    if (!reply.isValid() || reply.code >= 400) {
        emit fileOperationFailed(QStringLiteral("Create folder"), path,
            reply.isValid() ? reply.text : QStringLiteral("No response from server"));
        return;
    }
    listDirectory(m_currentPath);
}

void FtpBackend::createFile(const QString &path)
{
    if (!ensureConnected())
        return;

    // FTP has no exclusive-create flag the way SFTP's LIBSSH2_FXF_EXCL
    // does — checked explicitly first so "already exists" is reported
    // as a specific error rather than silently overwriting whatever's
    // there (matches every other backend's createFile() contract).
    bool existsOk = false;
    QString existsError;
    const int lastSlash = path.lastIndexOf(QLatin1Char('/'));
    const QString parentPath = lastSlash > 0 ? path.left(lastSlash) : QStringLiteral("/");
    const QString name = path.mid(lastSlash + 1);
    const QList<RemoteEntry> entries = listDirectoryInternal(parentPath, &existsOk, &existsError);
    if (existsOk) {
        for (const RemoteEntry &entry : entries) {
            if (entry.name == name) {
                emit fileOperationFailed(QStringLiteral("Create file"), path,
                                          QStringLiteral("Something with that name already exists"));
                return;
            }
        }
    }

    QString dataError;
    DataChannel channel;
    if (!openDataChannel(&channel, &dataError)) {
        emit fileOperationFailed(QStringLiteral("Create file"), path, dataError);
        return;
    }

    const FtpReply storReply = sendCommand(QStringLiteral("STOR %1").arg(path));
    if (!storReply.isValid() || (storReply.code != 150 && storReply.code != 125)) {
        emit fileOperationFailed(QStringLiteral("Create file"), path,
            storReply.isValid() ? storReply.text : QStringLiteral("No response from server"));
        delete channel.socket;
        delete channel.server;
        return;
    }

    FtpSocket *dataSocket = finalizeDataChannel(&channel, &dataError);
    if (!dataSocket) {
        emit fileOperationFailed(QStringLiteral("Create file"), path, dataError);
        return;
    }

    // Zero bytes written — an open-then-immediately-close data
    // connection is what "empty file" means over FTP, matching
    // SftpBackend::createFile()'s open-then-close-without-writing.
    dataSocket->closeAbruptly();
    delete dataSocket;

    const FtpReply finalReply = readReply();
    if (!finalReply.isValid() || finalReply.code >= 400) {
        emit fileOperationFailed(QStringLiteral("Create file"), path,
            finalReply.isValid() ? finalReply.text : QStringLiteral("No response from server"));
        return;
    }
    listDirectory(m_currentPath);
}

void FtpBackend::setPermissions(const QString &path, int mode)
{
    if (!ensureConnected())
        return;

    // SITE CHMOD is a widely-supported (vsftpd, proftpd) but
    // non-standard FTP extension — RFC 959 has no chmod equivalent at
    // all. A server that doesn't support it typically replies 502
    // (command not implemented), already >= 400 here, so it surfaces
    // as an ordinary fileOperationFailed like any other rejected
    // command, not a special "unsupported" case.
    const FtpReply reply = sendCommand(
        QStringLiteral("SITE CHMOD %1 %2").arg(QString::number(mode, 8), path));

    if (!reply.isValid() || reply.code >= 400) {
        emit fileOperationFailed(QStringLiteral("Change permissions"), path,
            reply.isValid() ? reply.text : QStringLiteral("No response from server"));
        return;
    }
    listDirectory(m_currentPath);
}
