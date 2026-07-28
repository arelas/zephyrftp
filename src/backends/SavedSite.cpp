#include "SavedSite.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

SftpCredentials SavedSite::toCredentials() const
{
    SftpCredentials creds;
    creds.host = host;
    creds.port = port;
    creds.username = username;
    creds.authMethod = authMethod;
    creds.privateKeyPath = privateKeyPath;
    creds.useHomeDirectory = useHomeDirectory;
    creds.startingDirectory = startingDirectory;
    // creds.password left empty — deliberately; see the class doc comment.
    return creds;
}

QString SiteStore::filePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + QStringLiteral("/sites.json");
}

QList<SavedSite> SiteStore::load()
{
    QList<SavedSite> sites;

    QFile file(filePath());
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
        site.port = obj.value(QStringLiteral("port")).toInt(22);
        site.username = obj.value(QStringLiteral("username")).toString();
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

        // Backfill an id for sites saved before `id` existed, rather than
        // silently dropping them or crashing on a lookup-by-id later.
        if (site.id.isEmpty())
            site.id = QUuid::createUuid().toString(QUuid::WithoutBraces);

        sites.append(site);
    }

    return sites;
}

bool SiteStore::save(const QList<SavedSite> &sites)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (!QDir().mkpath(dir))
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
        obj[QStringLiteral("authMethod")] =
            site.authMethod == SftpAuthMethod::PublicKey ? QStringLiteral("key") : QStringLiteral("password");
        obj[QStringLiteral("privateKeyPath")] = site.privateKeyPath;
        obj[QStringLiteral("useHomeDirectory")] = site.useHomeDirectory;
        obj[QStringLiteral("startingDirectory")] = site.startingDirectory;
        // No password field written, ever — see SavedSite's doc comment.
        array.append(obj);
    }

    QFile file(filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    return true;
}
