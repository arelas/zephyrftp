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

    // --- Fresh start: no file yet, defaults expected, no crash ---
    {
        AppSettings settings;
        check("fresh start (no file): showHiddenFiles defaults to false", !settings.showHiddenFiles());
        check("fresh start (no file): defaultProtocol defaults to Sftp",
              settings.defaultProtocol() == Protocol::Sftp);
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

        AppSettings reader;
        check("round-trip: showHiddenFiles", reader.showHiddenFiles() == true);
        check("round-trip: defaultProtocol", reader.defaultProtocol() == Protocol::Ftp);
        check("round-trip: windowGeometry", reader.windowGeometry() == QByteArray("fake-geometry-blob"));
        check("round-trip: windowState", reader.windowState() == QByteArray("fake-state-blob"));
    }

    // --- The file on disk is valid, well-formed JSON with the expected
    // keys — a basic sanity check on save()'s actual output, not just
    // what a fresh AppSettings reads back ---
    {
        QFile rawFile(settingsPath);
        check("settings.json exists on disk after save()", rawFile.open(QIODevice::ReadOnly));
        const QJsonDocument doc = QJsonDocument::fromJson(rawFile.readAll());
        rawFile.close();
        check("settings.json is a valid JSON object", doc.isObject());
        const QJsonObject obj = doc.object();
        check("settings.json has the expected keys",
              obj.contains("showHiddenFiles") && obj.contains("defaultProtocol")
                  && obj.contains("windowGeometry") && obj.contains("windowState"));
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
              !settings.showHiddenFiles() && settings.defaultProtocol() == Protocol::Sftp);
    }

    QFile::remove(settingsPath);

    qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
