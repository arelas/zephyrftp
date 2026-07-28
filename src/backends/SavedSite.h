#pragma once

#include <QString>
#include <QList>
#include "SftpCredentials.h"

// A saved connection profile — everything ConnectionDialog collects,
// plus a display name and an optional group for organizing the site
// tree (e.g. "Clients" / "Internal", matching the design mockup).
//
// DELIBERATE SECURITY DECISION: passwords are never persisted here, full
// stop — there's no `password` field. A saved site with
// SftpAuthMethod::Password always prompts for the password at connect
// time (SiteManagerDialog does this), rather than writing a plaintext
// secret to disk. This is narrower than the original design mockup,
// which showed a "Password" logon type as distinct from "Ask for
// password" — implying the mockup intended to support storing it. That
// was overridden here on purpose: this app has otherwise been built
// with real security hygiene (host-key TOFU verification, no silent
// trust), and shipping plaintext credential storage without being
// explicitly asked to would cut directly against that. Private-key auth
// CAN be saved in full (host/port/username/key path) — the key file
// itself lives on disk under the OS's own permissions and is commonly
// passphrase-protected already, which is a materially different risk
// than a bare password string in a JSON file.
struct SavedSite {
    QString id;       // stable identifier (not the display name, which can change) — a random string, generated once at creation
    QString name;     // display name, e.g. "Pivot Parking"
    QString group;    // e.g. "Clients" — empty means ungrouped, shown at the tree root

    QString host;
    int port = 22;
    QString username;
    SftpAuthMethod authMethod = SftpAuthMethod::Password;
    QString privateKeyPath;   // only meaningful when authMethod == PublicKey

    // Where this site's remote pane lands right after connecting.
    // Defaults to true (resolve the server's home directory, matching
    // the pre-existing behavior); if false, startingDirectory is used
    // as-is. Not validated here — an invalid path surfaces through the
    // normal listDirectory()/connectionFailed error path on first
    // connect, same as typing a bad path into the path bar manually.
    bool useHomeDirectory = true;
    QString startingDirectory;

    // Builds an SftpCredentials for connecting. password is left empty
    // when authMethod == Password — the caller (SiteManagerDialog) is
    // responsible for prompting and filling it in before actually
    // connecting; this struct has no way to carry one.
    SftpCredentials toCredentials() const;
};

// Loads/saves the full site list as JSON under
// QStandardPaths::AppConfigLocation (same directory as known_hosts).
// Deliberately simple — no encryption, no database, just a JSON array on
// disk; there's no secret in it worth protecting beyond what file
// permissions on that directory already provide (see the security note
// on SavedSite above for why there's no password to protect in the
// first place).
class SiteStore {
public:
    // Returns an empty list (not an error) if the file doesn't exist yet
    // — that's the expected state on first run, not a failure.
    static QList<SavedSite> load();

    // Returns false if the file couldn't be written (e.g. permissions).
    // Creates the containing directory if it doesn't exist yet.
    static bool save(const QList<SavedSite> &sites);

private:
    static QString filePath();
};
