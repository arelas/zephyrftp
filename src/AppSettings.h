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

    // The command EditSessionManager launches to open a remote file's
    // downloaded temp copy for editing (the file path is appended as
    // its sole argument — no {file}-style template substitution).
    // Empty (the default) means "use the OS's own default application
    // association" instead, via QDesktopServices::openUrl(). No change
    // signal — same reasoning as defaultProtocol above, nothing on
    // screen needs to react live to this changing.
    QString externalEditorCommand() const { return m_externalEditorCommand; }
    void setExternalEditorCommand(const QString &command);

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
    QString m_externalEditorCommand;
};
