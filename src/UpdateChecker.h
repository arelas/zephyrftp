#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class AppSettings;

// One-shot GitHub "latest release" check, driving the Help menu's
// "Check for Updates..." action. Construct with a parent that outlives
// the check (MainWindow already does), call check(), and respond to
// exactly one of the two signals below — no persistent connection, no
// polling, no state kept between calls. This is the only place in the
// app that uses Qt's own HTTP stack (QNetworkAccessManager) rather than
// a raw socket: every transfer protocol goes through
// FtpBackend/SftpBackend/libssh2 instead, so there was nothing here to
// reuse.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    // settings supplies the same global proxy every SFTP/FTP/FTPS
    // connection already uses (see ProxyConfig.h) — someone routing
    // their transfers through a corporate proxy is behind it for this
    // GET too, not just left to fail past it silently.
    explicit UpdateChecker(AppSettings *settings, QObject *parent = nullptr);

    void check();

signals:
    // latestVersion has any leading 'v' already stripped (e.g. "0.8.6").
    void checked(bool updateAvailable, const QString &latestVersion, const QString &releaseUrl);
    void failed(const QString &reason);

private:
    QNetworkAccessManager *m_manager;
};
