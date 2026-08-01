#pragma once

#include <QString>

// Opt-in secret storage for saved sites — the OS's own credential store
// (libsecret/GNOME Keyring or KWallet via the freedesktop Secret Service
// on Linux, Windows Credential Manager on Windows), never a file this
// app writes itself. This is what SiteManagerDialog's "Save password"
// checkbox actually calls.
//
// Deliberately NOT plaintext-or-obfuscated-in-our-own-JSON, the pattern
// FileZilla (sitemanager.xml, Base64 — not encryption) and WinSCP
// (without a master password) both use by default and have both been
// criticized for. This app already refuses to do the "everyone else
// does it" thing elsewhere (real host-key TOFU, fail-closed FTPS
// certs); doing the weak version here just to have the checkbox would
// cut against that. The real secret storage lives entirely in the OS's
// own protected store — sites.json itself still never contains a
// password or passphrase, unchanged from before this existed (see
// SavedSite.h and site-store-test.cpp, both still accurate).
//
// Keyed by a SavedSite's stable `id` (never its display name, which can
// change) — one secret per site, whichever one its auth method actually
// uses (a bare password, or a private key's passphrase). Only ever
// called for real saved sites with a real id; the one-off "Connect..."
// path has no id to key on and isn't offered this checkbox.
namespace CredentialStore {

// Returns true on success. Overwrites any existing secret for this id.
bool save(const QString &siteId, const QString &secret);

// Returns true and fills *secret if one was found for this id; false
// (secret left untouched) if none exists or the lookup failed for any
// reason (locked keyring, service unavailable, etc.) — callers fall
// back to prompting, the same behavior as before this existed.
bool load(const QString &siteId, QString *secret);

// Returns true if a secret was removed, false if there was nothing to
// remove (not an error — this is called unconditionally whenever the
// checkbox is unchecked, whether or not anything was actually stored).
bool remove(const QString &siteId);

// Presence check, used to set the checkbox's initial state when a site
// is loaded into the form. Implemented on top of load() on both
// platforms (there's no meaningfully cheaper "does it exist" query
// worth the extra API surface for something called once per form
// load) — the retrieved value is simply discarded, never shown or
// logged anywhere.
bool hasSecret(const QString &siteId);

}
