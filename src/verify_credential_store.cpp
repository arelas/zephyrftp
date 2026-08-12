// Exercises CredentialStore against the REAL OS credential store — the
// freedesktop Secret Service (GNOME Keyring/KWallet) on Linux, real
// Windows Credential Manager on Windows — not a mock. Closes a real gap:
// CredentialStore had no test coverage at all before this, since none of
// the ten self-contained EXCLUDE_FROM_ALL targets can safely touch a
// real, shared OS credential store as a side effect of routine testing.
// This is deliberately NOT one of those ten either, for the same reason
// — it's opt-in, run manually, same as the other verify-* live-service
// tools in this file's family.
//
// Covers the full save/load/hasSecret/remove round trip, including a
// non-ASCII id and secret. A code review round flagged CredentialStore's
// use of qPrintable() (QString::toLocal8Bit()) instead of explicit
// toUtf8() as a possible locale-dependent encoding mismatch between
// save() and a later load()/remove() — CredentialStore.cpp switched to
// toUtf8() throughout, and this tool's non-ASCII round trip is the
// regression coverage for that change. Note, verified directly with a
// standalone probe rather than assumed: under Qt6, toLocal8Bit() always
// uses UTF-8 regardless of the process's C-library locale, so this
// specific encoding mismatch isn't actually reachable on this codebase's
// Qt6 baseline — see CredentialStore.cpp's own comment. The switch to
// explicit toUtf8() is kept anyway as a clarity/robustness improvement,
// not a live-bug fix; this tool exists mainly because CredentialStore
// had zero test coverage of any kind before this.
//
// This tool writes and immediately removes its own clearly-namespaced
// test entries (never touches anything a real saved site would use) and
// cleans up after itself even on failure, so it's safe to run against a
// real, in-use keyring.
//
// Run with:
//   cmake --build build --target verify-credential-store
//   QT_QPA_PLATFORM=offscreen ./build/verify-credential-store
#include <QCoreApplication>
#include <QDebug>
#include <QStandardPaths>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include "backends/CredentialStore.h"
#include "AppSettings.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);   // AppSettings' own settings.json, isolated below

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    // A fixed, obviously-test, obviously-this-app's-own id — namespaced
    // the same way a real SavedSite's id would be (CredentialStore's own
    // Linux implementation additionally scopes by schema, and the
    // Windows implementation by the "ZephyrFTP/site/" prefix), so this
    // can never collide with or be mistaken for a real saved site.
    const QString asciiId = QStringLiteral("verify-credential-store-ascii-test-id");
    const QString asciiSecret = QStringLiteral("plain ascii secret 123!@#");

    // Round-trip an ASCII id/secret pair first — the common case, and a
    // baseline the non-ASCII round trip below can be compared against.
    check("save() (ASCII secret): reported success", CredentialStore::save(asciiId, asciiSecret));
    check("hasSecret() (ASCII): true right after saving", CredentialStore::hasSecret(asciiId));
    QString loadedAscii;
    check("load() (ASCII): reported success", CredentialStore::load(asciiId, &loadedAscii));
    check("load() (ASCII): content round-tripped exactly", loadedAscii == asciiSecret);
    CredentialStore::remove(asciiId);
    check("hasSecret() (ASCII): false after remove()", !CredentialStore::hasSecret(asciiId));

    // Non-ASCII id and secret — a password manager or a pasted
    // passphrase is exactly where real non-ASCII content shows up
    // (accented names, non-Latin scripts, emoji); a hand-edited
    // sites.json is where a non-ASCII id could come from. Encoding
    // consistency across save()/load()/hasSecret()/remove() for both is
    // what this covers.
    const QString nonAsciiId = QStringLiteral("verify-credential-store-unicode-test-id-héllo-世界");
    const QString nonAsciiSecret = QStringLiteral("pÄsswörd-日本語-emoji-🔒-done");

    check("save() (non-ASCII id AND secret): reported success",
          CredentialStore::save(nonAsciiId, nonAsciiSecret));
    check("hasSecret() (non-ASCII id): true right after saving — an id/secret encoding "
          "mismatch between save() and hasSecret() would make this false even though the "
          "entry above just succeeded",
          CredentialStore::hasSecret(nonAsciiId));
    QString loadedNonAscii;
    check("load() (non-ASCII id): reported success — same encoding-consistency check as "
          "above, via load() instead of hasSecret()",
          CredentialStore::load(nonAsciiId, &loadedNonAscii));
    check("load() (non-ASCII secret): content round-tripped exactly, not mangled by encoding",
          loadedNonAscii == nonAsciiSecret);
    check("remove() (non-ASCII id): reported success — same site_id encoding used "
          "consistently across save()/remove(), not just save()/load()",
          CredentialStore::remove(nonAsciiId));
    check("hasSecret() (non-ASCII id): false after remove()", !CredentialStore::hasSecret(nonAsciiId));

    // AppSettings' one global proxy password (see AppSettings::
    // setProxyPassword()'s own doc comment) — a real CredentialStore
    // round trip through the actual production code path, keyed by a
    // fixed sentinel string instead of a SavedSite::id, rather than
    // duplicating that private key here and testing CredentialStore
    // directly. Deliberately NOT covered by app-settings-test (required
    // suite): that would mean a routinely, automatically run test
    // writing a real secret into the developer's actual OS keyring on
    // every run — exactly what CredentialStore.h's own doc comment and
    // this file's header comment already establish no required target
    // may do. This is the one place that's actually appropriate.
    {
        // Clear any leftover state from a previous run of this same
        // binary first — CredentialStore isn't touched by
        // QStandardPaths::setTestModeEnabled(true) the way settings.json
        // is, so state here persists in the real keyring across runs
        // unless explicitly cleaned up (a real gap found while writing
        // this: the first version of this check assumed a fresh keyring
        // and failed on a second run).
        AppSettings cleanup;
        cleanup.setProxyPassword(QString());

        AppSettings settings;
        check("AppSettings::proxyPassword(): empty before ever being set (after explicit cleanup above)",
              settings.proxyPassword().isEmpty());

        settings.setProxyPassword(QStringLiteral("real-global-proxy-password-🔒"));
        AppSettings reader;
        check("AppSettings::proxyPassword(): round-trips through a real CredentialStore "
              "save+load, via a fresh AppSettings instance (not just the same one that "
              "just set it)",
              reader.proxyPassword() == QStringLiteral("real-global-proxy-password-🔒"));

        // The one check that actually needs a real secret to have been
        // set (app-settings-test's own coverage of this never calls the
        // real setProxyPassword() at all — see its own comment) —
        // confirms the secret genuinely never lands in settings.json
        // even once a real one has actually been stored via
        // CredentialStore, not just structurally absent by construction.
        const QString settingsPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/settings.json";
        QFile settingsFile(settingsPath);
        if (settingsFile.open(QIODevice::ReadOnly)) {
            const QByteArray raw = settingsFile.readAll();
            const QJsonObject obj = QJsonDocument::fromJson(raw).object();
            check("settings.json NEVER contains a proxyPassword key, even after a real "
                  "password was actually stored via CredentialStore above",
                  !obj.contains("proxyPassword"));
            check("settings.json NEVER contains the raw proxy password value anywhere in "
                  "the file (defense in depth beyond just the key name)",
                  !raw.contains("real-global-proxy-password-🔒"));
        }

        settings.setProxyPassword(QString());
        AppSettings reader2;
        check("AppSettings::proxyPassword(): setting it to an empty string removes the "
              "stored secret, per setProxyPassword()'s own contract",
              reader2.proxyPassword().isEmpty());
    }

    // Belt-and-suspenders cleanup: remove() is a harmless no-op if the
    // entry is already gone (documented contract, see CredentialStore.h),
    // so unconditionally calling it again here for both ids leaves
    // nothing behind in a real keyring even if an assertion above failed
    // partway through.
    CredentialStore::remove(asciiId);
    CredentialStore::remove(nonAsciiId);

    qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
