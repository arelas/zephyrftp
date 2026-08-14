#include "AppSettings.h"
#include "backends/CredentialStore.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
// Not a real SavedSite::id (those are QUuid-formatted,
// "{8-4-4-4-12 hex}") — CredentialStore is keyed by an arbitrary
// QString, and this reuses it for the one global proxy secret instead
// of building a second OS-credential-store integration for a single
// value. See AppSettings::setProxyPassword()'s own doc comment.
const QString kGlobalProxyCredentialKey = QStringLiteral("__zephyrftp_global_proxy__");
}

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

void AppSettings::setTheme(Theme value)
{
    if (m_theme == value)
        return;
    m_theme = value;
    save();
    emit themeChanged(value);
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

void AppSettings::setExternalEditorCommand(const QString &command)
{
    if (m_externalEditorCommand == command)
        return;
    m_externalEditorCommand = command;
    save();
}

void AppSettings::setProxyType(ProxyType value)
{
    if (m_proxyType == value)
        return;
    m_proxyType = value;
    save();
}

void AppSettings::setProxyHost(const QString &host)
{
    if (m_proxyHost == host)
        return;
    m_proxyHost = host;
    save();
}

void AppSettings::setProxyPort(int port)
{
    if (m_proxyPort == port)
        return;
    m_proxyPort = port;
    save();
}

void AppSettings::setProxyUsername(const QString &username)
{
    if (m_proxyUsername == username)
        return;
    m_proxyUsername = username;
    save();
}

void AppSettings::setProxyPassword(const QString &password)
{
    if (password.isEmpty())
        CredentialStore::remove(kGlobalProxyCredentialKey);
    else
        CredentialStore::save(kGlobalProxyCredentialKey, password);
}

QString AppSettings::proxyPassword() const
{
    QString password;
    CredentialStore::load(kGlobalProxyCredentialKey, &password);
    return password;
}

ProxyConfig AppSettings::resolvedProxyConfig() const
{
    ProxyConfig config;
    config.type = m_proxyType;
    config.host = m_proxyHost;
    config.port = m_proxyPort;
    config.username = m_proxyUsername;
    config.password = proxyPassword();
    return config;
}

void AppSettings::setBandwidthLimitKBps(int value)
{
    if (m_bandwidthLimitKBps == value)
        return;
    m_bandwidthLimitKBps = value;
    save();
}

void AppSettings::setSynchronizedBrowsingEnabled(bool enabled)
{
    if (m_synchronizedBrowsingEnabled == enabled)
        return;
    m_synchronizedBrowsingEnabled = enabled;
    save();
}

void AppSettings::setQuickConnectFieldVisible(bool visible)
{
    if (m_quickConnectFieldVisible == visible)
        return;
    m_quickConnectFieldVisible = visible;
    save();
}

void AppSettings::setFilenameFilterVisible(bool visible)
{
    if (m_filenameFilterVisible == visible)
        return;
    m_filenameFilterVisible = visible;
    save();
}

void AppSettings::setMenuBarVisible(bool visible)
{
    if (m_menuBarVisible == visible)
        return;
    m_menuBarVisible = visible;
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
    m_externalEditorCommand = obj.value(QStringLiteral("externalEditorCommand")).toString();
    m_proxyType = proxyTypeFromKey(obj.value(QStringLiteral("proxyType")).toString());
    m_proxyHost = obj.value(QStringLiteral("proxyHost")).toString();
    m_proxyPort = obj.value(QStringLiteral("proxyPort")).toInt(1080);
    m_proxyUsername = obj.value(QStringLiteral("proxyUsername")).toString();
    m_quickConnectFieldVisible = obj.value(QStringLiteral("quickConnectFieldVisible")).toBool(true);
    m_filenameFilterVisible = obj.value(QStringLiteral("filenameFilterVisible")).toBool(true);
    m_bandwidthLimitKBps = obj.value(QStringLiteral("bandwidthLimitKBps")).toInt(0);
    m_synchronizedBrowsingEnabled = obj.value(QStringLiteral("synchronizedBrowsingEnabled")).toBool(false);
    m_theme = themeFromKey(obj.value(QStringLiteral("theme")).toString());
    m_menuBarVisible = obj.value(QStringLiteral("menuBarVisible")).toBool(true);
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
    obj[QStringLiteral("externalEditorCommand")] = m_externalEditorCommand;
    obj[QStringLiteral("proxyType")] = proxyTypeToKey(m_proxyType);
    obj[QStringLiteral("proxyHost")] = m_proxyHost;
    obj[QStringLiteral("proxyPort")] = m_proxyPort;
    obj[QStringLiteral("proxyUsername")] = m_proxyUsername;
    // No proxyPassword key, ever — see setProxyPassword()'s own doc
    // comment; that secret lives only in CredentialStore.
    obj[QStringLiteral("quickConnectFieldVisible")] = m_quickConnectFieldVisible;
    obj[QStringLiteral("filenameFilterVisible")] = m_filenameFilterVisible;
    obj[QStringLiteral("bandwidthLimitKBps")] = m_bandwidthLimitKBps;
    obj[QStringLiteral("synchronizedBrowsingEnabled")] = m_synchronizedBrowsingEnabled;
    obj[QStringLiteral("theme")] = themeToKey(m_theme);
    obj[QStringLiteral("menuBarVisible")] = m_menuBarVisible;

    // QSaveFile (not QFile + Truncate) — writes the new content to a
    // temporary file first and only atomically replaces settings.json on
    // a successful commit(), so a crash/power-loss/full-disk mid-write
    // can never leave a truncated or half-written file in its place. That
    // mattered more here than it might look: load() treats ANY parse
    // failure as "corrupt or unexpected content — fail soft to defaults"
    // (see its own comment), so a truncated file didn't just lose
    // whichever single preference was being saved — it silently reset
    // every saved preference (window geometry/state included) back to
    // hardcoded defaults on the next launch. A real risk found by code
    // review, not exercised by any test until app-settings-test's
    // simulated-crash-mid-write regression (below) — building `dir +
    // "settings.json"` directly here also avoids re-resolving
    // AppConfigLocation a second time, which filePath() (already called
    // once above via `dir`) would otherwise do on every single save().
    QSaveFile file(dir + QStringLiteral("/settings.json"));
    if (!file.open(QIODevice::WriteOnly))
        return;

    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    file.commit();
}
