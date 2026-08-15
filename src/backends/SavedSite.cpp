#include "SavedSite.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>
#include <QSet>

ConnectionRequest SavedSite::toConnectionRequest() const
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
        request.sftp.simultaneousConnections = simultaneousConnections;
        // password/passphrase left empty — deliberately; see the class doc comment.
    } else {
        request.ftp.host = host;
        request.ftp.port = port;
        request.ftp.username = username;
        request.ftp.ftpsMode = protocolToFtpsMode(protocol);
        request.ftp.useHomeDirectory = useHomeDirectory;
        request.ftp.startingDirectory = startingDirectory;
        request.ftp.simultaneousConnections = simultaneousConnections;
        // password left empty — same rule, no exception for FTP. Worth
        // stating explicitly because FTP has no key-based alternative,
        // so "don't store the password" means this protocol prompts on
        // every single connect with no way to opt out. That's the
        // intended tradeoff, not an oversight.
    }

    return request;
}

QString SiteStore::filePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + QStringLiteral("/sites.json");
}

QList<SavedSite> SiteStore::loadFromFile(const QString &path)
{
    QList<SavedSite> sites;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return sites;   // no file yet — expected on first run, not an error
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return sites;   // corrupt or unexpected content — fail soft to an empty list rather than crash

    for (const QJsonValue &value : doc.array()) {
        if (!value.isObject())
            continue;
        const QJsonObject obj = value.toObject();

        SavedSite site;
        site.id = obj.value(QStringLiteral("id")).toString();
        site.name = obj.value(QStringLiteral("name")).toString();
        site.group = obj.value(QStringLiteral("group")).toString();
        site.host = obj.value(QStringLiteral("host")).toString();
        // Read after protocol below would be cleaner, but the default
        // only matters for a malformed file missing the port key; a
        // protocol-appropriate fallback still beats a hardcoded 22.
        const Protocol parsedProtocol =
            protocolFromKey(obj.value(QStringLiteral("protocol")).toString());
        site.port = obj.value(QStringLiteral("port")).toInt(defaultPortFor(parsedProtocol));
        site.username = obj.value(QStringLiteral("username")).toString();
        // Absent in every sites.json written before FTP existed, which
        // protocolFromKey() maps to Sftp — correct, since SFTP was the
        // only thing that could have been saved back then.
        site.protocol = protocolFromKey(obj.value(QStringLiteral("protocol")).toString());
        site.authMethod = obj.value(QStringLiteral("authMethod")).toString() == QStringLiteral("key")
            ? SftpAuthMethod::PublicKey
            : SftpAuthMethod::Password;
        site.privateKeyPath = obj.value(QStringLiteral("privateKeyPath")).toString();
        // Sites saved before this field existed have no "useHomeDirectory"
        // key at all — toBool(true) means that absence correctly defaults
        // to the pre-existing behavior (resolve home dir) rather than
        // silently reinterpreting old sites as pointing at an empty path.
        site.useHomeDirectory = obj.value(QStringLiteral("useHomeDirectory")).toBool(true);
        site.startingDirectory = obj.value(QStringLiteral("startingDirectory")).toString();
        // Absent in every sites.json written before this field existed —
        // toInt(1) correctly defaults those to today's exact behavior
        // (one connection), not silently opting them into pooling.
        site.simultaneousConnections = obj.value(QStringLiteral("simultaneousConnections")).toInt(1);

        // Backfill an id for sites saved before `id` existed, rather than
        // silently dropping them or crashing on a lookup-by-id later.
        if (site.id.isEmpty())
            site.id = QUuid::createUuid().toString(QUuid::WithoutBraces);

        sites.append(site);
    }

    // A real bug found by code review: a hand-edited or otherwise
    // corrupted sites.json could have two entries sharing the same id
    // (the backfill above only handles a MISSING id, not a genuinely
    // duplicated one). Every id-based lookup elsewhere does a linear
    // first-match search (SiteManagerDialog::selectedSite(), in
    // particular), so acting on the second of two colliding rows would
    // silently mutate/delete the FIRST site instead of the one actually
    // selected. Deduplicated once, here, rather than at every call site:
    // the first occurrence of a given id keeps it; any later entry
    // sharing that id is assigned a fresh one instead, so every site
    // this function returns is guaranteed to have a genuinely unique id.
    QSet<QString> seenIds;
    for (SavedSite &site : sites) {
        if (seenIds.contains(site.id))
            site.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        seenIds.insert(site.id);
    }

    return sites;
}

bool SiteStore::saveToFile(const QList<SavedSite> &sites, const QString &path)
{
    const QFileInfo pathInfo(path);
    if (!QDir().mkpath(pathInfo.absolutePath()))
        return false;

    QJsonArray array;
    for (const SavedSite &site : sites) {
        QJsonObject obj;
        obj[QStringLiteral("id")] = site.id;
        obj[QStringLiteral("name")] = site.name;
        obj[QStringLiteral("group")] = site.group;
        obj[QStringLiteral("host")] = site.host;
        obj[QStringLiteral("port")] = site.port;
        obj[QStringLiteral("username")] = site.username;
        obj[QStringLiteral("protocol")] = protocolToKey(site.protocol);
        obj[QStringLiteral("authMethod")] =
            site.authMethod == SftpAuthMethod::PublicKey ? QStringLiteral("key") : QStringLiteral("password");
        obj[QStringLiteral("privateKeyPath")] = site.privateKeyPath;
        obj[QStringLiteral("useHomeDirectory")] = site.useHomeDirectory;
        obj[QStringLiteral("startingDirectory")] = site.startingDirectory;
        obj[QStringLiteral("simultaneousConnections")] = site.simultaneousConnections;
        // No password field written, ever — see SavedSite's doc comment.
        array.append(obj);
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    return true;
}
