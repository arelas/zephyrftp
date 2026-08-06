#include "AppSettings.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

AppSettings::AppSettings(QObject *parent)
    : QObject(parent)
{
    load();
}

void AppSettings::setShowHiddenFiles(bool value)
{
    if (m_showHiddenFiles == value)
        return;
    m_showHiddenFiles = value;
    save();
    emit showHiddenFilesChanged(value);
}

void AppSettings::setDefaultProtocol(Protocol value)
{
    if (m_defaultProtocol == value)
        return;
    m_defaultProtocol = value;
    save();
}

void AppSettings::setWindowGeometry(const QByteArray &geometry)
{
    m_windowGeometry = geometry;
    save();
}

void AppSettings::setWindowState(const QByteArray &state)
{
    m_windowState = state;
    save();
}

void AppSettings::setShowTransfersOnStart(bool value)
{
    if (m_showTransfersOnStart == value)
        return;
    m_showTransfersOnStart = value;
    save();
}

void AppSettings::setShowCommandsOnStart(bool value)
{
    if (m_showCommandsOnStart == value)
        return;
    m_showCommandsOnStart = value;
    save();
}

QString AppSettings::filePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + QStringLiteral("/settings.json");
}

void AppSettings::load()
{
    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly))
        return;   // no file yet — expected on first run, defaults already set above

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return;   // corrupt or unexpected content — fail soft to defaults, same as SiteStore::load()

    const QJsonObject obj = doc.object();
    m_showHiddenFiles = obj.value(QStringLiteral("showHiddenFiles")).toBool(false);
    m_defaultProtocol = protocolFromKey(obj.value(QStringLiteral("defaultProtocol")).toString());
    m_windowGeometry = QByteArray::fromBase64(
        obj.value(QStringLiteral("windowGeometry")).toString().toLatin1());
    m_windowState = QByteArray::fromBase64(
        obj.value(QStringLiteral("windowState")).toString().toLatin1());
    m_showTransfersOnStart = obj.value(QStringLiteral("showTransfersOnStart")).toBool(true);
    m_showCommandsOnStart = obj.value(QStringLiteral("showCommandsOnStart")).toBool(true);
}

void AppSettings::save() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (!QDir().mkpath(dir))
        return;

    QJsonObject obj;
    obj[QStringLiteral("showHiddenFiles")] = m_showHiddenFiles;
    obj[QStringLiteral("defaultProtocol")] = protocolToKey(m_defaultProtocol);
    obj[QStringLiteral("windowGeometry")] = QString::fromLatin1(m_windowGeometry.toBase64());
    obj[QStringLiteral("windowState")] = QString::fromLatin1(m_windowState.toBase64());
    obj[QStringLiteral("showTransfersOnStart")] = m_showTransfersOnStart;
    obj[QStringLiteral("showCommandsOnStart")] = m_showCommandsOnStart;

    QFile file(filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;

    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
}
