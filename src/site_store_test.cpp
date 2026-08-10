// Verifies SiteStore's save/load round-trip for real — not just that it
// compiles. QStandardPaths::setTestModeEnabled(true) redirects
// AppConfigLocation (and friends) to a throwaway `qttest` subdirectory
// so this can never touch a real user's actual sites.json; SiteStore has
// no test-only constructor seam to redirect it otherwise, since
// QStandardPaths::AppConfigLocation is the right thing for it to use in
// production.
//
// A real bug found running this on native Windows (not just under CI's
// wine emulation): this used to rely on XDG_CONFIG_HOME pointed at a
// throwaway directory instead — an XDG/Linux-only convention Qt's native
// Win32 QStandardPaths implementation never consults at all, so the
// "isolation" silently did nothing on real Windows, and this test would
// read/overwrite/delete whatever developer's ACTUAL saved sites.json
// happened to be sitting in the real per-user config location.
// setTestModeEnabled() is Qt's own cross-platform mechanism for exactly
// this — no environment variable, no platform-specific behavior to get
// wrong.
//
// Run with:
//   QT_QPA_PLATFORM=offscreen ./build/site-store-test
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include "backends/SavedSite.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);   // QStandardPaths needs an application instance
    QStandardPaths::setTestModeEnabled(true);

    bool allPass = true;

    // --- Round-trip: a password-auth site and a key-auth site ---
    QList<SavedSite> original;

    SavedSite passwordSite;
    passwordSite.id = "site-1";
    passwordSite.name = "Pivot Parking";
    passwordSite.group = "Clients";
    passwordSite.host = "sftp.pivotparking.net";
    passwordSite.port = 2222;
    passwordSite.username = "deploy";
    passwordSite.authMethod = SftpAuthMethod::Password;
    passwordSite.useHomeDirectory = false;
    passwordSite.startingDirectory = "/uploads/incoming";
    original.append(passwordSite);

    SavedSite keySite;
    keySite.id = "site-2";
    keySite.name = "Staging box";
    keySite.group = "Internal";
    keySite.host = "staging.internal";
    keySite.port = 22;
    keySite.username = "ci";
    keySite.authMethod = SftpAuthMethod::PublicKey;
    keySite.privateKeyPath = "/home/user/.ssh/staging_ed25519";
    keySite.useHomeDirectory = true;   // deliberately the default, to cover both branches
    original.append(keySite);

    const bool saveOk = SiteStore::save(original);
    qDebug() << "[test] save() succeeded:" << saveOk;
    if (!saveOk) allPass = false;

    const QList<SavedSite> loaded = SiteStore::load();
    qDebug() << "[test] loaded" << loaded.size() << "sites (expected 2)";
    const bool countOk = loaded.size() == 2;
    if (!countOk) allPass = false;

    bool fieldsOk = true;
    if (countOk) {
        for (int i = 0; i < 2; ++i) {
            const SavedSite &o = original[i];
            const SavedSite &l = loaded[i];
            const bool matches = o.id == l.id && o.name == l.name && o.group == l.group
                && o.host == l.host && o.port == l.port && o.username == l.username
                && o.authMethod == l.authMethod && o.privateKeyPath == l.privateKeyPath
                && o.useHomeDirectory == l.useHomeDirectory
                && o.startingDirectory == l.startingDirectory;
            qDebug() << "[test] site" << i << "round-trip match:" << matches;
            if (!matches) fieldsOk = false;
        }
    }
    if (!fieldsOk) allPass = false;

    // --- Security property: no password ever appears in the file, and
    // toConnectionRequest() never fabricates one from thin air ---
    const ConnectionRequest passwordRequest = passwordSite.toConnectionRequest();
    const bool credsPasswordEmpty = passwordRequest.sftp.password.isEmpty();
    qDebug() << "[test] toConnectionRequest() leaves password empty for a Password-auth site:" << credsPasswordEmpty;
    if (!credsPasswordEmpty) allPass = false;

    // passwordSite was set up with useHomeDirectory=false and a specific
    // path above — confirm toConnectionRequest() actually carries that
    // choice through rather than silently defaulting back to home-dir.
    const bool startingDirOk = !passwordRequest.useHomeDirectory()
        && passwordRequest.startingDirectory() == "/uploads/incoming";
    qDebug() << "[test] toConnectionRequest() carries the starting-directory choice through:" << startingDirOk;
    if (!startingDirOk) allPass = false;

    // Read the raw file and confirm no plausible secret-bearing key name
    // exists anywhere in it — belt-and-suspenders on top of SavedSite
    // structurally having no password field to serialize in the first
    // place. Checks actual JSON object KEYS via QJsonDocument, not a raw
    // text substring search: a naive `contains("password")` on the whole
    // file false-positives on the perfectly innocuous
    // `"authMethod": "password"` label (which distinguishes auth type,
    // carries no secret) — caught during this test's own development,
    // fixed before it could misreport a real problem that wasn't there.
    QFile rawFile(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/sites.json");
    rawFile.open(QIODevice::ReadOnly);
    const QByteArray rawBytes = rawFile.readAll();
    // Closed explicitly rather than left for end-of-scope: the empty-store
    // check below deletes this same file, and on Windows DeleteFile()
    // fails while any handle to it is still open (unlike POSIX unlink(),
    // which doesn't care) — caught by actually running this test on
    // Windows, not by reasoning about the code.
    rawFile.close();
    const QJsonDocument rawDoc = QJsonDocument::fromJson(rawBytes);
    bool noPasswordKey = rawDoc.isArray();
    if (rawDoc.isArray()) {
        for (const QJsonValue &v : rawDoc.array()) {
            if (!v.isObject())
                continue;
            const QStringList keys = v.toObject().keys();
            for (const QString &key : keys) {
                if (key.contains("password", Qt::CaseInsensitive)
                    || key.contains("passphrase", Qt::CaseInsensitive)) {
                    noPasswordKey = false;
                }
            }
        }
    }
    qDebug() << "[test] raw sites.json has no password/passphrase KEY in any object:" << noPasswordKey;
    if (!noPasswordKey) allPass = false;

    // --- Empty-store behavior: loading before anything was ever saved
    // should return an empty list, not an error or a crash ---
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/sites.json");
    const QList<SavedSite> emptyLoad = SiteStore::load();
    const bool emptyOk = emptyLoad.isEmpty();
    qDebug() << "[test] load() with no file returns an empty list, not an error:" << emptyOk;
    if (!emptyOk) allPass = false;

    // --- Duplicate-id handling: a hand-edited or otherwise corrupted
    // sites.json with two entries sharing the same id must come back
    // with genuinely unique ids, not silently keep the collision.
    // Regression test for a real bug: the id-backfill above only handles
    // a MISSING id, not a genuinely duplicated one — every id-based
    // lookup elsewhere (SiteManagerDialog::selectedSite(), in
    // particular) does a linear first-match search, so acting on the
    // second of two colliding rows would silently affect the FIRST site
    // instead of the one actually selected. ---
    {
        QJsonArray dupArray;
        QJsonObject dup1;
        dup1["id"] = "duplicate-id";
        dup1["name"] = "First Site";
        dup1["host"] = "first.example.com";
        dupArray.append(dup1);
        QJsonObject dup2;
        dup2["id"] = "duplicate-id";
        dup2["name"] = "Second Site";
        dup2["host"] = "second.example.com";
        dupArray.append(dup2);

        QFile dupFile(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/sites.json");
        dupFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
        dupFile.write(QJsonDocument(dupArray).toJson());
        dupFile.close();

        const QList<SavedSite> deduped = SiteStore::load();
        const bool dupCountOk = deduped.size() == 2;
        qDebug() << "[test] a duplicate-id file still loads both sites:" << dupCountOk;
        if (!dupCountOk) allPass = false;

        const bool idsUnique = dupCountOk && deduped[0].id != deduped[1].id;
        qDebug() << "[test] duplicate ids were deduplicated — the two sites now have genuinely different ids:"
                  << idsUnique;
        if (!idsUnique) allPass = false;

        const bool firstKeptOriginalId = dupCountOk && deduped[0].id == "duplicate-id";
        qDebug() << "[test] the FIRST occurrence kept its original id (only the later duplicate was reassigned):"
                  << firstKeptOriginalId;
        if (!firstKeptOriginalId) allPass = false;

        const bool namesIntact = dupCountOk && deduped[0].name == "First Site" && deduped[1].name == "Second Site";
        qDebug() << "[test] both sites' other fields survived intact:" << namesIntact;
        if (!namesIntact) allPass = false;
    }

    qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
