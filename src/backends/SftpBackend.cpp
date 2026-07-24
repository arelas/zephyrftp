#include "SftpBackend.h"

#include <QFile>
#include <QFileInfo>
#include <QTcpSocket>

SftpBackend::SftpBackend(QString host, int port, QString username, QString password,
                          QObject *parent)
    : RemoteBackend(parent)
    , m_host(std::move(host))
    , m_port(port)
    , m_username(std::move(username))
    , m_password(std::move(password))
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
    m_socket->connectToHost(m_host, static_cast<quint16>(m_port));
    if (!m_socket->waitForConnected(10000)) {
        emit connectionFailed(QStringLiteral("TCP connect to %1:%2 failed: %3")
                               .arg(m_host).arg(m_port).arg(m_socket->errorString()));
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

    // UNVERIFIED: no host-key verification against known_hosts here yet.
    // libssh2_session_hostkey() + libssh2_knownhost_* need to be wired in
    // before this touches anything but a throwaway test server.

    if (libssh2_userauth_password(m_session, m_username.toUtf8().constData(),
                                   m_password.toUtf8().constData()) != 0) {
        emit connectionFailed(QStringLiteral("Authentication failed for %1").arg(m_username));
        return false;
    }

    m_sftp = libssh2_sftp_init(m_session);
    if (!m_sftp) {
        emit connectionFailed(QStringLiteral("SFTP subsystem init failed"));
        return false;
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
        char perms[10] = "---------";
        mode_t m = attrs.permissions;
        if (m & S_IRUSR) perms[0] = 'r';
        if (m & S_IWUSR) perms[1] = 'w';
        if (m & S_IXUSR) perms[2] = 'x';
        if (m & S_IRGRP) perms[3] = 'r';
        if (m & S_IWGRP) perms[4] = 'w';
        if (m & S_IXGRP) perms[5] = 'x';
        if (m & S_IROTH) perms[6] = 'r';
        if (m & S_IWOTH) perms[7] = 'w';
        if (m & S_IXOTH) perms[8] = 'x';
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
