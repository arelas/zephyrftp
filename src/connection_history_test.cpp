// Verifies ConnectionHistoryStore's save/load round-trip, recordConnection()'s
// dedup-and-move-to-front + cap-at-N behavior, and — the load-bearing
// security property — that connection_history.json never contains a
// password/passphrase-shaped KEY, for real, not just by code inspection.
// Same technique site_store_test.cpp already established for sites.json:
// QStandardPaths::setTestModeEnabled(true) redirects AppConfigLocation to a
// throwaway `qttest` subdirectory, and the raw-JSON check inspects actual
// QJsonObject KEYS, not a text substring search (a naive contains("password")
// on the whole file would false-positive on the perfectly innocuous
// "authMethod": "password" label).
//
// Run with:
//   QT_QPA_PLATFORM=offscreen ./build/connection-history-test
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include "backends/ConnectionHistory.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);   // QStandardPaths needs an application instance
    QStandardPaths::setTestModeEnabled(true);

    bool allPass = true;

    // --- Round-trip: an SFTP key-auth entry and an FTP entry ---
    ConnectionHistoryEntry sftpEntry;
    sftpEntry.protocol = Protocol::Sftp;
    sftpEntry.host = "sftp.example.com";
    sftpEntry.port = 2222;
    sftpEntry.username = "deploy";
    sftpEntry.authMethod = SftpAuthMethod::PublicKey;
    sftpEntry.privateKeyPath = "/home/user/.ssh/id_ed25519";
    sftpEntry.useHomeDirectory = false;
    sftpEntry.startingDirectory = "/uploads/incoming";
    sftpEntry.lastConnectedAtMs = 1000;

    ConnectionHistoryEntry ftpEntry;
    ftpEntry.protocol = Protocol::Ftps;
    ftpEntry.host = "ftp.example.com";
    ftpEntry.port = 21;
    ftpEntry.username = "anonymous";
    ftpEntry.ftpsMode = FtpsMode::Explicit;
    ftpEntry.useHomeDirectory = true;
    ftpEntry.lastConnectedAtMs = 2000;

    const bool saveOk = ConnectionHistoryStore::save({sftpEntry, ftpEntry});
    qDebug() << "[test] save() succeeded:" << saveOk;
    if (!saveOk) allPass = false;

    QList<ConnectionHistoryEntry> loaded = ConnectionHistoryStore::load();
    const bool countOk = loaded.size() == 2;
    qDebug() << "[test] load() returns exactly 2 entries:" << countOk;
    if (!countOk) allPass = false;

    const bool sftpFieldsOk = countOk
        && loaded[0].protocol == Protocol::Sftp && loaded[0].host == sftpEntry.host
        && loaded[0].port == sftpEntry.port && loaded[0].username == sftpEntry.username
        && loaded[0].authMethod == SftpAuthMethod::PublicKey
        && loaded[0].privateKeyPath == sftpEntry.privateKeyPath
        && loaded[0].useHomeDirectory == false && loaded[0].startingDirectory == sftpEntry.startingDirectory
        && loaded[0].lastConnectedAtMs == sftpEntry.lastConnectedAtMs;
    qDebug() << "[test] SFTP key-auth entry round-trips exactly:" << sftpFieldsOk;
    if (!sftpFieldsOk) allPass = false;

    const bool ftpFieldsOk = countOk
        && loaded[1].protocol == Protocol::Ftps && loaded[1].host == ftpEntry.host
        && loaded[1].port == ftpEntry.port && loaded[1].username == ftpEntry.username
        && loaded[1].ftpsMode == FtpsMode::Explicit && loaded[1].useHomeDirectory == true;
    qDebug() << "[test] FTPS entry round-trips exactly:" << ftpFieldsOk;
    if (!ftpFieldsOk) allPass = false;

    // --- toConnectionRequest(): secrets always empty, everything else carried through ---
    const ConnectionRequest sftpRequest = sftpEntry.toConnectionRequest();
    const bool sftpRequestOk = sftpRequest.protocol == Protocol::Sftp
        && sftpRequest.sftp.host == sftpEntry.host && sftpRequest.sftp.port == sftpEntry.port
        && sftpRequest.sftp.authMethod == SftpAuthMethod::PublicKey
        && sftpRequest.sftp.privateKeyPath == sftpEntry.privateKeyPath
        && sftpRequest.sftp.password.isEmpty() && sftpRequest.sftp.passphrase.isEmpty()
        && sftpRequest.sourceSiteId.isEmpty();
    qDebug() << "[test] toConnectionRequest() carries fields through with secrets always empty:" << sftpRequestOk;
    if (!sftpRequestOk) allPass = false;

    // --- Read the raw file: no plausible secret-bearing key anywhere ---
    QFile rawFile(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                  + "/connection_history.json");
    rawFile.open(QIODevice::ReadOnly);
    const QByteArray rawBytes = rawFile.readAll();
    rawFile.close();
    const QJsonDocument rawDoc = QJsonDocument::fromJson(rawBytes);
    bool noSecretKey = rawDoc.isArray();
    if (rawDoc.isArray()) {
        for (const QJsonValue &v : rawDoc.array()) {
            if (!v.isObject())
                continue;
            for (const QString &key : v.toObject().keys()) {
                if (key.contains("password", Qt::CaseInsensitive)
                    || key.contains("passphrase", Qt::CaseInsensitive)) {
                    noSecretKey = false;
                }
            }
        }
    }
    qDebug() << "[test] raw connection_history.json has no password/passphrase KEY in any object:" << noSecretKey;
    if (!noSecretKey) allPass = false;

    // --- recordConnection(): dedup + move-to-front ---
    ConnectionHistoryStore::save({});   // start clean for this section
    ConnectionHistoryEntry hostA;
    hostA.protocol = Protocol::Sftp;
    hostA.host = "a.example.com";
    hostA.port = 22;
    hostA.username = "u";
    ConnectionHistoryEntry hostB = hostA;
    hostB.host = "b.example.com";

    ConnectionHistoryStore::recordConnection(hostA);
    ConnectionHistoryStore::recordConnection(hostB);
    QList<ConnectionHistoryEntry> afterTwo = ConnectionHistoryStore::load();
    const bool orderAfterTwoOk = afterTwo.size() == 2 && afterTwo[0].host == "b.example.com"
        && afterTwo[1].host == "a.example.com";
    qDebug() << "[test] recordConnection() prepends the most recent connection:" << orderAfterTwoOk;
    if (!orderAfterTwoOk) allPass = false;

    // Reconnecting to A again should move it to the front, not duplicate it.
    ConnectionHistoryStore::recordConnection(hostA);
    QList<ConnectionHistoryEntry> afterReconnectA = ConnectionHistoryStore::load();
    const bool dedupOk = afterReconnectA.size() == 2 && afterReconnectA[0].host == "a.example.com"
        && afterReconnectA[1].host == "b.example.com";
    qDebug() << "[test] reconnecting to an existing entry moves it to front instead of duplicating it:" << dedupOk;
    if (!dedupOk) allPass = false;

    // A different username to the SAME host is a genuinely different
    // connection identity — must NOT be deduplicated with hostA.
    ConnectionHistoryEntry hostADifferentUser = hostA;
    hostADifferentUser.username = "someone-else";
    ConnectionHistoryStore::recordConnection(hostADifferentUser);
    QList<ConnectionHistoryEntry> afterDifferentUser = ConnectionHistoryStore::load();
    const bool differentUserNotDedupedOk = afterDifferentUser.size() == 3;
    qDebug() << "[test] a different username on the same host is NOT deduplicated:" << differentUserNotDedupedOk;
    if (!differentUserNotDedupedOk) allPass = false;

    // --- Cap at maxEntries ---
    ConnectionHistoryStore::save({});   // start clean for this section
    for (int i = 0; i < 11; ++i) {
        ConnectionHistoryEntry e;
        e.protocol = Protocol::Sftp;
        e.host = QStringLiteral("host%1.example.com").arg(i);
        e.port = 22;
        e.username = "u";
        ConnectionHistoryStore::recordConnection(e, 10);
    }
    QList<ConnectionHistoryEntry> capped = ConnectionHistoryStore::load();
    const bool cappedCountOk = capped.size() == 10;
    qDebug() << "[test] recordConnection() caps the list at maxEntries (10):" << cappedCountOk;
    if (!cappedCountOk) allPass = false;

    const bool oldestDroppedOk = cappedCountOk && capped.first().host == "host10.example.com"
        && capped.last().host == "host1.example.com";
    qDebug() << "[test] the OLDEST entry (host0) was dropped, not the newest:" << oldestDroppedOk;
    if (!oldestDroppedOk) allPass = false;

    // --- clear() ---
    const bool clearOk = ConnectionHistoryStore::clear();
    const bool clearedListEmpty = ConnectionHistoryStore::load().isEmpty();
    qDebug() << "[test] clear() succeeds and leaves the list empty:" << (clearOk && clearedListEmpty);
    if (!clearOk || !clearedListEmpty) allPass = false;

    // --- Empty-store behavior: loading before anything was ever saved
    // returns an empty list, not an error or a crash ---
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                  + "/connection_history.json");
    const bool emptyLoadOk = ConnectionHistoryStore::load().isEmpty();
    qDebug() << "[test] load() with no file returns an empty list, not an error:" << emptyLoadOk;
    if (!emptyLoadOk) allPass = false;

    // --- loadFromFile()/saveToFile(): the same logic, parameterized by
    // an arbitrary path, same as SiteStore's own pair ---
    {
        const QString scratchPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
            + "/zephyrftp_connection_history_test_scratch.json";
        QFile::remove(scratchPath);

        const bool scratchSaveOk = ConnectionHistoryStore::saveToFile({sftpEntry}, scratchPath);
        qDebug() << "[test] saveToFile() to an arbitrary path succeeded:" << scratchSaveOk;
        if (!scratchSaveOk) allPass = false;

        const QList<ConnectionHistoryEntry> scratchLoaded = ConnectionHistoryStore::loadFromFile(scratchPath);
        const bool scratchLoadOk = scratchLoaded.size() == 1 && scratchLoaded[0].host == sftpEntry.host;
        qDebug() << "[test] loadFromFile() from that same arbitrary path round-trips correctly:" << scratchLoadOk;
        if (!scratchLoadOk) allPass = false;

        const bool realStoreUnaffected = ConnectionHistoryStore::load().isEmpty();
        qDebug() << "[test] saveToFile() to the arbitrary path left the REAL store untouched:"
                  << realStoreUnaffected;
        if (!realStoreUnaffected) allPass = false;

        QFile::remove(scratchPath);
    }

    qDebug() << (allPass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return allPass ? 0 : 1;
}
