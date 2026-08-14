#pragma once

#include <QObject>
#include <QByteArray>
#include "backends/Protocol.h"
#include "backends/ProxyConfig.h"
#include "Theme.h"

// App-wide preferences, persisted the same way SiteStore persists
// SavedSite — a hand-written JSON file under
// QStandardPaths::AppConfigLocation (settings.json, alongside
// sites.json/known_hosts/known_certs.json), not QSettings, so this
// follows the one persistence convention the rest of the app already
// uses rather than introducing a second one.
//
// A QObject (unlike SiteStore, which is stateless static load()/save()
// functions) specifically because showHiddenFiles needs to propagate
// live to both FilePaneWidgets the instant it's toggled in
// PreferencesDialog, without either pane re-fetching its listing from
// the backend — see FilePaneWidget's own showHiddenFilesChanged handling.
class AppSettings : public QObject {
    Q_OBJECT
public:
    explicit AppSettings(QObject *parent = nullptr);

    bool showHiddenFiles() const { return m_showHiddenFiles; }
    void setShowHiddenFiles(bool value);

    // Applied by MainWindow::onConnectTriggered() to preselect
    // ConnectionDialog's protocol combo via its existing setProtocol().
    // No change signal — unlike showHiddenFiles, nothing currently on
    // screen needs to react live to this changing.
    Protocol defaultProtocol() const { return m_defaultProtocol; }
    void setDefaultProtocol(Protocol value);

    // Opaque QMainWindow::saveGeometry()/saveState() blobs. Read once at
    // startup, written once from MainWindow::closeEvent() — no live
    // propagation needed for either.
    QByteArray windowGeometry() const { return m_windowGeometry; }
    void setWindowGeometry(const QByteArray &geometry);
    QByteArray windowState() const { return m_windowState; }
    void setWindowState(const QByteArray &state);

    // The command EditSessionManager launches to open a remote file's
    // downloaded temp copy for editing (the file path is appended as
    // its sole argument — no {file}-style template substitution).
    // Empty (the default) means "use the OS's own default application
    // association" instead, via QDesktopServices::openUrl(). No change
    // signal — same reasoning as defaultProtocol above, nothing on
    // screen needs to react live to this changing.
    QString externalEditorCommand() const { return m_externalEditorCommand; }
    void setExternalEditorCommand(const QString &command);

    // A single global proxy, applied to every SFTP/FTP/FTPS connection
    // (see MainWindow::startConnection()) — no per-site override, same
    // "one shared value" shape as defaultProtocol above. No change
    // signal — same reasoning as defaultProtocol/externalEditorCommand:
    // nothing on screen needs to react live to this changing.
    ProxyType proxyType() const { return m_proxyType; }
    void setProxyType(ProxyType value);
    QString proxyHost() const { return m_proxyHost; }
    void setProxyHost(const QString &host);
    int proxyPort() const { return m_proxyPort; }
    void setProxyPort(int port);
    QString proxyUsername() const { return m_proxyUsername; }
    void setProxyUsername(const QString &username);

    // The proxy password is deliberately NEVER a plain member here or a
    // settings.json field — see CLAUDE.md's "no secrets in a file this
    // app writes" rule, which applies just as much to settings.json as
    // it does to sites.json. Routed through CredentialStore (the OS's
    // own credential store) instead, keyed by a fixed sentinel string
    // rather than a SavedSite::id, since there's exactly one global
    // proxy config rather than one secret per saved site. Persists
    // immediately on every call, same as every other field here — no
    // opt-in "save password" checkbox the way SiteManagerDialog needs
    // one for its N-saved-sites case, since there's only ever one proxy
    // config and no other path to re-enter a not-saved password later.
    void setProxyPassword(const QString &password);
    QString proxyPassword() const;

    // Assembles the above into one ProxyConfig for a real connection
    // attempt — see SftpCredentials::proxy/FtpCredentials::proxy.
    ProxyConfig resolvedProxyConfig() const;

    // 0 (default) means unlimited. A single global value, same "one
    // shared value, no per-site override" shape as defaultProtocol/
    // proxy* above — applied PER-TRANSFER, not as one combined cap
    // across every transfer at once; see BandwidthThrottle.h's own
    // class doc comment for why. No change signal — same reasoning as
    // defaultProtocol: nothing on screen needs to react live to this
    // changing, and (like proxy) it only takes effect on the NEXT
    // connection, not retroactively on one already open. Read by
    // MainWindow::startConnection() into SftpCredentials::
    // bandwidthLimitKBps/FtpCredentials::bandwidthLimitKBps.
    int bandwidthLimitKBps() const { return m_bandwidthLimitKBps; }
    void setBandwidthLimitKBps(int value);

    // Whether the toolbar's quick-connect field (MainWindow) is shown —
    // toggled from the View menu. Defaults to true: burying a new
    // feature behind an opt-in toggle would defeat the point of adding
    // it. No change signal — MainWindow owns both the View-menu QAction
    // and the field itself and wires them directly in the same call
    // stack, same reasoning as defaultProtocol/externalEditorCommand
    // above (nothing else needs to react live to this changing).
    bool quickConnectFieldVisible() const { return m_quickConnectFieldVisible; }
    void setQuickConnectFieldVisible(bool visible);

    // Whether each pane's filename-filter field is shown — toggled from
    // the View menu, applied to BOTH panes at once (see MainWindow's
    // toggle). Defaults to true, same "burying a new feature behind an
    // opt-in toggle would defeat the point" reasoning as
    // quickConnectFieldVisible above. No change signal — each
    // FilePaneWidget reads this once, itself, at construction (it
    // already owns an AppSettings*), and MainWindow's toggle drives both
    // panes' setFilterFieldVisible() directly; nothing needs a live
    // notification path through AppSettings itself.
    bool filenameFilterVisible() const { return m_filenameFilterVisible; }
    void setFilenameFilterVisible(bool visible);

    // Whether navigating one pane drives the other to the corresponding
    // relative path — toggled from the View menu. Defaults to FALSE,
    // unlike quickConnectFieldVisible/filenameFilterVisible above: this
    // changes navigation BEHAVIOR, not just a UI element's visibility,
    // so opt-in is the safer default rather than "burying a new feature"
    // reasoning applying here. No change signal — same reasoning as
    // defaultProtocol: MainWindow owns both the View-menu QAction and
    // the orchestration logic, wired directly in the same call stack.
    bool synchronizedBrowsingEnabled() const { return m_synchronizedBrowsingEnabled; }
    void setSynchronizedBrowsingEnabled(bool enabled);

    // Dark (default) or Light. Unlike defaultProtocol/proxy*/bandwidth
    // above, this DOES need a change signal — live switching (not
    // restart-required) applies immediately: IconTheme's tinted-icon
    // colors, the app-wide QSS, and every already-visible icon all need
    // to react the instant this changes, the same "propagate live to
    // everything already on screen" reasoning showHiddenFiles already
    // established as this class's one existing precedent for it.
    Theme theme() const { return m_theme; }
    void setTheme(Theme value);

signals:
    void showHiddenFilesChanged(bool value);
    void themeChanged(Theme value);

private:
    static QString filePath();
    void load();
    void save() const;

    bool m_showHiddenFiles = false;
    Protocol m_defaultProtocol = Protocol::Sftp;
    QByteArray m_windowGeometry;
    QByteArray m_windowState;
    QString m_externalEditorCommand;
    ProxyType m_proxyType = ProxyType::None;
    QString m_proxyHost;
    int m_proxyPort = 1080;
    QString m_proxyUsername;
    bool m_quickConnectFieldVisible = true;
    bool m_filenameFilterVisible = true;
    int m_bandwidthLimitKBps = 0;
    bool m_synchronizedBrowsingEnabled = false;
    Theme m_theme = Theme::Dark;
};
