// Verifies SiteStore's save/load round-trip for real — not just that it
// compiles. Run with XDG_CONFIG_HOME pointed at a throwaway directory
// (see the run command below) so this can never touch a real user's
// actual sites.json; SiteStore has no test-only constructor seam to
// redirect it otherwise, since QStandardPaths::AppConfigLocation is the
// right thing for it to use in production.
//
// Run with:
//   XDG_CONFIG_HOME=/tmp/zephyrftp_test_config QT_QPA_PLATFORM=offscreen ./build/site-store-test
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
    // toCredentials() never fabricates one from thin air ---
    const SftpCredentials passwordCreds = passwordSite.toCredentials();
    const bool credsPasswordEmpty = passwordCreds.password.isEmpty();
    qDebug() << "[test] toCredentials() leaves password empty for a Password-auth site:" << credsPasswordEmpty;
    if (!credsPasswordEmpty) allPass = false;

    // passwordSite was set up with useHomeDirectory=false and a specific
    // path above — confirm toCredentials() actually carries that choice
    // through rather than silently defaulting back to home-dir.
    const bool startingDirOk = !passwordCreds.useHomeDirectory
        && passwordCreds.startingDirectory == "/uploads/incoming";
    qDebug() << "[test] toCredentials() carries the starting-directory choice through:" << startingDirOk;
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

    qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
