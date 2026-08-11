#include "TransferQueueStore.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

namespace {
// Stable string keys, same reasoning Protocol.h's protocolToKey()/
// protocolFromKey() already establish for this exact problem — not the
// raw enum's integer values, which would silently change meaning if the
// enum were ever reordered. Only the three directions
// PersistedTransferItem's own doc comment says are ever actually
// persisted have real keys; anything else reading back from a
// hand-edited or corrupted file falls back to LocalToLocal, the
// simplest, safest direction to misinterpret an entry as (dispatches
// immediately against whatever local pane is available, no dangling
// wait-for-reconnect).
QString directionToKey(TransferDirection direction)
{
    switch (direction) {
    case TransferDirection::LocalToLocal:  return QStringLiteral("localToLocal");
    case TransferDirection::LocalToRemote: return QStringLiteral("localToRemote");
    case TransferDirection::RemoteToLocal: return QStringLiteral("remoteToLocal");
    default: return QStringLiteral("localToLocal");
    }
}

TransferDirection directionFromKey(const QString &key)
{
    if (key == QStringLiteral("localToRemote")) return TransferDirection::LocalToRemote;
    if (key == QStringLiteral("remoteToLocal")) return TransferDirection::RemoteToLocal;
    return TransferDirection::LocalToLocal;
}

QJsonObject connectionToJson(const ConnectionDescriptor &descriptor)
{
    QJsonObject obj;
    if (descriptor.isEmpty())
        return obj;   // local side -- an empty object round-trips back through connectionFromJson() as an empty (isEmpty()==true) descriptor
    obj[QStringLiteral("savedSiteId")] = descriptor.savedSiteId;
    obj[QStringLiteral("protocol")] = protocolToKey(descriptor.protocol);
    obj[QStringLiteral("host")] = descriptor.host;
    obj[QStringLiteral("port")] = descriptor.port;
    obj[QStringLiteral("username")] = descriptor.username;
    return obj;
}

ConnectionDescriptor connectionFromJson(const QJsonObject &obj)
{
    ConnectionDescriptor descriptor;
    descriptor.savedSiteId = obj.value(QStringLiteral("savedSiteId")).toString();
    descriptor.protocol = protocolFromKey(obj.value(QStringLiteral("protocol")).toString());
    descriptor.host = obj.value(QStringLiteral("host")).toString();
    descriptor.port = obj.value(QStringLiteral("port")).toInt();
    descriptor.username = obj.value(QStringLiteral("username")).toString();
    return descriptor;
}
}

QString TransferQueueStore::filePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + QStringLiteral("/queue.json");
}

QList<PersistedTransferItem> TransferQueueStore::load()
{
    QList<PersistedTransferItem> items;

    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly))
        return items;   // no file yet -- expected, not an error

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return items;   // corrupt or unexpected content -- fail soft to an empty queue, same as SiteStore::load()

    for (const QJsonValue &value : doc.array()) {
        if (!value.isObject())
            continue;
        const QJsonObject obj = value.toObject();

        PersistedTransferItem item;
        item.fileName = obj.value(QStringLiteral("fileName")).toString();
        item.sourcePath = obj.value(QStringLiteral("sourcePath")).toString();
        item.destPath = obj.value(QStringLiteral("destPath")).toString();
        item.direction = directionFromKey(obj.value(QStringLiteral("direction")).toString());
        item.bytesDone = static_cast<qint64>(obj.value(QStringLiteral("bytesDone")).toDouble());
        item.bytesTotal = static_cast<qint64>(obj.value(QStringLiteral("bytesTotal")).toDouble());
        item.sourceConnection = connectionFromJson(obj.value(QStringLiteral("sourceConnection")).toObject());
        item.destConnection = connectionFromJson(obj.value(QStringLiteral("destConnection")).toObject());

        // A source/dest path is the one thing an entry genuinely can't
        // function without (dispatch would have nothing to actually
        // transfer) -- skip rather than restore something broken.
        if (item.sourcePath.isEmpty() || item.destPath.isEmpty())
            continue;

        items.append(item);
    }

    return items;
}

bool TransferQueueStore::save(const QList<PersistedTransferItem> &items)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (!QDir().mkpath(dir))
        return false;

    QJsonArray array;
    for (const PersistedTransferItem &item : items) {
        QJsonObject obj;
        obj[QStringLiteral("fileName")] = item.fileName;
        obj[QStringLiteral("sourcePath")] = item.sourcePath;
        obj[QStringLiteral("destPath")] = item.destPath;
        obj[QStringLiteral("direction")] = directionToKey(item.direction);
        obj[QStringLiteral("bytesDone")] = item.bytesDone;
        obj[QStringLiteral("bytesTotal")] = item.bytesTotal;
        obj[QStringLiteral("sourceConnection")] = connectionToJson(item.sourceConnection);
        obj[QStringLiteral("destConnection")] = connectionToJson(item.destConnection);
        // No password/passphrase field, ever -- ConnectionDescriptor
        // itself has none to write. See this file's own header comment.
        array.append(obj);
    }

    // QSaveFile (not QFile + Truncate) -- same atomic-write reasoning
    // AppSettings::save() already established for settings.json: writes
    // to a temporary file first, only replacing queue.json on a
    // successful commit(), so a crash/power-loss/full-disk mid-write
    // can never leave a truncated file that load() would then silently
    // treat as "nothing to restore" (empty queue) instead of what it
    // actually is (a corrupted one).
    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly))
        return false;

    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    return file.commit();
}
