#pragma once

#include <QString>
#include <QList>
#include "SftpCredentials.h"
#include "ConnectionRequest.h"

// A recently-established, AD-HOC connection — Quick Connect toolbar or the
// plain Connect... dialog, never something already saved as a SavedSite
// (see MainWindow::startConnection()'s own doc comment on why: a
// Site-Manager-sourced connection is already one click away there, so
// duplicating it into a second list would be noise, not new value).
//
// DELIBERATE SECURITY DECISION, stricter than SavedSite's own: this
// struct, and the connection_history.json file it (de)serializes to,
// NEVER hold a password or passphrase, with no opt-in escape hatch —
// verified directly by connection-history-test.cpp's raw-JSON key
// inspection, same technique site-store-test.cpp already established.
// Unlike a SavedSite (an explicit, named, intentionally-kept profile
// someone might reasonably want CredentialStore to remember a secret
// for), a history entry is inherently ephemeral and has no stable id
// worth keying a stored secret on — reconnecting from history always
// re-prompts for exactly one secret (password, or a key's passphrase),
// the same prompt SiteManagerDialog::onConnectClicked() already shows,
// just with no save-back step.
struct ConnectionHistoryEntry {
    Protocol protocol = Protocol::Sftp;
    QString host;
    int port = 0;
    QString username;

    // Only meaningful when protocol == Sftp.
    SftpAuthMethod authMethod = SftpAuthMethod::Password;
    QString privateKeyPath;   // only meaningful when authMethod == PublicKey — a path, not a secret, same as SavedSite's identical field

    // Only meaningful when protocol == Ftp or Ftps.
    FtpsMode ftpsMode = FtpsMode::None;

    bool useHomeDirectory = true;
    QString startingDirectory;

    // QDateTime::currentMSecsSinceEpoch() at the moment this connection
    // succeeded — drives most-recent-first ordering. Not shown as a raw
    // number anywhere; the UI is free to format it however it likes.
    qint64 lastConnectedAtMs = 0;

    // Builds a ConnectionRequest ready for MainWindow::startConnection(),
    // mirroring SavedSite::toConnectionRequest() exactly — password/
    // passphrase always left empty, for the same reason: this struct has
    // no way to carry one, by design. sourceSiteId is deliberately left
    // empty (this connection didn't come from a SavedSite, and
    // reconnecting from history shouldn't record ITSELF as if it did —
    // see the recording hook in MainWindow::startConnection() for why
    // that matters: an empty sourceSiteId is exactly the condition that
    // makes a connection eligible to be recorded again).
    ConnectionRequest toConnectionRequest() const;
};

// Loads/saves the recent-connections list as JSON under
// QStandardPaths::AppConfigLocation, alongside sites.json — its own
// file, not a second array folded into sites.json, matching
// TransferQueueStore's own established "one persistence convention, one
// file per concern" precedent (see that class's doc comment). Same
// deliberately simple shape as SiteStore: no encryption, no database,
// just a JSON array on disk.
class ConnectionHistoryStore {
public:
    // Returns an empty list (not an error) if the file doesn't exist yet
    // — expected on first run, not a failure.
    static QList<ConnectionHistoryEntry> load() { return loadFromFile(filePath()); }

    static bool save(const QList<ConnectionHistoryEntry> &entries) { return saveToFile(entries, filePath()); }

    // The one call MainWindow's connection-success hook actually needs:
    // loads the current list, removes any existing entry that matches
    // `entry` on (protocol, host, port, username) — reconnecting to
    // something already in history moves it to the front rather than
    // duplicating it — prepends the new entry, truncates to maxEntries,
    // and saves. Returns false if the save itself failed (e.g.
    // permissions); the in-memory list is still correctly updated
    // either way; callers here don't currently act on a false, same as
    // most SiteStore::save() call sites.
    static bool recordConnection(const ConnectionHistoryEntry &entry, int maxEntries = 10);

    // Empties the list — "Clear Recent Connections".
    static bool clear() { return saveToFile({}, filePath()); }

    // Same load()/save() logic, parameterized by an arbitrary path —
    // exists for the same reason SiteStore's own pair does (a clean seam
    // for tests to point at a scratch file), even though nothing in the
    // UI currently exports/imports connection history the way sites.json
    // can be.
    static QList<ConnectionHistoryEntry> loadFromFile(const QString &path);
    static bool saveToFile(const QList<ConnectionHistoryEntry> &entries, const QString &path);

private:
    static QString filePath();
};
