#pragma once

#include <QObject>
#include <QByteArray>
#include "backends/Protocol.h"

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

    // Whether each dock should be visible on a fresh launch. Applied by
    // MainWindow after restoreState() so it's an explicit override of
    // whatever dock-visibility restoreState() itself restored — turning
    // this off is the only way to make a dock stay closed permanently,
    // since otherwise reopening it once (even by accident) would make
    // restoreState() reopen it on every later launch too.
    bool showTransfersOnStart() const { return m_showTransfersOnStart; }
    void setShowTransfersOnStart(bool value);
    bool showCommandsOnStart() const { return m_showCommandsOnStart; }
    void setShowCommandsOnStart(bool value);

signals:
    void showHiddenFilesChanged(bool value);

private:
    static QString filePath();
    void load();
    void save() const;

    bool m_showHiddenFiles = false;
    Protocol m_defaultProtocol = Protocol::Sftp;
    QByteArray m_windowGeometry;
    QByteArray m_windowState;
    bool m_showTransfersOnStart = true;
    bool m_showCommandsOnStart = true;
};
