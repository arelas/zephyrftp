// Headless functional test of the four file-management operations —
// delete, rename, create file, create folder — against a real
// LocalBackend and real temp directories, not mocked state. Tests the
// backend directly (not through FilePaneWidget's right-click menu),
// since the menu's own role is just prompting via QInputDialog/QMessageBox
// dialogs, which can't be driven headlessly — the actual risk and
// complexity lives in the backend operations themselves, which this
// exercises for real.
//
// SftpBackend's implementation of these four operations is NOT covered
// here — no live SFTP server is available in this environment, the same
// limitation already flagged elsewhere in this project for transfers,
// cancel/pause/resume, and pipelining. What's verified: the operations
// compile and the libssh2 calls they're built on were confirmed against
// the installed libssh2_sftp.h before being used, but "does this actually
// delete a file on a real server" is unverified.
#include <QCoreApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include "backends/LocalBackend.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);   // no GUI needed — testing the backend directly, not FilePaneWidget

    const QString base = "/tmp/file_ops_test";
    QDir().mkpath(base);

    auto *backend = new LocalBackend();

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    QString lastFailedOperation, lastFailedPath, lastFailedReason;
    QObject::connect(backend, &RemoteBackend::fileOperationFailed, &app,
                      [&](const QString &op, const QString &path, const QString &reason) {
        lastFailedOperation = op;
        lastFailedPath = path;
        lastFailedReason = reason;
        qDebug() << "[test] fileOperationFailed:" << op << path << reason;
    });

    // --- Phase 1: create an empty file ---
    QTimer::singleShot(100, &app, [&]() {
        QMetaObject::invokeMethod(backend, "createFile", Qt::QueuedConnection,
                                   Q_ARG(QString, base + "/newfile.txt"));
    });

    QTimer::singleShot(300, &app, [&]() {
        const QFileInfo info(base + "/newfile.txt");
        check("createFile: file now exists", info.exists());
        check("createFile: file is empty (0 bytes)", info.size() == 0);

        // --- Phase 2: refuse to overwrite an existing file ---
        QFile marker(base + "/existing.txt");
        marker.open(QIODevice::WriteOnly);
        marker.write("original content");
        marker.close();

        lastFailedOperation.clear();
        QMetaObject::invokeMethod(backend, "createFile", Qt::QueuedConnection,
                                   Q_ARG(QString, base + "/existing.txt"));
    });

    QTimer::singleShot(500, &app, [&]() {
        check("createFile on an existing name: reported as a failure", lastFailedOperation == "Create file");
        QFile marker(base + "/existing.txt");
        marker.open(QIODevice::ReadOnly);
        check("createFile on an existing name: original content untouched (not truncated)",
              marker.readAll() == "original content");

        // --- Phase 3: create a folder ---
        QMetaObject::invokeMethod(backend, "createDirectory", Qt::QueuedConnection,
                                   Q_ARG(QString, base + "/newfolder"));
    });

    QTimer::singleShot(700, &app, [&]() {
        check("createDirectory: folder now exists", QDir(base + "/newfolder").exists());

        // --- Phase 4: refuse to create a folder where one already exists ---
        lastFailedOperation.clear();
        QMetaObject::invokeMethod(backend, "createDirectory", Qt::QueuedConnection,
                                   Q_ARG(QString, base + "/newfolder"));
    });

    QTimer::singleShot(900, &app, [&]() {
        check("createDirectory on an existing name: reported as a failure",
              lastFailedOperation == "Create folder");

        // --- Phase 5: rename a file ---
        QMetaObject::invokeMethod(backend, "renameEntry", Qt::QueuedConnection,
                                   Q_ARG(QString, base + "/newfile.txt"),
                                   Q_ARG(QString, base + "/renamed.txt"));
    });

    QTimer::singleShot(1100, &app, [&]() {
        check("renameEntry (file): old name is gone", !QFile::exists(base + "/newfile.txt"));
        check("renameEntry (file): new name exists", QFile::exists(base + "/renamed.txt"));

        // --- Phase 6: rename a folder ---
        QMetaObject::invokeMethod(backend, "renameEntry", Qt::QueuedConnection,
                                   Q_ARG(QString, base + "/newfolder"),
                                   Q_ARG(QString, base + "/renamedfolder"));
    });

    QTimer::singleShot(1300, &app, [&]() {
        check("renameEntry (folder): old name is gone", !QDir(base + "/newfolder").exists());
        check("renameEntry (folder): new name exists", QDir(base + "/renamedfolder").exists());

        // --- Phase 7: delete a file ---
        QMetaObject::invokeMethod(backend, "deleteEntry", Qt::QueuedConnection,
                                   Q_ARG(QString, base + "/renamed.txt"), Q_ARG(bool, false));
    });

    QTimer::singleShot(1500, &app, [&]() {
        check("deleteEntry (file): file is gone", !QFile::exists(base + "/renamed.txt"));

        // --- Phase 8: delete an EMPTY folder ---
        QMetaObject::invokeMethod(backend, "deleteEntry", Qt::QueuedConnection,
                                   Q_ARG(QString, base + "/renamedfolder"), Q_ARG(bool, true));
    });

    QTimer::singleShot(1700, &app, [&]() {
        check("deleteEntry (empty folder): folder is gone", !QDir(base + "/renamedfolder").exists());

        // --- Phase 9: refuse to delete a NON-empty folder (not recursive) ---
        QDir().mkpath(base + "/nonempty/inner");
        lastFailedOperation.clear();
        QMetaObject::invokeMethod(backend, "deleteEntry", Qt::QueuedConnection,
                                   Q_ARG(QString, base + "/nonempty"), Q_ARG(bool, true));
    });

    QTimer::singleShot(1900, &app, [&]() {
        check("deleteEntry (non-empty folder): reported as a failure, not silently ignored",
              lastFailedOperation == "Delete");
        check("deleteEntry (non-empty folder): folder was NOT deleted (no accidental recursive wipe)",
              QDir(base + "/nonempty").exists() && QDir(base + "/nonempty/inner").exists());

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}
