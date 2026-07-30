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

// RAII: temporarily switches the session to non-blocking mode so
// downloadFile()/uploadFile()'s read/write loop can pipeline multiple
// outstanding SFTP requests instead of one-request-wait-response-next —
// see SftpBackend.h's class doc comment for why this matters (a
// well-documented ~5-10x real-world throughput penalty in blocking
// mode, not a guess). Restores blocking mode on destruction regardless
// of which return path the loop took, so a single missed early-return
// can't leave the session in the wrong mode for whatever blocking call
// (sftp_close, the next transfer's sftp_open, etc.) comes after it.
class ScopedNonBlocking {
public:
    explicit ScopedNonBlocking(LIBSSH2_SESSION *session) : m_session(session)
    {
        libssh2_session_set_blocking(m_session, 0);
    }
    ~ScopedNonBlocking()
    {
        libssh2_session_set_blocking(m_session, 1);
    }
    ScopedNonBlocking(const ScopedNonBlocking &) = delete;
    ScopedNonBlocking &operator=(const ScopedNonBlocking &) = delete;

private:
    LIBSSH2_SESSION *m_session;
};

// Blocks until the socket is ready in whatever direction libssh2 says it
// currently needs (read, write, or — during a rekey — potentially the
// opposite of what the caller was actually trying to do), avoiding a
// busy-spin CPU loop while retrying an EAGAIN. Uses libssh2_poll()
// rather than raw select()/fd_set specifically for portability —
// libssh2_poll() handles the POSIX/Windows gap internally (Windows has
// no native poll()), avoiding the platform-specific header split
// (sys/select.h vs winsock2.h) that already caused a real bug earlier
// in this project's history (the first Windows build failed to compile
// over exactly this kind of POSIX-only header). Matches libssh2's own
// canonical waitsocket() pattern from its sftp_write_nonblock.c example,
// translated to the portable libssh2_poll() API instead of raw select().
void waitForSocketReady(LIBSSH2_SESSION *session, libssh2_socket_t socket)
{
    LIBSSH2_POLLFD pollFd;
    pollFd.type = LIBSSH2_POLLFD_SOCKET;
    pollFd.fd.socket = socket;
    pollFd.events = 0;

    const int dir = libssh2_session_block_directions(session);
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND)
        pollFd.events |= LIBSSH2_POLLFD_POLLIN;
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
        pollFd.events |= LIBSSH2_POLLFD_POLLOUT;

    // 1-second timeout (not the 10s the canonical example uses) — bounds
    // how long a cancel/pause request could theoretically wait behind a
    // stalled poll if the server ever stops responding mid-transfer,
    // without materially affecting steady-state throughput (a
    // well-behaved read/write call returns as soon as data/room is
    // actually available, long before this timeout is reached).
    libssh2_poll(&pollFd, 1, 1000);
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

    // Disable Nagle's algorithm (TCP_NODELAY) — standard practice for a
    // request/ACK-style protocol like SFTP. Nagle's algorithm batches
    // small outgoing writes to reduce packet count, which is exactly the
    // wrong trade for a protocol issuing many pipelined small
    // reads/writes (see downloadFile()/uploadFile()) where each one is
    // waiting on a response; the added send-side delay directly costs
    // round-trip latency, which is the whole thing pipelining is meant
    // to avoid paying repeatedly for. Qt does not disable Nagle by
    // default. This is a real, independent lever from the pipelining
    // fix itself (LowDelayOption is Qt's cross-platform equivalent of
    // setsockopt(TCP_NODELAY) — no raw platform-specific socket code
    // needed here, same reasoning as using libssh2_poll() over raw
    // select() elsewhere in this file).
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

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

    // Where to land right after connecting: either resolve the server's
    // default directory for this connection (almost always the user's
    // home directory — matches the convention other SFTP clients
    // (FileZilla, WinSCP) follow), or use an explicitly configured
    // starting directory from Site Manager. Falls back to home-dir
    // resolution if a starting directory was requested but somehow left
    // empty (shouldn't happen — SiteManagerDialog validates this — but
    // failing back to a working default beats leaving m_currentPath at
    // its hardcoded "/" fallback for no good reason).
    if (m_credentials.useHomeDirectory || m_credentials.startingDirectory.isEmpty()) {
        char realPathBuf[1024];
        const int realPathLen = libssh2_sftp_realpath(m_sftp, ".", realPathBuf, sizeof(realPathBuf) - 1);
        if (realPathLen > 0) {
            m_currentPath = QString::fromUtf8(realPathBuf, realPathLen);
        }
        // Not fatal if this fails — m_currentPath's default member
        // initializer ("/") stands in, which still works, just starts at
        // the root.
    } else {
        // Not validated here — an invalid path surfaces through the
        // normal listDirectory()/connectionFailed error path on the
        // first directory listing, same as typing a bad path into the
        // path bar manually.
        m_currentPath = m_credentials.startingDirectory;
    }

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

void SftpBackend::downloadFile(const QString &remotePath, const QString &localPath, qint64 resumeOffset)
{
    if (!ensureSession())
        return;

    m_cancelRequested.storeRelaxed(false);   // reset — backend persists across transfers
    m_pauseRequested.storeRelaxed(false);

    LIBSSH2_SFTP_HANDLE *handle =
        libssh2_sftp_open(m_sftp, remotePath.toUtf8().constData(), LIBSSH2_FXF_READ, 0);
    if (!handle) {
        emit transferFailed(remotePath, QStringLiteral("Cannot open remote file"));
        return;
    }

    // Resuming: open without truncating. QIODevice::ReadWrite (not
    // WriteOnly) so the file can be seeked/resized precisely rather than
    // just appended to — needed for the size-mismatch handling below.
    QFile out(localPath);
    const QIODevice::OpenMode openMode = resumeOffset > 0
        ? QIODevice::ReadWrite
        : (QIODevice::WriteOnly | QIODevice::Truncate);
    if (!out.open(openMode)) {
        libssh2_sftp_close(handle);
        emit transferFailed(remotePath, QStringLiteral("Cannot open local file for write: %1").arg(localPath));
        return;
    }

    // Trust what's actually on disk over the requested offset if they
    // disagree — the local file could have changed between pause and
    // resume (edge case, but a real one). Smaller-than-expected: clamp
    // down rather than zero-pad up to resumeOffset, which would corrupt
    // the file with a gap of null bytes. Larger-than-expected: trim the
    // excess back to the resume point.
    qint64 effectiveOffset = 0;
    if (resumeOffset > 0) {
        effectiveOffset = qMin(resumeOffset, out.size());
        if (out.size() > effectiveOffset)
            out.resize(effectiveOffset);
        out.seek(effectiveOffset);
        libssh2_sftp_seek64(handle, static_cast<libssh2_uint64_t>(effectiveOffset));
    }

    LIBSSH2_SFTP_ATTRIBUTES attrs;
    libssh2_sftp_fstat(handle, &attrs);
    const qint64 totalSize = static_cast<qint64>(attrs.filesize);
    qint64 done = effectiveOffset;
    bool cancelled = false;
    bool paused = false;

    // Pipelined, non-blocking read loop — see SftpBackend.h's class doc
    // comment and ScopedNonBlocking's own comment for the full reasoning.
    // In short: blocking-mode libssh2 sends one 32KB-capped SFTP READ
    // packet and waits for its ACK before sending the next, so
    // round-trip time — not bandwidth — becomes the throughput ceiling.
    // Non-blocking mode with a generous buffer lets libssh2 pipeline
    // multiple outstanding READ requests internally instead. Scoped
    // tightly to just this loop: connect/auth/host-key/directory-listing
    // and the open/close calls immediately around this block stay
    // blocking, already-verified code paths, untouched.
    ssize_t n = 0;
    {
        ScopedNonBlocking nonBlocking(m_session);
        const libssh2_socket_t sock = static_cast<libssh2_socket_t>(m_socket->socketDescriptor());
        // 480000 = 16 x 30000. libssh2's internal SFTP packet size is
        // hardcoded to exactly 30000 bytes (confirmed directly against
        // libssh2's own current source, MAX_SFTP_READ_SIZE in sftp.h —
        // not something we can change, but something our buffer size
        // should be an exact multiple of). A buffer that isn't leaves a
        // small "leftover" packet every cycle below the 30000 cap,
        // which doesn't fully use the pipeline — a real, independently
        // reported ~20% throughput cost from exactly this misalignment.
        char buf[480000];

        for (;;) {
            n = libssh2_sftp_read(handle, buf, sizeof(buf));

            if (n == LIBSSH2_ERROR_EAGAIN) {
                waitForSocketReady(m_session, sock);
                continue;
            }
            if (n <= 0)
                break;   // 0 = EOF; any other negative here is a real error, checked below

            out.write(buf, n);
            done += n;
            emit transferProgress(remotePath, done, totalSize);

            if (m_cancelRequested.loadRelaxed()) {
                cancelled = true;
                break;
            }
            if (m_pauseRequested.loadRelaxed()) {
                paused = true;
                break;
            }
        }
    }   // ScopedNonBlocking's destructor restores blocking mode here, before sftp_close below

    out.close();
    libssh2_sftp_close(handle);

    if (paused)
        emit transferPaused(remotePath, done);
    else if (cancelled)
        emit transferFailed(remotePath, QStringLiteral("Cancelled"));
    else if (n < 0)
        emit transferFailed(remotePath, QStringLiteral("Read error during transfer"));
    else
        emit transferFinished(remotePath);
}

void SftpBackend::uploadFile(const QString &localPath, const QString &remotePath, qint64 resumeOffset)
{
    if (!ensureSession())
        return;

    m_cancelRequested.storeRelaxed(false);   // reset — backend persists across transfers
    m_pauseRequested.storeRelaxed(false);

    QFile in(localPath);
    if (!in.open(QIODevice::ReadOnly)) {
        emit transferFailed(localPath, QStringLiteral("Cannot open local file: %1").arg(localPath));
        return;
    }

    // Clamp to the actual local size if the source shrank since the
    // pause (edge case) — seeking past EOF would just immediately hit
    // end-of-file and upload nothing further; resuming from what's
    // actually there is the more correct behavior.
    qint64 effectiveOffset = 0;
    if (resumeOffset > 0) {
        effectiveOffset = qMin(resumeOffset, in.size());
        in.seek(effectiveOffset);
    }

    // Resuming: no LIBSSH2_FXF_TRUNC — truncating would destroy the bytes
    // already uploaded before the pause. Fresh start keeps the original
    // truncate-on-open behavior.
    const int openFlags = effectiveOffset > 0
        ? (LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT)
        : (LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC);

    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open(
        m_sftp, remotePath.toUtf8().constData(), openFlags,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!handle) {
        emit transferFailed(localPath, QStringLiteral("Cannot open remote file for write"));
        return;
    }
    if (effectiveOffset > 0)
        libssh2_sftp_seek64(handle, static_cast<libssh2_uint64_t>(effectiveOffset));

    const qint64 totalSize = in.size();
    qint64 done = effectiveOffset;
    bool cancelled = false;
    bool paused = false;

    // Pipelined, non-blocking write loop — same reasoning as
    // downloadFile()'s read loop above (see SftpBackend.h's class doc
    // comment). Only the libssh2_sftp_write() calls need EAGAIN
    // handling — the outer loop's in.read() is a purely local QFile
    // read, unrelated to network readiness. Scoped tightly to just this
    // loop; everything around it stays blocking, already-verified code.
    {
        ScopedNonBlocking nonBlocking(m_session);
        const libssh2_socket_t sock = static_cast<libssh2_socket_t>(m_socket->socketDescriptor());
        // Same reasoning and same value as downloadFile()'s buffer above
        // — see that comment. MAX_SFTP_OUTGOING_SIZE (libssh2's internal
        // write-packet cap) is also hardcoded to exactly 30000.
        char buf[480000];

        while (!in.atEnd()) {
            qint64 n = in.read(buf, sizeof(buf));
            if (n <= 0)
                break;

            qint64 written = 0;
            while (written < n) {
                ssize_t w = libssh2_sftp_write(handle, buf + written, n - written);
                if (w == LIBSSH2_ERROR_EAGAIN) {
                    waitForSocketReady(m_session, sock);
                    continue;
                }
                if (w < 0) {
                    libssh2_sftp_close(handle);
                    emit transferFailed(localPath, QStringLiteral("Write error during transfer"));
                    return;
                }
                written += w;
            }
            done += n;
            emit transferProgress(localPath, done, totalSize);

            if (m_cancelRequested.loadRelaxed()) {
                cancelled = true;
                break;
            }
            if (m_pauseRequested.loadRelaxed()) {
                paused = true;
                break;
            }
        }
    }   // ScopedNonBlocking's destructor restores blocking mode here, before sftp_close below

    libssh2_sftp_close(handle);

    if (paused)
        emit transferPaused(localPath, done);
    else if (cancelled)
        emit transferFailed(localPath, QStringLiteral("Cancelled"));
    else
        emit transferFinished(localPath);
}

void SftpBackend::requestPause()
{
    m_pauseRequested.storeRelaxed(true);
}

void SftpBackend::requestCancel()
{
    m_cancelRequested.storeRelaxed(true);
}

QString SftpBackend::currentPath() const
{
    return m_currentPath;
}
