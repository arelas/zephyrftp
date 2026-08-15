#include "ConnectionHistory.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

ConnectionRequest ConnectionHistoryEntry::toConnectionRequest() const
{
    ConnectionRequest request;
    request.protocol = protocol;

    if (protocol == Protocol::Sftp) {
        request.sftp.host = host;
        request.sftp.port = port;
        request.sftp.username = username;
        request.sftp.authMethod = authMethod;
        request.sftp.privateKeyPath = privateKeyPath;
        request.sftp.useHomeDirectory = useHomeDirectory;
        request.sftp.startingDirectory = startingDirectory;
        // password/passphrase left empty — deliberately; see the struct's own doc comment.
    } else {
        request.ftp.host = host;
        request.ftp.port = port;
        request.ftp.username = username;
        request.ftp.ftpsMode = ftpsMode;
        request.ftp.useHomeDirectory = useHomeDirectory;
        request.ftp.startingDirectory = startingDirectory;
        // password left empty — same rule, no exception for FTP.
    }
    // sourceSiteId left empty — this connection didn't come from a
    // SavedSite, and that emptiness is exactly what makes it eligible to
    // be recorded again after a successful reconnect.

    return request;
}

QString ConnectionHistoryStore::filePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + QStringLiteral("/connection_history.json");
}

QList<ConnectionHistoryEntry> ConnectionHistoryStore::loadFromFile(const QString &path)
{
    QList<ConnectionHistoryEntry> entries;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return entries;   // no file yet — expected on first run, not an error

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return entries;   // corrupt or unexpected content — fail soft to an empty list rather than crash

    for (const QJsonValue &value : doc.array()) {
        if (!value.isObject())
            continue;
        const QJsonObject obj = value.toObject();

        ConnectionHistoryEntry entry;
        entry.protocol = protocolFromKey(obj.value(QStringLiteral("protocol")).toString());
        entry.host = obj.value(QStringLiteral("host")).toString();
        entry.port = obj.value(QStringLiteral("port")).toInt(defaultPortFor(entry.protocol));
        entry.username = obj.value(QStringLiteral("username")).toString();
        entry.authMethod = obj.value(QStringLiteral("authMethod")).toString() == QStringLiteral("key")
            ? SftpAuthMethod::PublicKey
            : SftpAuthMethod::Password;
        entry.privateKeyPath = obj.value(QStringLiteral("privateKeyPath")).toString();
        entry.ftpsMode = obj.value(QStringLiteral("ftpsMode")).toString() == QStringLiteral("explicit")
            ? FtpsMode::Explicit
            : FtpsMode::None;
        entry.useHomeDirectory = obj.value(QStringLiteral("useHomeDirectory")).toBool(true);
        entry.startingDirectory = obj.value(QStringLiteral("startingDirectory")).toString();
        entry.lastConnectedAtMs = static_cast<qint64>(obj.value(QStringLiteral("lastConnectedAt")).toDouble());

        // A malformed/hand-edited row with no host is useless in a
        // "pick something to reconnect to" menu — skip it rather than
        // surfacing an empty entry.
        if (entry.host.isEmpty())
            continue;

        entries.append(entry);
    }

    return entries;
}

bool ConnectionHistoryStore::saveToFile(const QList<ConnectionHistoryEntry> &entries, const QString &path)
{
    const QFileInfo pathInfo(path);
    if (!QDir().mkpath(pathInfo.absolutePath()))
        return false;

    QJsonArray array;
    for (const ConnectionHistoryEntry &entry : entries) {
        QJsonObject obj;
        obj[QStringLiteral("protocol")] = protocolToKey(entry.protocol);
        obj[QStringLiteral("host")] = entry.host;
        obj[QStringLiteral("port")] = entry.port;
        obj[QStringLiteral("username")] = entry.username;
        obj[QStringLiteral("authMethod")] =
            entry.authMethod == SftpAuthMethod::PublicKey ? QStringLiteral("key") : QStringLiteral("password");
        obj[QStringLiteral("privateKeyPath")] = entry.privateKeyPath;
        obj[QStringLiteral("ftpsMode")] =
            entry.ftpsMode == FtpsMode::Explicit ? QStringLiteral("explicit") : QStringLiteral("none");
        obj[QStringLiteral("useHomeDirectory")] = entry.useHomeDirectory;
        obj[QStringLiteral("startingDirectory")] = entry.startingDirectory;
        obj[QStringLiteral("lastConnectedAt")] = entry.lastConnectedAtMs;
        // No password/passphrase field written, ever — see ConnectionHistoryEntry's own doc comment.
        array.append(obj);
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    return true;
}

bool ConnectionHistoryStore::recordConnection(const ConnectionHistoryEntry &entry, int maxEntries)
{
    QList<ConnectionHistoryEntry> entries = load();

    // Remove any existing entry matching the same real-world connection
    // identity — reconnecting to something already in history moves it
    // to the front instead of duplicating it.
    for (int i = entries.size() - 1; i >= 0; --i) {
        const ConnectionHistoryEntry &existing = entries.at(i);
        if (existing.protocol == entry.protocol && existing.host == entry.host
            && existing.port == entry.port && existing.username == entry.username) {
            entries.removeAt(i);
        }
    }

    entries.prepend(entry);
    while (entries.size() > maxEntries)
        entries.removeLast();

    return save(entries);
}
