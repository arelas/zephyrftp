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
#include <QEventLoop>
#include <functional>
#include "backends/LocalBackend.h"
#include "ui/PermissionsDialog.h"

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

    // Polls rather than trusting a fixed delay — a real flake found by
    // code review (confirmed present on the unmodified pre-existing
    // baseline too, not something any of this session's own changes
    // introduced): every phase in this file uses a flat 200ms window
    // between dispatching a queued backend call and checking its result,
    // which this project's own header comments elsewhere already flag as
    // exactly the kind of assumption that doesn't hold under real load
    // (see e.g. navigation-test's own header comment). Used below only
    // where this was actually observed to flake, rather than rewriting
    // every phase in this file on spec.
    auto waitUntil = [&](std::function<bool()> condition) {
        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeoutTimer.start(5000);
        while (!condition() && timeoutTimer.isActive())
            loop.processEvents(QEventLoop::AllEvents, 20);
    };

    // --- Pure logic, no event loop needed: PermissionsDialog's
    // string<->mode conversion, the first place RemoteEntry::permissions
    // has ever been parsed back rather than just displayed. ---
    check("permissionsStringToMode: full rwx round-trips to 0755",
          permissionsStringToMode(QStringLiteral("rwxr-xr-x")) == 0755);
    check("permissionsStringToMode: all-bits-set round-trips to 0777",
          permissionsStringToMode(QStringLiteral("rwxrwxrwx")) == 0777);
    check("permissionsStringToMode: all-dashes round-trips to 0",
          permissionsStringToMode(QStringLiteral("---------")) == 0);
    check("permissionsStringToMode: FTP's own \"-\" placeholder (wrong length) -> 0, not a guess",
          permissionsStringToMode(QStringLiteral("-")) == 0);
    check("permissionsStringToMode: empty string -> 0",
          permissionsStringToMode(QString()) == 0);
    check("modeToPermissionsString: 0755 -> \"0755\"",
          modeToPermissionsString(0755) == QStringLiteral("0755"));
    check("modeToPermissionsString: 0 -> \"0000\"",
          modeToPermissionsString(0) == QStringLiteral("0000"));
    check("modeToPermissionsString: 0777 -> \"0777\"",
          modeToPermissionsString(0777) == QStringLiteral("0777"));

    QString lastFailedOperation, lastFailedPath, lastFailedReason;
    QObject::connect(backend, &RemoteBackend::fileOperationFailed, &app,
                      [&](const QString &op, const QString &path, const QString &reason) {
        lastFailedOperation = op;
        lastFailedPath = path;
        lastFailedReason = reason;
        qDebug() << "[test] fileOperationFailed:" << op << path << reason;
    });

    // For the rollback regression phases further down.
    bool lastTransferFailed = false;
    QObject::connect(backend, &RemoteBackend::transferFailed, &app,
                      [&](const QString &fileName, const QString &reason) {
        Q_UNUSED(fileName);
        lastTransferFailed = true;
        qDebug() << "[test] transferFailed:" << reason;
    });
    bool lastMoveFailed = false;
    int lastMoveFailedRequestId = -1;
    QObject::connect(backend, &RemoteBackend::entryMoveFailed, &app,
                      [&](const QString &reason, int requestId) {
        lastMoveFailed = true;
        lastMoveFailedRequestId = requestId;
        qDebug() << "[test] entryMoveFailed:" << reason << requestId;
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

        // --- Phase 10: delete a DIRECTORY SYMLINK. Regression test for a
        // real bug: QFileInfo::isDir() — which RemoteEntry::isDir, and so
        // deleteEntry()'s isDirectory parameter, is ultimately derived
        // from in listDirectory() — follows symlinks, but QDir::rmdir()
        // (POSIX rmdir(2)) deliberately does NOT follow one in its final
        // path component. A directory symlink could therefore never
        // actually be deleted, always failing with the same misleading
        // "may not be empty" message no matter how removable it (or its
        // target) genuinely was.
        //
        // Unix-only: on Windows, QFile::link() does NOT create a real
        // filesystem symlink the way it does on Unix — confirmed
        // empirically (a standalone probe run under wine, matching this
        // project's own CI): it writes a small Shell-Shortcut-style file
        // instead, which QFileInfo correctly reports as neither a
        // directory nor a symlink. Forcing isDirectory=true against that
        // (as this phase's hardcoded Q_ARG(bool, true) does, mirroring
        // what a REAL directory symlink's QFileInfo::isDir() would report
        // on Unix) would make deleteEntry() wrongly attempt QDir::rmdir()
        // on a plain file and fail — not a bug in the app, just this
        // phase's simulation technique having no Windows equivalent, so
        // it's skipped there rather than asserting something false. ---
#ifndef Q_OS_WIN
        QDir().mkpath(base + "/symlink_target");
        QFile::link(base + "/symlink_target", base + "/symlink_to_dir");
        lastFailedOperation.clear();
        QMetaObject::invokeMethod(backend, "deleteEntry", Qt::QueuedConnection,
                                   Q_ARG(QString, base + "/symlink_to_dir"), Q_ARG(bool, true));
#endif
    });

    QTimer::singleShot(2100, &app, [&]() {
#ifndef Q_OS_WIN
        check("deleteEntry (directory symlink): no failure reported", lastFailedOperation.isEmpty());
        check("deleteEntry (directory symlink): the symlink itself is gone",
              !QFileInfo(base + "/symlink_to_dir").exists());
        check("deleteEntry (directory symlink): its TARGET directory survived untouched "
              "(only the link was removed, not the real directory)",
              QDir(base + "/symlink_target").exists());
#endif

        // --- Phase 11: renameEntry() must still correctly reject a
        // GENUINE naming conflict (a real, different file already at
        // newPath) — confirms the operator== fix below didn't
        // accidentally loosen this into a no-op. ---
        QFile a(base + "/conflict_a.txt");
        a.open(QIODevice::WriteOnly);
        a.write("file a");
        a.close();
        QFile b(base + "/conflict_b.txt");
        b.open(QIODevice::WriteOnly);
        b.write("file b");
        b.close();
        lastFailedOperation.clear();
        QMetaObject::invokeMethod(backend, "renameEntry", Qt::QueuedConnection,
                                   Q_ARG(QString, base + "/conflict_a.txt"),
                                   Q_ARG(QString, base + "/conflict_b.txt"));
    });

    QTimer::singleShot(2300, &app, [&]() {
        // See waitUntil()'s own doc comment — this specific transition
        // (real disk I/O for two file writes immediately before the
        // queued dispatch, right after phase 10's own real symlink
        // deletion) was observed to occasionally still be in flight at
        // the flat 200ms mark under real load, reading lastFailedOperation
        // before fileOperationFailed had actually fired yet.
        waitUntil([&]() { return !lastFailedOperation.isEmpty(); });
        check("renameEntry onto a genuinely different existing file: still correctly rejected",
              lastFailedOperation == "Rename");
        check("renameEntry onto a genuinely different existing file: neither file was touched",
              QFile::exists(base + "/conflict_a.txt") && QFile::exists(base + "/conflict_b.txt"));

        // --- Phase 12: a failed download copy must roll back to the
        // original destination content, not lose it. Regression test for
        // a real bug: the old code deleted any existing destination
        // FIRST, then attempted the copy — if the copy then failed, the
        // original content was already gone, permanently. Forces a real,
        // deterministic copy failure against a real pre-existing
        // destination file.
        //
        // The source is a DIRECTORY, not a chmod-000 regular file — a
        // real bug in this test itself, caught by CI: chmod 000 only
        // blocks reads for a non-root user, and this project's own
        // container-based CI jobs (build-linux-appimage, build-linux-rpm,
        // build-windows) run their test suite as root inside a Docker
        // container, where root bypasses Unix permission bits entirely
        // and the "unreadable" file was actually still readable —
        // confirmed empirically inside a real `fedora:44` container:
        // QFile::copy() on a chmod-000 file succeeds as root but still
        // correctly fails (QFile refuses to open a directory as a
        // regular file) regardless of privilege, verified both as a
        // normal user and as root the same way. ---
        QDir().mkpath(base + "/rollback_src_dir");
        QFile dest(base + "/rollback_dest.txt");
        dest.open(QIODevice::WriteOnly);
        dest.write("original destination content — must survive a failed copy");
        dest.close();

        lastTransferFailed = false;
        QMetaObject::invokeMethod(backend, "downloadFile", Qt::QueuedConnection,
                                   Q_ARG(QString, base + "/rollback_src_dir"),
                                   Q_ARG(QString, base + "/rollback_dest.txt"),
                                   Q_ARG(qint64, 0));
    });

    QTimer::singleShot(2500, &app, [&]() {
        // See waitUntil()'s own doc comment — same class of fragility as
        // Phase 11, now also observed here under real load (5/20 local
        // stress-test failures before this fix).
        waitUntil([&]() { return lastTransferFailed; });
        check("downloadFile with an unreadable source: reported as a failure", lastTransferFailed);
        QFile dest(base + "/rollback_dest.txt");
        dest.open(QIODevice::ReadOnly);
        check("downloadFile with an unreadable source: the ORIGINAL destination content survived "
              "(not deleted before the copy was known to succeed)",
              dest.readAll() == "original destination content — must survive a failed copy");

        // --- Phase 13: same bug, same fix, for moveEntry() — a failed
        // move must roll back an overwritten destination too. oldPath
        // deliberately doesn't exist, guaranteeing QDir::rename() fails
        // deterministically inside moveEntry(), AFTER it has already
        // moved the pre-existing destination aside. ---
        QFile moveDest(base + "/move_rollback_dest.txt");
        moveDest.open(QIODevice::WriteOnly);
        moveDest.write("move destination content — must survive a failed move");
        moveDest.close();

        lastMoveFailed = false;
        QMetaObject::invokeMethod(backend, "moveEntry", Qt::QueuedConnection,
                                   Q_ARG(QString, base + "/move_rollback_nonexistent_source.txt"),
                                   Q_ARG(QString, base + "/move_rollback_dest.txt"),
                                   Q_ARG(int, 777));
    });

    QTimer::singleShot(2700, &app, [&]() {
        // See waitUntil()'s own doc comment — same class of fragility as
        // Phase 11, now also observed here under real load (5/20 local
        // stress-test failures before this fix).
        waitUntil([&]() { return lastMoveFailed; });
        check("moveEntry with a nonexistent source: reported as a failure", lastMoveFailed);
        check("moveEntry with a nonexistent source: the failure carried the right request id",
              lastMoveFailedRequestId == 777);
        QFile moveDest(base + "/move_rollback_dest.txt");
        moveDest.open(QIODevice::ReadOnly);
        check("moveEntry with a nonexistent source: the ORIGINAL destination content survived "
              "(not deleted before the move was known to succeed)",
              moveDest.readAll() == "move destination content — must survive a failed move");

#ifndef Q_OS_WIN
        // --- Phase 14: setPermissions() — real chmod against a real temp
        // file. Unix-only: Windows has no equivalent rwxrwxrwx permission
        // model for QFile::setPermissions() to map onto (Qt approximates
        // it via ACLs there), so a byte-exact 0640 round-trip isn't a
        // meaningful assertion on that platform the way it is on Unix. ---
        QFile permFile(base + "/chmod_target.txt");
        permFile.open(QIODevice::WriteOnly);
        permFile.write("chmod me");
        permFile.close();
        // A deliberately unusual mode (owner rw, group r, other nothing)
        // — distinct from whatever QFile's own default umask-derived mode
        // already produced, so a passing check actually proves
        // setPermissions() changed something rather than coincidentally
        // matching what was already there.
        QMetaObject::invokeMethod(backend, "setPermissions", Qt::QueuedConnection,
                                   Q_ARG(QString, base + "/chmod_target.txt"), Q_ARG(int, 0640));
#endif
    });

    QTimer::singleShot(2900, &app, [&]() {
#ifndef Q_OS_WIN
        const QFileInfo info(base + "/chmod_target.txt");
        const QFileDevice::Permissions p = info.permissions();
        const bool ownerRW = (p & QFileDevice::ReadOwner) && (p & QFileDevice::WriteOwner)
            && !(p & QFileDevice::ExeOwner);
        const bool groupR = (p & QFileDevice::ReadGroup) && !(p & QFileDevice::WriteGroup)
            && !(p & QFileDevice::ExeGroup);
        const bool otherNone = !(p & QFileDevice::ReadOther) && !(p & QFileDevice::WriteOther)
            && !(p & QFileDevice::ExeOther);
        check("setPermissions: mode 0640 actually applied (owner rw, group r, other none)",
              ownerRW && groupR && otherNone);
#endif

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}
