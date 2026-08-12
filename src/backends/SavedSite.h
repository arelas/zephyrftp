#pragma once

#include <QString>
#include <QList>
#include "SftpCredentials.h"
#include "ConnectionRequest.h"

// A saved connection profile — everything ConnectionDialog collects,
// plus a display name and an optional group for organizing the site
// tree (e.g. "Clients" / "Internal", matching the design mockup).
//
// DELIBERATE SECURITY DECISION: there's no `password` field, and never
// will be — this struct, and the sites.json file it (de)serializes to,
// stay permanently secret-free, verified directly by
// site-store-test.cpp's raw-JSON key inspection. A saved site with
// SftpAuthMethod::Password always prompts for the password at connect
// time (SiteManagerDialog does this) rather than reading one from here.
//
// This does NOT mean a password can never be remembered at all —
// SiteManagerDialog's opt-in "Save password" checkbox (unchecked by
// default) persists it via CredentialStore instead, which writes to the
// OS's own credential store (libsecret/Keychain-equivalent on Linux,
// the real Windows Credential Manager on Windows), keyed by this
// struct's `id`. That's a deliberately different, narrower promise than
// the original "no passwords, full stop": the plaintext-in-our-own-file
// pattern FileZilla and WinSCP (without a master password) both use by
// default — and have both been publicly criticized for — is still
// refused; what's offered instead is real OS-level secret storage,
// opt-in, with the connect-time prompt always still shown (pre-filled,
// never silent) rather than skipped. See CredentialStore.h for the
// full reasoning. Private-key auth's non-secret fields (host/port/
// username/key path) have always been saved here in full regardless —
// the key file itself lives on disk under the OS's own permissions and
// is commonly passphrase-protected already, a materially different risk
// than a bare password string.
struct SavedSite {
    QString id;       // stable identifier (not the display name, which can change) — a random string, generated once at creation
    QString name;     // display name, e.g. "Pivot Parking"
    QString group;    // e.g. "Clients" — empty means ungrouped, shown at the tree root

    // Which protocol this site connects with. Defaults to Sftp, which is
    // also what every site saved before this field existed reads back as
    // — see protocolFromKey()'s comment for why that's the correct
    // migration rather than merely a convenient default.
    Protocol protocol = Protocol::Sftp;

    QString host;
    int port = 22;
    QString username;
    // Only meaningful when protocol == Sftp; FTP and FTPS have no
    // key-based auth at all (see supportsKeyAuth()). Left at Password
    // for those, and ignored.
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

    // Builds the right credentials for this site's protocol, wrapped in
    // a ConnectionRequest. The password is ALWAYS left empty, for every
    // protocol — the caller (SiteManagerDialog) prompts for it and fills
    // it in before connecting; this struct has no way to carry one, by
    // design. That property is what site-store-test asserts against the
    // actual bytes on disk.
    ConnectionRequest toConnectionRequest() const;
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
    static QList<SavedSite> load() { return loadFromFile(filePath()); }

    // Returns false if the file couldn't be written (e.g. permissions).
    // Creates the containing directory if it doesn't exist yet.
    static bool save(const QList<SavedSite> &sites) { return saveToFile(sites, filePath()); }

    // The same parsing/serialization load()/save() use above, against an
    // ARBITRARY path rather than the fixed sites.json location — what
    // SiteManagerDialog's Export.../Import... actually call. Not a
    // separate implementation: load()/save() are thin wrappers around
    // these, so every existing defensive-parsing behavior (missing-field
    // defaults, the duplicate-id backfill, failing soft to an empty list
    // on a corrupt/unreadable file) is exactly what an imported file
    // gets too, for free — the same tolerance a hand-edited or
    // older-version sites.json already gets. Export's file is exactly
    // this struct's existing JSON schema, not a separate interchange
    // format — already documented, already secret-free by construction
    // (SavedSite has no password field to accidentally serialize; see
    // its own doc comment), so nothing extra is needed to make an
    // exported file safe to hand to someone else.
    static QList<SavedSite> loadFromFile(const QString &path);
    static bool saveToFile(const QList<SavedSite> &sites, const QString &path);

private:
    static QString filePath();
};
