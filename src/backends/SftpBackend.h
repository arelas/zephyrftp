#pragma once

#include "RemoteBackend.h"
#include <libssh2.h>
#include <libssh2_sftp.h>

// SFTP backend built directly on libssh2. All calls here are BLOCKING
// (that's how libssh2's sync API works) — this object must live on a
// QThread of its own, never the GUI thread. MainWindow is responsible for
// the moveToThread() + signal/slot wiring; this class assumes it's already
// off the main thread by the time connectToHost() runs.
//
// UNVERIFIED: auth is password-only below. Key-based auth
// (libssh2_userauth_publickey_fromfile) and known_hosts verification are
// both required before this is safe to point at a real server — neither
// is implemented yet, flagged here rather than silently skipped.
class SftpBackend : public RemoteBackend {
    Q_OBJECT
public:
    SftpBackend(QString host, int port, QString username, QString password,
                QObject *parent = nullptr);
    ~SftpBackend() override;

    void connectToHost() override;
    void listDirectory(const QString &path) override;
    void downloadFile(const QString &remotePath, const QString &localPath) override;
    void uploadFile(const QString &localPath, const QString &remotePath) override;

    QString currentPath() const override;
    bool isLocalFilesystem() const override { return false; }

private:
    bool ensureSession();   // lazy TCP + libssh2 handshake + auth
    void teardown();

    QString m_host;
    int m_port;
    QString m_username;
    QString m_password;
    QString m_currentPath = QStringLiteral("/");

    int m_socket = -1;
    LIBSSH2_SESSION *m_session = nullptr;
    LIBSSH2_SFTP *m_sftp = nullptr;
};
