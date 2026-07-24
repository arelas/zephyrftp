#include "SftpBackend.h"
#include "../ui/HostKeyVerifier.h"

#include <QFile>
#include <QFileInfo>
#include <QTcpSocket>
#include <QStandardPaths>
#include <QDir>
#include <QMetaObject>

namespace {
// libssh2_session_hostkey()'s type constant (LIBSSH2_HOSTKEY_TYPE_*) and
// the knownhost API's key-type mask bits (LIBSSH2_KNOWNHOST_KEY_*) are NOT
// the same numbering — confirmed directly against libssh2.h rather than
// assumed a naive cast/shift would line up (it doesn't: RSA is type 1 but
// KEY_SSHRSA is (2<<18), off by one across the board). This maps between
// them explicitly.
int hostkeyTypeToKnownhostKeyMask(int hostkeyType)
{
    switch (hostkeyType) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS:       return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   return LIBSSH2_KNOWNHOST_KEY_ED25519;
    default:                              return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

QString knownHostsFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/known_hosts");
}
}

SftpBackend::SftpBackend(SftpCredentials credentials, HostKeyVerifier *hostKeyVerifier,
                          QObject *parent)
    : RemoteBackend(parent)
    , m_credentials(std::move(credentials))
    , m_hostKeyVerifier(hostKeyVerifier)
{
}

SftpBackend::~SftpBackend()
{
    teardown();
}

void SftpBackend::teardown()
{
    if (m_sftp) {
        libssh2_sftp_shutdown(m_sftp);
        m_sftp = nullptr;
    }
    if (m_session) {
        libssh2_session_disconnect(m_session, "client shutdown");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
    if (m_socket) {
        m_socket->disconnectFromHost();
        delete m_socket;
        m_socket = nullptr;
    }
}

bool SftpBackend::askUserToTrustHostKey(const QString &fingerprint, bool isMismatch)
{
    if (!m_hostKeyVerifier) {
        // Fails safe: with no way to ask a person, refuse rather than
        // silently trust. This should never happen in practice (MainWindow
        // always wires one up), but if it did, silently trusting an
        // unknown host key would defeat the entire point of this feature.
        return false;
    }

    bool accepted = false;
    QMetaObject::invokeMethod(m_hostKeyVerifier, "confirmHostKey", Qt::BlockingQueuedConnection,
                               Q_RETURN_ARG(bool, accepted),
                               Q_ARG(QString, m_credentials.host),
                               Q_ARG(QString, fingerprint),
                               Q_ARG(bool, isMismatch));
    return accepted;
}

bool SftpBackend::verifyHostKey()
{
    LIBSSH2_KNOWNHOSTS *knownHosts = libssh2_knownhost_init(m_session);
    if (!knownHosts) {
        emit connectionFailed(QStringLiteral("Could not initialize known-hosts store"));
        return false;
    }

    const QString knownHostsPath = knownHostsFilePath();
    // Best-effort load: a missing/empty file just means every host is
    // currently unknown, which is the expected state on first run —
    // libssh2_knownhost_readfile()'s return value is intentionally not
    // treated as fatal here.
    libssh2_knownhost_readfile(knownHosts, knownHostsPath.toUtf8().constData(),
                                LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    size_t keyLen = 0;
    int keyType = 0;
    const char *keyData = libssh2_session_hostkey(m_session, &keyLen, &keyType);
    if (!keyData) {
        libssh2_knownhost_free(knownHosts);
        emit connectionFailed(QStringLiteral("Could not retrieve server host key"));
        return false;
    }

    const int typemask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW
                          | hostkeyTypeToKnownhostKeyMask(keyType);

    struct libssh2_knownhost *matchedEntry = nullptr;
    const int check = libssh2_knownhost_checkp(knownHosts, m_credentials.host.toUtf8().constData(),
                                                m_credentials.port, keyData, keyLen, typemask,
                                                &matchedEntry);

    // SHA256 fingerprint, base64-displayed the way OpenSSH shows it
    // ("SHA256:xxxx") — raw key bytes aren't something a person can
    // meaningfully verify, but this format is exactly what's printed on
    // the server side by `ssh-keygen -lf` for comparison.
    QString fingerprint;
    if (const char *hashRaw = libssh2_hostkey_hash(m_session, LIBSSH2_HOSTKEY_HASH_SHA256)) {
        fingerprint = QStringLiteral("SHA256:")
            + QByteArray(hashRaw, 32).toBase64(QByteArray::OmitTrailingEquals);
    }

    bool trusted = false;
    switch (check) {
    case LIBSSH2_KNOWNHOST_CHECK_MATCH:
        trusted = true;
        break;

    case LIBSSH2_KNOWNHOST_CHECK_MISMATCH:
        trusted = askUserToTrustHostKey(fingerprint, /*isMismatch=*/true);
        if (trusted) {
            // User explicitly accepted a changed key — update the stored
            // entry to match, same as accepting a host-key change in any
            // other SSH client would.
            if (matchedEntry)
                libssh2_knownhost_del(knownHosts, matchedEntry);
            libssh2_knownhost_addc(knownHosts, m_credentials.host.toUtf8().constData(), nullptr,
                                    keyData, keyLen, nullptr, 0, typemask, nullptr);
            libssh2_knownhost_writefile(knownHosts, knownHostsPath.toUtf8().constData(),
                                        LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        }
        break;

    case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND:
        trusted = askUserToTrustHostKey(fingerprint, /*isMismatch=*/false);
        if (trusted) {
            libssh2_knownhost_addc(knownHosts, m_credentials.host.toUtf8().constData(), nullptr,
                                    keyData, keyLen, nullptr, 0, typemask, nullptr);
            libssh2_knownhost_writefile(knownHosts, knownHostsPath.toUtf8().constData(),
                                        LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        }
        break;

    case LIBSSH2_KNOWNHOST_CHECK_FAILURE:
    default:
        trusted = false;
        break;
    }

    libssh2_knownhost_free(knownHosts);

    if (!trusted) {
        emit connectionFailed(QStringLiteral("Host key for %1 was not trusted — connection refused")
                               .arg(m_credentials.host));
    }
    return trusted;
}

bool SftpBackend::ensureSession()
{
    if (m_session && m_sftp)
        return true;

    if (libssh2_init(0) != 0) {
        emit connectionFailed(QStringLiteral("libssh2_init failed"));
        return false;
    }

    // QTcpSocket handles DNS resolution and connection setup portably —
    // this replaced a hand-rolled getaddrinfo()/socket()/connect() block
    // that only ever compiled on POSIX (used sys/socket.h, netinet/in.h,
    // etc., none of which exist on MSVC). libssh2 just needs the native
    // socket descriptor once the connection is up.
    m_socket = new QTcpSocket();
    m_socket->connectToHost(m_credentials.host, static_cast<quint16>(m_credentials.port));
    if (!m_socket->waitForConnected(10000)) {
        emit connectionFailed(QStringLiteral("TCP connect to %1:%2 failed: %3")
                               .arg(m_credentials.host).arg(m_credentials.port)
                               .arg(m_socket->errorString()));
        delete m_socket;
        m_socket = nullptr;
        return false;
    }

    const libssh2_socket_t sock = static_cast<libssh2_socket_t>(m_socket->socketDescriptor());

    m_session = libssh2_session_init();
    if (!m_session) {
        emit connectionFailed(QStringLiteral("libssh2_session_init failed"));
        return false;
    }

    if (libssh2_session_handshake(m_session, sock) != 0) {
        emit connectionFailed(QStringLiteral("SSH handshake failed"));
        return false;
    }

    if (!verifyHostKey()) {
        // connectionFailed already emitted inside verifyHostKey() with a
        // specific reason (rejected / unverifiable / no verifier wired up).
        return false;
    }

    if (m_credentials.authMethod == SftpAuthMethod::Password) {
        if (libssh2_userauth_password(m_session, m_credentials.username.toUtf8().constData(),
                                       m_credentials.password.toUtf8().constData()) != 0) {
            emit connectionFailed(QStringLiteral("Password authentication failed for %1")
                                   .arg(m_credentials.username));
            return false;
        }
    } else {
        // Public-key auth. libssh2_userauth_publickey_fromfile_ex (the
        // underlying call) wants an actual public-key file, not just the
        // private key — falls back to the conventional <privatekey>.pub
        // sibling if the person didn't specify one separately.
        //
        // UNVERIFIED: assumes that .pub-sibling convention holds for
        // whatever key file is provided. If it doesn't, this fails with a
        // libssh2 auth error rather than silently doing something
        // unexpected — no real SFTP server has exercised this path yet.
        const QString publicKeyPath = m_credentials.privateKeyPath + QStringLiteral(".pub");
        const QByteArray passphraseUtf8 = m_credentials.passphrase.toUtf8();
        const char *passphrasePtr = passphraseUtf8.isEmpty() ? nullptr : passphraseUtf8.constData();

        const int rc = libssh2_userauth_publickey_fromfile(
            m_session,
            m_credentials.username.toUtf8().constData(),
            publicKeyPath.toUtf8().constData(),
            m_credentials.privateKeyPath.toUtf8().constData(),
            passphrasePtr);
        if (rc != 0) {
            emit connectionFailed(QStringLiteral("Public-key authentication failed for %1")
                                   .arg(m_credentials.username));
            return false;
        }
    }

    m_sftp = libssh2_sftp_init(m_session);
    if (!m_sftp) {
        emit connectionFailed(QStringLiteral("SFTP subsystem init failed"));
        return false;
    }

    // Resolve the server's default directory for this connection — almost
    // always the user's home directory — rather than hardcoding "/".
    // Matches the convention other SFTP clients (FileZilla, WinSCP) follow.
    char realPathBuf[1024];
    const int realPathLen = libssh2_sftp_realpath(m_sftp, ".", realPathBuf, sizeof(realPathBuf) - 1);
    if (realPathLen > 0) {
        m_currentPath = QString::fromUtf8(realPathBuf, realPathLen);
    }
    // Not fatal if this fails — m_currentPath's default member initializer
    // ("/") stands in, which still works, just starts at the root.

    return true;
}

void SftpBackend::connectToHost()
{
    if (ensureSession())
        emit connected();
}

void SftpBackend::listDirectory(const QString &path)
{
    if (!ensureSession())
        return;

    const QString target = path.isEmpty() ? m_currentPath : path;
    LIBSSH2_SFTP_HANDLE *handle =
        libssh2_sftp_opendir(m_sftp, target.toUtf8().constData());
    if (!handle) {
        emit connectionFailed(QStringLiteral("Cannot open remote directory: %1").arg(target));
        return;
    }

    QList<RemoteEntry> entries;
    char nameBuf[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    int rc = 0;
    while ((rc = libssh2_sftp_readdir(handle, nameBuf, sizeof(nameBuf), &attrs)) > 0) {
        const QString name = QString::fromUtf8(nameBuf, rc);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;

        RemoteEntry e;
        e.name = name;
        e.isDir = LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        e.size = static_cast<qint64>(attrs.filesize);
        e.modified = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(attrs.mtime));

        // Render permission bits the same way `ls -l` does — decode once
        // here rather than pushing raw octal up into the model/view.
        // Literal octal masks instead of sys/stat.h's S_IRUSR-style macros:
        // those are POSIX-only (don't exist on MSVC), but the bits
        // themselves are a fixed SFTP wire-protocol format, not tied to
        // the host OS's stat.h.
        char perms[10] = "---------";
        const quint32 m = attrs.permissions;
        if (m & 0400) perms[0] = 'r';
        if (m & 0200) perms[1] = 'w';
        if (m & 0100) perms[2] = 'x';
        if (m & 0040) perms[3] = 'r';
        if (m & 0020) perms[4] = 'w';
        if (m & 0010) perms[5] = 'x';
        if (m & 0004) perms[6] = 'r';
        if (m & 0002) perms[7] = 'w';
        if (m & 0001) perms[8] = 'x';
        e.permissions = QString::fromLatin1(perms, 9);

        entries.append(e);
    }
    libssh2_sftp_closedir(handle);

    m_currentPath = target;
    emit directoryListed(m_currentPath, entries);
}

void SftpBackend::downloadFile(const QString &remotePath, const QString &localPath)
{
    if (!ensureSession())
        return;

    LIBSSH2_SFTP_HANDLE *handle =
        libssh2_sftp_open(m_sftp, remotePath.toUtf8().constData(), LIBSSH2_FXF_READ, 0);
    if (!handle) {
        emit transferFailed(remotePath, QStringLiteral("Cannot open remote file"));
        return;
    }

    QFile out(localPath);
    if (!out.open(QIODevice::WriteOnly)) {
        libssh2_sftp_close(handle);
        emit transferFailed(remotePath, QStringLiteral("Cannot open local file for write: %1").arg(localPath));
        return;
    }

    LIBSSH2_SFTP_ATTRIBUTES attrs;
    libssh2_sftp_fstat(handle, &attrs);
    const qint64 totalSize = static_cast<qint64>(attrs.filesize);
    qint64 done = 0;

    char buf[32 * 1024];
    ssize_t n = 0;
    while ((n = libssh2_sftp_read(handle, buf, sizeof(buf))) > 0) {
        out.write(buf, n);
        done += n;
        emit transferProgress(remotePath, done, totalSize);
    }

    out.close();
    libssh2_sftp_close(handle);

    if (n < 0)
        emit transferFailed(remotePath, QStringLiteral("Read error during transfer"));
    else
        emit transferFinished(remotePath);
}

void SftpBackend::uploadFile(const QString &localPath, const QString &remotePath)
{
    if (!ensureSession())
        return;

    QFile in(localPath);
    if (!in.open(QIODevice::ReadOnly)) {
        emit transferFailed(localPath, QStringLiteral("Cannot open local file: %1").arg(localPath));
        return;
    }

    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open(
        m_sftp, remotePath.toUtf8().constData(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!handle) {
        emit transferFailed(localPath, QStringLiteral("Cannot open remote file for write"));
        return;
    }

    const qint64 totalSize = in.size();
    qint64 done = 0;
    char buf[32 * 1024];

    while (!in.atEnd()) {
        qint64 n = in.read(buf, sizeof(buf));
        if (n <= 0)
            break;

        qint64 written = 0;
        while (written < n) {
            ssize_t w = libssh2_sftp_write(handle, buf + written, n - written);
            if (w < 0) {
                libssh2_sftp_close(handle);
                emit transferFailed(localPath, QStringLiteral("Write error during transfer"));
                return;
            }
            written += w;
        }
        done += n;
        emit transferProgress(localPath, done, totalSize);
    }

    libssh2_sftp_close(handle);
    emit transferFinished(localPath);
}

QString SftpBackend::currentPath() const
{
    return m_currentPath;
}
