// Exercises SftpBackend against a REAL server — not a mock, not just
// "the code compiles" — closing three gaps flagged in ARCHITECTURE.md's
// Known gaps: (1) public-key auth had never been tried against a real
// key file, (2) checkExists() — the destination-conflict-detection
// primitive — was unverified against a real server, (3) so was
// listDirectoryForEnumeration(), the primitive the whole-folder-transfer
// recursive walk depends on. Needs
// tools/local-test-servers/start-sftp-pubkey.sh already running; this is
// NOT one of the ten fully-self-contained EXCLUDE_FROM_ALL test targets
// (it has an external precondition those deliberately don't), so it
// isn't part of that suite or CI.
//
// Run with:
//   tools/local-test-servers/start-sftp-pubkey.sh
//   cmake --build build --target verify-sftp-pubkey
//   QT_QPA_PLATFORM=offscreen SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp \
//       ./build/verify-sftp-pubkey
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QMessageBox>
#include <QAbstractButton>
#include <cstdio>
#include "ui/HostKeyVerifier.h"
#include "backends/SftpBackend.h"
#include "backends/SftpCredentials.h"
#include "transfer/FolderEnumerator.h"

namespace {
// The host-key TOFU prompt is a real, synchronous, blocking-queued
// QMessageBox::question() call from the worker thread onto this one
// (see HostKeyVerifier.cpp) — exec() pumps the event loop internally,
// same technique already proven in conflict_resolution_test.cpp for
// driving a still-open QMessageBox from a QTimer.
void autoAcceptHostKeyPrompt()
{
    QTimer::singleShot(50, qApp, [] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (box) {
            for (QAbstractButton *button : box->buttons()) {
                if (box->buttonRole(button) == QMessageBox::YesRole) {
                    button->click();
                    return;
                }
            }
        }
        // Not up yet — this is a genuinely first-ever connection to this
        // host/port combo (a fresh scratch known_hosts each run would
        // make it so every time, but XDG_CONFIG_HOME isn't overridden
        // here — see the header comment on why that's fine for this
        // harness), so keep polling briefly rather than assuming timing.
        QTimer::singleShot(50, qApp, &autoAcceptHostKeyPrompt);
    });
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // No QMainWindow is ever shown here — the host-key trust dialog is
    // the only top-level window this process ever displays, and Qt's
    // default quitOnLastWindowClosed would auto-quit the instant it's
    // closed (i.e. the moment the TOFU prompt is answered), well before
    // connectToHost() actually finishes. Same real issue already caught
    // and documented in conflict_resolution_test.cpp.
    app.setQuitOnLastWindowClosed(false);

    bool allPass = true;
    auto check = [&](const char *label, bool condition) {
        fprintf(stderr, "[%s] %s\n", condition ? "PASS" : "FAIL", label);
        if (!condition) allPass = false;
    };

    const QString scratch = qEnvironmentVariable(
        "SFTP_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/sftp");

    SftpCredentials creds;
    creds.host = "127.0.0.1";
    creds.port = 2222;
    creds.username = qEnvironmentVariable("USER", "test");
    creds.authMethod = SftpAuthMethod::PublicKey;
    creds.privateKeyPath = scratch + "/client_key";

    auto *hostKeyVerifier = new HostKeyVerifier(&app);
    auto *backend = new SftpBackend(creds, hostKeyVerifier);
    auto *thread = new QThread(&app);
    backend->moveToThread(thread);

    bool connected = false;
    QList<RemoteEntry> listedEntries;
    bool listed = false;
    bool uploadOk = false;
    bool downloadOk = false;
    QString failureReason;

    // --- checkExists() phase state ---
    bool existsFileChecked = false, existsFileResult = false, existsFileIsDir = true;
    bool existsDirChecked = false, existsDirResult = false, existsDirIsDir = false;
    bool existsMissingChecked = false, existsMissingResult = true;
    constexpr int kExistsFileRequestId = 101;
    constexpr int kExistsDirRequestId = 102;
    constexpr int kExistsMissingRequestId = 103;

    // --- FolderEnumerator phase state ---
    bool enumerationDone = false;
    bool enumerationOk = false;
    QList<EnumeratedItem> enumeratedItems;
    QString enumerationFailureReason;

    const QString localDownloadPath = "/tmp/zephyrftp_verify_sftp_download.txt";
    const QString localUploadSourcePath = "/tmp/zephyrftp_verify_sftp_upload_source.txt";
    QFile::remove(localDownloadPath);
    QFile(localUploadSourcePath).open(QIODevice::WriteOnly);
    {
        QFile f(localUploadSourcePath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("uploaded via SftpBackend PublicKey auth verification harness\n");
    }

    QObject::connect(backend, &RemoteBackend::connectionFailed, &app, [&](const QString &reason) {
        failureReason = reason;
        qApp->quit();
    });

    QObject::connect(backend, &RemoteBackend::connected, &app, [&]() {
        connected = true;
        QMetaObject::invokeMethod(backend, "listDirectory", Qt::QueuedConnection,
                                   Q_ARG(QString, "."));
    });

    QObject::connect(backend, &RemoteBackend::directoryListed, &app,
        [&](const QString &, const QList<RemoteEntry> &entries) {
            listed = true;
            listedEntries = entries;
            QMetaObject::invokeMethod(backend, "downloadFile", Qt::QueuedConnection,
                                       Q_ARG(QString, "sample.txt"),
                                       Q_ARG(QString, localDownloadPath),
                                       Q_ARG(qint64, 0));
        });

    QObject::connect(backend, &RemoteBackend::transferFinished, &app,
        [&](const QString &fileName) {
            if (fileName == "sample.txt" && !downloadOk) {
                downloadOk = true;
                QMetaObject::invokeMethod(backend, "uploadFile", Qt::QueuedConnection,
                                           Q_ARG(QString, localUploadSourcePath),
                                           Q_ARG(QString, "uploads/verify_roundtrip.txt"),
                                           Q_ARG(qint64, 0));
            } else if (fileName == localUploadSourcePath && !uploadOk) {
                uploadOk = true;
                // Three checkExists() calls in flight at once, matched
                // back by requestId in existsChecked below — exercises
                // the same requestId-disambiguation contract
                // TransferManager relies on for real, not just a single
                // best-case call.
                QMetaObject::invokeMethod(backend, "checkExists", Qt::QueuedConnection,
                                           Q_ARG(QString, "sample.txt"),
                                           Q_ARG(int, kExistsFileRequestId));
                QMetaObject::invokeMethod(backend, "checkExists", Qt::QueuedConnection,
                                           Q_ARG(QString, "testfolder"),
                                           Q_ARG(int, kExistsDirRequestId));
                QMetaObject::invokeMethod(backend, "checkExists", Qt::QueuedConnection,
                                           Q_ARG(QString, "this_does_not_exist_at_all.xyz"),
                                           Q_ARG(int, kExistsMissingRequestId));
            }
        });

    QObject::connect(backend, &RemoteBackend::transferFailed, &app,
        [&](const QString &fileName, const QString &reason) {
            failureReason = QStringLiteral("transfer of %1 failed: %2").arg(fileName, reason);
            qApp->quit();
        });

    FolderEnumerator *enumerator = nullptr;

    QObject::connect(backend, &RemoteBackend::existsChecked, &app,
        [&](const QString &, bool exists, bool isDir, int requestId) {
            switch (requestId) {
            case kExistsFileRequestId:
                existsFileChecked = true;
                existsFileResult = exists;
                existsFileIsDir = isDir;
                break;
            case kExistsDirRequestId:
                existsDirChecked = true;
                existsDirResult = exists;
                existsDirIsDir = isDir;
                break;
            case kExistsMissingRequestId:
                existsMissingChecked = true;
                existsMissingResult = exists;
                break;
            default:
                return;
            }

            if (existsFileChecked && existsDirChecked && existsMissingChecked) {
                // Real recursive walk against the real server — same
                // class TransferManager::enqueueFolder() uses, same
                // requestId-per-call contract as above, now exercising
                // SftpBackend::listDirectoryForEnumeration() specifically
                // (never listDirectory() — see RemoteBackend's own doc
                // comment on why those two must stay separate).
                enumerator = new FolderEnumerator(backend, "testfolder", "testfolder", &app);
                QObject::connect(enumerator, &FolderEnumerator::finished, &app,
                    [&](const QList<EnumeratedItem> &items) {
                        enumerationDone = true;
                        enumerationOk = true;
                        enumeratedItems = items;
                        qApp->quit();
                    });
                QObject::connect(enumerator, &FolderEnumerator::failed, &app,
                    [&](const QString &reason) {
                        enumerationDone = true;
                        enumerationOk = false;
                        enumerationFailureReason = reason;
                        qApp->quit();
                    });
                enumerator->start();
            }
        });

    thread->start();
    autoAcceptHostKeyPrompt();
    QMetaObject::invokeMethod(backend, "connectToHost", Qt::QueuedConnection);

    QTimer::singleShot(15000, &app, [&]() {
        failureReason = QStringLiteral("timed out after 15s");
        qApp->quit();
    });

    app.exec();

    check("connected to real local SFTP server via PublicKey auth", connected);
    check("listDirectory returned real entries", listed && !listedEntries.isEmpty());
    check("downloadFile completed", downloadOk);
    check("uploadFile completed", uploadOk);
    if (!failureReason.isEmpty())
        fprintf(stderr, "failure reason: %s\n", qPrintable(failureReason));

    // Confirms the downloaded content is genuinely correct, not just
    // that a transferFinished signal fired.
    QFile downloaded(localDownloadPath);
    downloaded.open(QIODevice::ReadOnly);
    const QString downloadedContent = QString::fromUtf8(downloaded.readAll());
    check("downloaded content matches the real server file",
          downloadedContent.trimmed() == "hello from the local sftp test server");

    // Confirms the upload genuinely landed server-side with correct
    // content, not just that the client thought it finished — reads the
    // file back through the same SFTP session's own listing/download
    // machinery would be circular, so this reads the server's actual
    // scratch directory directly from disk (this harness and the server
    // are on the same machine).
    QFile roundtrip(scratch + "/root/uploads/verify_roundtrip.txt");
    roundtrip.open(QIODevice::ReadOnly);
    const QString roundtripContent = QString::fromUtf8(roundtrip.readAll());
    check("uploaded file genuinely landed server-side with correct content",
          roundtripContent == "uploaded via SftpBackend PublicKey auth verification harness\n");

    // --- checkExists() against a real server ---
    check("checkExists() on a real existing FILE reports exists=true, isDir=false",
          existsFileChecked && existsFileResult && !existsFileIsDir);
    check("checkExists() on a real existing DIRECTORY reports exists=true, isDir=true",
          existsDirChecked && existsDirResult && existsDirIsDir);
    check("checkExists() on a genuinely nonexistent path reports exists=false",
          existsMissingChecked && !existsMissingResult);

    // --- listDirectoryForEnumeration() / FolderEnumerator recursive walk
    // against a real server, over a real multi-level tree with a
    // genuinely empty leaf directory (same fixture shape
    // folder-transfer-test.cpp uses against LocalBackend) ---
    check("FolderEnumerator finished (not failed) walking a real remote folder",
          enumerationDone && enumerationOk);
    if (!enumerationOk && !enumerationFailureReason.isEmpty())
        fprintf(stderr, "enumeration failure reason: %s\n", qPrintable(enumerationFailureReason));

    int fileCount = 0, dirCount = 0;
    bool sawEmptyDir = false, sawThreeLevelsDeep = false;
    for (const EnumeratedItem &item : enumeratedItems) {
        if (item.isDir) {
            dirCount++;
            if (item.relativePath == "testfolder/emptydir")
                sawEmptyDir = true;
        } else {
            fileCount++;
            if (item.relativePath == "testfolder/subdir2/nested/d.txt")
                sawThreeLevelsDeep = true;
        }
    }
    check("walk found exactly 4 real files (directories correctly excluded from that count)",
          fileCount == 4);
    check("walk found the file three levels deep (subdir2/nested/d.txt) — walk doesn't stop after one level",
          sawThreeLevelsDeep);
    check("walk included the genuinely empty leaf directory despite it contributing zero files",
          sawEmptyDir);
    check("walk found all 5 real directories (testfolder itself, subdir1, subdir2, subdir2/nested, emptydir)",
          dirCount == 5);

    backend->deleteLater();
    thread->quit();
    thread->wait();

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
