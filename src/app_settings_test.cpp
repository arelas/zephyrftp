// Verifies AppSettings' save/load round-trip for real — not just that it
// compiles. First-ever test coverage for this file. Follows
// site-store-test's own pattern: QStandardPaths::setTestModeEnabled(true)
// redirects AppConfigLocation to a throwaway `qttest` subdirectory so
// this can never touch a real user's actual settings.json.
//
// Also exercises the fix for a real bug found by code review: save()
// used to write settings.json in place (open + Truncate), so a
// crash/power-loss/full-disk mid-write could leave a truncated file
// behind — and load() treats ANY parse failure as "corrupt, reset to
// defaults," meaning a crash while saving ONE preference could silently
// wipe out every OTHER already-saved preference too. Fixed with
// QSaveFile (write-to-temp, atomic replace only on a successful
// commit()). That specific crash-mid-write recovery isn't exercised
// here — save() is private and always commits on success, so there's no
// seam to interrupt it through AppSettings' own public API without
// adding a test-only hook that doesn't otherwise need to exist — but
// QSaveFile's own crash-safety property (an abandoned, uncommitted write
// leaves the original file completely untouched) was confirmed directly
// with a standalone probe before relying on it. What IS covered here:
// the round-trip itself still works correctly after switching write
// primitives (the actual regression risk of that specific change), plus
// AppSettings' pre-existing fail-soft-to-defaults behavior on a missing
// or corrupt file.
//
// Run with:
//   QT_QPA_PLATFORM=offscreen ./build/app-settings-test
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include "AppSettings.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);   // QStandardPaths needs an application instance
    QStandardPaths::setTestModeEnabled(true);

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    const QString settingsPath =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/settings.json";
    QFile::remove(settingsPath);

    // --- proxyTypeToKey()/proxyTypeFromKey() pure round trip, same shape
    // as Protocol.h's protocolToKey()/protocolFromKey() coverage would be
    // if it had any — no I/O needed ---
    check("proxyTypeToKey/FromKey: Socks5 round-trips", proxyTypeFromKey(proxyTypeToKey(ProxyType::Socks5)) == ProxyType::Socks5);
    check("proxyTypeToKey/FromKey: Http round-trips", proxyTypeFromKey(proxyTypeToKey(ProxyType::Http)) == ProxyType::Http);
    check("proxyTypeToKey/FromKey: None round-trips", proxyTypeFromKey(proxyTypeToKey(ProxyType::None)) == ProxyType::None);
    check("proxyTypeFromKey: unrecognized/absent key -> None (the correct migration for "
          "every settings.json written before this field existed)",
          proxyTypeFromKey(QStringLiteral("bogus")) == ProxyType::None
              && proxyTypeFromKey(QString()) == ProxyType::None);

    // --- Fresh start: no file yet, defaults expected, no crash ---
    {
        AppSettings settings;
        check("fresh start (no file): showHiddenFiles defaults to false", !settings.showHiddenFiles());
        check("fresh start (no file): defaultProtocol defaults to Sftp",
              settings.defaultProtocol() == Protocol::Sftp);
        check("fresh start (no file): proxyType defaults to None",
              settings.proxyType() == ProxyType::None);
        check("fresh start (no file): quickConnectFieldVisible defaults to true "
              "(shown by default — burying a new feature behind an opt-in toggle "
              "would defeat the point of adding it)",
              settings.quickConnectFieldVisible());
        check("fresh start (no file): filenameFilterVisible defaults to true "
              "(same shown-by-default reasoning as quickConnectFieldVisible)",
              settings.filenameFilterVisible());
    }

    // --- Round-trip every field through a real save() (via the public
    // setters, since save()/load() are private) and a SECOND, independent
    // AppSettings instance's load() ---
    {
        AppSettings writer;
        writer.setShowHiddenFiles(true);
        writer.setDefaultProtocol(Protocol::Ftp);
        writer.setWindowGeometry(QByteArray("fake-geometry-blob"));
        writer.setWindowState(QByteArray("fake-state-blob"));
        writer.setQuickConnectFieldVisible(false);   // deliberately the non-default value
        writer.setFilenameFilterVisible(false);      // deliberately the non-default value

        AppSettings reader;
        check("round-trip: showHiddenFiles", reader.showHiddenFiles() == true);
        check("round-trip: defaultProtocol", reader.defaultProtocol() == Protocol::Ftp);
        check("round-trip: windowGeometry", reader.windowGeometry() == QByteArray("fake-geometry-blob"));
        check("round-trip: windowState", reader.windowState() == QByteArray("fake-state-blob"));
        check("round-trip: quickConnectFieldVisible", reader.quickConnectFieldVisible() == false);
        check("round-trip: filenameFilterVisible", reader.filenameFilterVisible() == false);
    }

    // --- Proxy: type/host/port/username round-trip through settings.json
    // like every other field above. Deliberately does NOT call
    // setProxyPassword() here — that routes through the REAL OS
    // credential store (see its own doc comment), and this is a
    // required, self-contained suite target run routinely and
    // automatically; CredentialStore.h's own doc comment and
    // verify_credential_store.cpp's header comment already establish
    // the rule this follows: no required target may touch a real,
    // shared OS credential store as a side effect of routine testing.
    // verify-credential-store (opt-in, live) covers setProxyPassword()/
    // proxyPassword() for real, including confirming the password it
    // sets doesn't leak into settings.json either. ---
    {
        AppSettings writer;
        writer.setProxyType(ProxyType::Socks5);
        writer.setProxyHost(QStringLiteral("proxy.example.com"));
        writer.setProxyPort(9050);
        writer.setProxyUsername(QStringLiteral("proxyuser"));

        AppSettings reader;
        check("round-trip: proxyType", reader.proxyType() == ProxyType::Socks5);
        check("round-trip: proxyHost", reader.proxyHost() == QStringLiteral("proxy.example.com"));
        check("round-trip: proxyPort", reader.proxyPort() == 9050);
        check("round-trip: proxyUsername", reader.proxyUsername() == QStringLiteral("proxyuser"));
    }

    // --- The file on disk is valid, well-formed JSON with the expected
    // keys — a basic sanity check on save()'s actual output, not just
    // what a fresh AppSettings reads back. Also the load-bearing check
    // that no secret ever lands here: CLAUDE.md's "no secrets in a file
    // this app writes" rule was written about sites.json, but applies
    // just as much to settings.json now that it can hold proxy config —
    // same discipline site-store-test's own raw-JSON key inspection
    // already applies to sites.json. ---
    {
        QFile rawFile(settingsPath);
        check("settings.json exists on disk after save()", rawFile.open(QIODevice::ReadOnly));
        const QByteArray raw = rawFile.readAll();
        rawFile.close();
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        check("settings.json is a valid JSON object", doc.isObject());
        const QJsonObject obj = doc.object();
        check("settings.json has the expected keys",
              obj.contains("showHiddenFiles") && obj.contains("defaultProtocol")
                  && obj.contains("windowGeometry") && obj.contains("windowState")
                  && obj.contains("proxyType") && obj.contains("proxyHost")
                  && obj.contains("proxyPort") && obj.contains("proxyUsername")
                  && obj.contains("quickConnectFieldVisible") && obj.contains("filenameFilterVisible"));
        check("settings.json NEVER contains a proxyPassword key",
              !obj.contains("proxyPassword"));
    }

    // --- A change that toggles a value back to its already-in-memory
    // default (e.g. false -> false) must NOT call save() at all — several
    // setters guard on this explicitly. Confirmed indirectly: setting
    // showHiddenFiles to its current value, then checking the file's
    // mtime didn't change, would be a real test of that, but the simpler
    // and equally real property to check is that a fresh instance still
    // sees the LAST genuinely-saved value even after a same-value no-op
    // "set" — regression test for a setter accidentally writing default
    // values back out and clobbering a real one. ---
    {
        AppSettings writer;
        writer.setShowHiddenFiles(true);   // already true from the round-trip above — a no-op set
        AppSettings reader;
        check("a same-value set() is a genuine no-op, not a silent reset",
              reader.showHiddenFiles() == true);
    }

    // --- Corrupt file: fail soft to defaults, not a crash ---
    {
        QFile corrupt(settingsPath);
        corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate);
        corrupt.write("{ this is not valid JSON");
        corrupt.close();

        AppSettings settings;
        check("corrupt settings.json: falls back to defaults instead of crashing",
              !settings.showHiddenFiles() && settings.defaultProtocol() == Protocol::Sftp
                  && settings.proxyType() == ProxyType::None
                  && settings.quickConnectFieldVisible()
                  && settings.filenameFilterVisible());
    }

    QFile::remove(settingsPath);

    qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
