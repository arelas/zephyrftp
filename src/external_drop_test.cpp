// Headless functional test of dragging files between this app and the
// OS's own file manager — a direct user request ("allow the dragging
// of files between the app and the system file manager"), both
// directions.
//
// Phases A-D: dragging files/folders IN (OS -> a pane). Covers the
// actual new logic (TransferManager::enqueueExternalUpload()/
// enqueueExternalFolder(), the isExternal branch of
// PendingFolderConflictCheck, and FolderEnumerator run against a
// throwaway LocalBackend with no pane association at all) against a
// real LocalBackend and real temp directories — not mocked state, same
// convention folder_transfer_test.cpp already established.
//
// Phases E-G: dragging files OUT (a REMOTE pane -> the OS), via
// FileTreeView::downloadForDragOut() called directly rather than
// through startDrag() itself — startDrag() is protected, and more
// fundamentally a real native drag gesture can't be reliably triggered
// under the offscreen platform (QDrag::exec() has no real drop target
// to negotiate with headlessly; this project's test suite has no
// existing precedent for simulating one). downloadForDragOut() is
// exposed public specifically for this — see its own doc comment
// (FileTreeView.h) — and IS the real, novel logic (the thin mouse-
// gesture layer above it is a handful of lines, not independently
// worth this same investment). A LOCAL pane's own drag-out path
// (building QUrls from files that already exist) is trivial enough
// (two lines in startDrag() itself) that it's judged sufficient to
// verify by reading the code rather than a matching test here.
//
// SftpBackend/FtpBackend are NOT covered here — no live server is
// available in this environment, the same limitation already flagged
// elsewhere in this project. Nothing about dispatchActiveItem()'s own
// upload path changes for an externally-dropped file vs. a pane-to-pane
// one (both end up calling uploadFile(item.sourcePath, item.destPath)
// on the SAME destination backend either way), so this local-only
// coverage exercises the real, novel code — the difference is entirely
// upstream of dispatch, in how sourcePath gets resolved with no source
// pane at all. downloadForDragOut() itself is direction-agnostic too
// (it calls TransferManager::startEditDownload(), the same primitive
// regardless of which concrete RemoteBackend subclass answers it), so a
// small fake non-local backend below exercises the real logic under
// test just as faithfully as a live SFTP server would for this specific
// purpose.
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>
#include <QMimeData>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QTreeView>
#include <QMessageBox>
#include <QAbstractButton>
#include <QCheckBox>
#include "ui/FilePaneWidget.h"
#include "ui/FileTreeView.h"
#include "backends/LocalBackend.h"
#include "backends/RemoteBackend.h"
#include "transfer/TransferManager.h"

namespace {
bool writeFile(const QString &path, const QString &content)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content.toUtf8());
    return true;
}

QString readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

// Minimal fake for Phases E-G below — downloadForDragOut() only ever
// calls TransferManager::startEditDownload(), which only ever needs
// isLocalFilesystem()==false and a working downloadFile(); every other
// override exists purely because RemoteBackend is a pure interface
// (same "legitimate test-double technique" transfer_concurrency_test.cpp's
// own FakeAsyncBackend already establishes, not a mock framework).
// downloadFile() writes real, identifiable bytes to the real local
// path it's given — exactly what a genuine SFTP/FTP download would
// produce — so the resulting temp file is something Phase E-G can
// actually read back and verify, not just assert existence of.
class FakeRemoteBackend : public RemoteBackend {
    Q_OBJECT
public:
    explicit FakeRemoteBackend(QObject *parent = nullptr) : RemoteBackend(parent) {}
    QString currentPath() const override { return QStringLiteral("/remote"); }
    bool isLocalFilesystem() const override { return false; }
    QString connectionIdentity() const override { return QStringLiteral("fake-remote"); }
    void requestCancel() override {}
    void requestPause() override {}

    // Entry names containing this substring fail instead of succeeding —
    // Phase G's own partial-failure coverage.
    static const QString kFailMarker;

public slots:
    void connectToHost() override { emit connected(); }
    void listDirectory(const QString &path) override { emit directoryListed(path, {}); }
    void downloadFile(const QString &remotePath, const QString &localPath, qint64 resumeOffset = 0) override {
        Q_UNUSED(resumeOffset);
        if (remotePath.contains(kFailMarker)) {
            emit transferFailed(remotePath, QStringLiteral("simulated failure"));
            return;
        }
        QFile f(localPath);
        f.open(QIODevice::WriteOnly);
        const QByteArray content = ("fake remote content of " + remotePath).toUtf8();
        f.write(content);
        f.close();
        emit transferProgress(remotePath, content.size(), content.size());
        emit transferFinished(remotePath);
    }
    void uploadFile(const QString &, const QString &, qint64 = 0) override {}
    void deleteEntry(const QString &, bool) override {}
    void renameEntry(const QString &, const QString &) override {}
    void moveEntry(const QString &, const QString &, int requestId) override {
        emit entryMoveFailed(QStringLiteral("Not implemented"), requestId);
    }
    void createDirectory(const QString &, bool = false) override {}
    void createFile(const QString &) override {}
    void setPermissions(const QString &, int) override {}
    void listDirectoryForEnumeration(const QString &, int) override {}
    void checkExists(const QString &path, int requestId) override {
        emit existsChecked(path, false, false, requestId);
    }
};
const QString FakeRemoteBackend::kFailMarker = QStringLiteral("shouldfail");
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    const QString base = QStringLiteral("/tmp/external_drop_test");
    QDir(base).removeRecursively();
    QDir().mkpath(base + "/outside");     // simulates a directory OUTSIDE this app's own panes entirely
    QDir().mkpath(base + "/dest");

    writeFile(base + "/outside/dropped.txt", "hello from outside");

    // A real nested tree to drop as a folder — same shape
    // folder_transfer_test.cpp's own fixture uses, including a
    // genuinely empty subdirectory (the case most likely to be
    // silently skipped).
    QDir().mkpath(base + "/outside/myfolder/subdir1");
    QDir().mkpath(base + "/outside/myfolder/emptydir");
    writeFile(base + "/outside/myfolder/a.txt", "hi from a");
    writeFile(base + "/outside/myfolder/subdir1/b.txt", "hi from b");

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    auto *destPane = new FilePaneWidget(new LocalBackend());
    auto *manager = new TransferManager(&app);

    int itemsAdded = 0, itemsDone = 0;
    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &) {
        itemsAdded++;
    });
    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.status == TransferStatus::Done)
            itemsDone++;
    });

    QTimer::singleShot(200, &app, [&]() {
        destPane->navigateTo(base + "/dest");
    });

    // ---------- Phase A: a single external file ----------
    QTimer::singleShot(400, &app, [&]() {
        check("setup: dest pane at its own root", destPane->currentDirectory() == base + "/dest");
        manager->enqueueExternalUpload(destPane, base + "/outside/dropped.txt", "dropped.txt");
    });

    QTimer::singleShot(1000, &app, [&]() {
        check("Phase A: external file transferred with correct content",
              readFile(base + "/dest/dropped.txt") == "hello from outside");
        check("Phase A: exactly 1 item added so far", itemsAdded == 1);
        check("Phase A: it reached Done", itemsDone == 1);
    });

    // ---------- Phase B: an external folder, with a nested empty dir ----------
    QTimer::singleShot(1200, &app, [&]() {
        manager->enqueueExternalFolder(destPane, base + "/outside/myfolder", "myfolder");
    });

    QTimer::singleShot(2200, &app, [&]() {
        const QString dst = base + "/dest/myfolder";
        check("Phase B: root folder created", QDir(dst).exists());
        check("Phase B: subdir1 created", QDir(dst + "/subdir1").exists());
        check("Phase B: emptydir created despite containing nothing — the case most likely "
              "to be silently skipped",
              QDir(dst + "/emptydir").exists());
        check("Phase B: a.txt transferred with correct content",
              readFile(dst + "/a.txt") == "hi from a");
        check("Phase B: subdir1/b.txt transferred with correct content",
              readFile(dst + "/subdir1/b.txt") == "hi from b");
        check("Phase B: exactly 2 more items added (a.txt, b.txt — not counting directories)",
              itemsAdded == 3);
        check("Phase B: both reached Done", itemsDone == 3);
    });

    // ---------- Phase C: dropping a folder that already exists at the
    // destination — the isExternal branch of PendingFolderConflictCheck,
    // driving the exact same Write Into dialog an ordinary pane-to-pane
    // folder conflict already uses. Single fixed-delay check-and-click,
    // not a repeating poll — matching conflict_resolution_test.cpp's own
    // clickConflictDialog() pattern for this exact same dialog exactly,
    // since a real QMessageBox::exec() blocks indefinitely until clicked
    // (nothing auto-dismisses it), so any single check after it has
    // opened finds it exactly as reliably as a poll would. ----------
    QTimer::singleShot(2400, &app, [&]() {
        // Same source folder again — myfolder already exists at the
        // destination from Phase B, so this is a genuine root conflict.
        writeFile(base + "/outside/myfolder/c.txt", "hi from c (added after phase B)");
        manager->enqueueExternalFolder(destPane, base + "/outside/myfolder", "myfolder");
    });

    bool sawConflictDialog = false;
    QTimer::singleShot(2900, &app, [&]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (box) {
            sawConflictDialog = true;
            for (QAbstractButton *button : box->buttons()) {
                if (button->text().contains("Write Into", Qt::CaseInsensitive)) {
                    button->click();
                    break;
                }
            }
        }
    });

    // a.txt/b.txt (already transferred in Phase B) ALSO trigger their own
    // per-file conflict — a real, separate dialog, same as it would for
    // an ordinary pane-to-pane Write Into merge (this isn't specific to
    // external drops; startExternalFolderFileTransfers() reuses the
    // exact same per-file enqueueExternalUpload()/startNext()/checkExists()
    // pipeline unchanged). Checking "apply to all" + Overwrite on the
    // first one resolves both a.txt and b.txt via the remembered
    // decision; c.txt is genuinely new so it needs no dialog of its own.
    QTimer::singleShot(3300, &app, [&]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (box) {
            if (QCheckBox *checkbox = box->findChild<QCheckBox *>())
                checkbox->setChecked(true);
            for (QAbstractButton *button : box->buttons()) {
                if (button->text().contains("Overwrite", Qt::CaseInsensitive)) {
                    button->click();
                    break;
                }
            }
        }
    });

    QTimer::singleShot(4200, &app, [&]() {
        check("Phase C: the Write Into conflict dialog actually appeared for the external "
              "folder drop (proves isExternal correctly routed through the ordinary "
              "root-conflict pipeline)",
              sawConflictDialog);
        check("Phase C: Write Into merged the new file in without disturbing what was already there",
              readFile(base + "/dest/myfolder/c.txt") == "hi from c (added after phase B)"
                  && readFile(base + "/dest/myfolder/a.txt") == "hi from a");
    });

    // ---------- Phase D: the actual FileTreeView-level drop handling —
    // a real QDropEvent carrying real file:// QUrls, delivered via
    // QCoreApplication::sendEvent() (a legitimate, standard Qt technique
    // for exercising a protected event handler like dropEvent() from
    // outside the class — sendEvent() dispatches through QWidget::event()
    // exactly as the real windowing system would, no friend/protected-
    // access workaround needed). Proves the actual drop-to-signal
    // translation works, not just the TransferManager API it calls into. ----
    QTimer::singleShot(4400, &app, [&]() {
        auto *dropPane = new FilePaneWidget(new LocalBackend());
        auto *view = dropPane->findChild<QTreeView *>();
        check("Phase D: found the drop-target pane's QTreeView", view != nullptr);

        QStringList capturedPaths;
        QObject::connect(dropPane, &FilePaneWidget::externalFilesDropped, &app,
                          [&](const QStringList &paths) { capturedPaths = paths; });

        QList<QUrl> urls;
        urls << QUrl::fromLocalFile(base + "/outside/dropped.txt");
        urls << QUrl::fromLocalFile(base + "/outside/myfolder");
        auto *mimeData = new QMimeData;
        mimeData->setUrls(urls);

        QDragEnterEvent enterEvent(QPoint(5, 5), Qt::CopyAction, mimeData,
                                    Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view->viewport(), &enterEvent);
        check("Phase D: dragEnterEvent accepted a real OS-style file drag (hasUrls())",
              enterEvent.isAccepted());

        QDropEvent dropEvent(QPoint(5, 5), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view->viewport(), &dropEvent);

        check("Phase D: externalFilesDropped fired with both real local paths",
              capturedPaths.size() == 2
                  && capturedPaths.contains(base + "/outside/dropped.txt")
                  && capturedPaths.contains(base + "/outside/myfolder"));
    });

    // ---------- Phase E: downloadForDragOut() — a single remote file,
    // real success. A fresh TransferManager/pane pair (FakeRemoteBackend
    // above), isolated from Phases A-D. Real bug found by this test
    // itself, not the feature: TransferManager's own constructor sweeps
    // the ENTIRE shared zephyrftp-staging/ directory clean on startup
    // (the documented crash/leak backstop) — constructing Phase F's own
    // TransferManager too soon after Phase E's own downloadForDragOut()
    // call had genuinely started (its nested QEventLoop hadn't resolved
    // yet, even for a fake backend's near-instant download — there's
    // still at least one real Qt::QueuedConnection dispatch turn) wiped
    // out Phase E's own in-flight temp file out from under it. Each
    // phase below now gets a full second to itself, comfortably past
    // that window, so no two phases' own TransferManager constructions
    // can ever land while another's download is still resolving. ----------
    QTimer::singleShot(4600, &app, [&]() {
        auto *dragOutManager = new TransferManager(&app);
        auto *remotePane = new FilePaneWidget(new FakeRemoteBackend(), nullptr, nullptr, dragOutManager);
        auto *view = remotePane->findChild<FileTreeView *>();
        check("Phase E: found the remote pane's FileTreeView", view != nullptr);

        RemoteEntry entry;
        entry.name = QStringLiteral("remotefile.txt");
        QStringList tempPaths;
        const QList<QUrl> urls = view->downloadForDragOut({entry}, tempPaths);

        check("Phase E: exactly one URL and one temp path came back",
              urls.size() == 1 && tempPaths.size() == 1);
        check("Phase E: the temp file genuinely exists on disk", QFile::exists(tempPaths.value(0)));
        check("Phase E: the temp file's content is the real (fake) downloaded bytes, not empty/placeholder",
              readFile(tempPaths.value(0)) == "fake remote content of /remote/remotefile.txt");
        check("Phase E: the returned URL actually points at that same temp file",
              !urls.isEmpty() && !tempPaths.isEmpty() && urls.first() == QUrl::fromLocalFile(tempPaths.first()));

        // Same cleanup startDrag() itself does once the real OS drag
        // finishes — see FileTreeView::startDrag()'s own comment on why
        // that's safe.
        for (const QString &path : tempPaths)
            QFile::remove(path);
    });

    // ---------- Phase F: downloadForDragOut() — multiple files at once,
    // all succeeding. ----------
    QTimer::singleShot(5600, &app, [&]() {
        auto *dragOutManager = new TransferManager(&app);
        auto *remotePane = new FilePaneWidget(new FakeRemoteBackend(), nullptr, nullptr, dragOutManager);
        auto *view = remotePane->findChild<FileTreeView *>();

        RemoteEntry entryA, entryB;
        entryA.name = QStringLiteral("alpha.txt");
        entryB.name = QStringLiteral("beta.txt");
        QStringList tempPaths;
        const QList<QUrl> urls = view->downloadForDragOut({entryA, entryB}, tempPaths);

        check("Phase F: two URLs and two temp paths came back for a two-file selection",
              urls.size() == 2 && tempPaths.size() == 2);
        check("Phase F: both temp files exist with their own distinct real content",
              tempPaths.size() == 2
                  && readFile(tempPaths[0]) == "fake remote content of /remote/alpha.txt"
                  && readFile(tempPaths[1]) == "fake remote content of /remote/beta.txt");

        for (const QString &path : tempPaths)
            QFile::remove(path);
    });

    // ---------- Phase G: downloadForDragOut() — one file fails partway
    // through a multi-file selection. Must come back completely empty
    // (never a partial drag), AND must not leak the temp file for
    // whichever entry DID succeed before the failure was noticed. ----------
    QTimer::singleShot(6600, &app, [&]() {
        const QString stagingDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
            + QStringLiteral("/zephyrftp-staging");
        auto stagingFileCount = [&]() {
            return QDir(stagingDir).entryList(QDir::Files).size();
        };
        const int stagingCountBefore = stagingFileCount();

        auto *dragOutManager = new TransferManager(&app);
        auto *remotePane = new FilePaneWidget(new FakeRemoteBackend(), nullptr, nullptr, dragOutManager);
        auto *view = remotePane->findChild<FileTreeView *>();

        RemoteEntry goodEntry, badEntry;
        goodEntry.name = QStringLiteral("gamma.txt");
        badEntry.name = QStringLiteral("shouldfail.txt");   // FakeRemoteBackend::kFailMarker
        QStringList tempPaths;
        const QList<QUrl> urls = view->downloadForDragOut({goodEntry, badEntry}, tempPaths);

        check("Phase G: a partial failure returns completely empty, never a partial drag",
              urls.isEmpty() && tempPaths.isEmpty());
        check("Phase G: the one entry that DID succeed before the failure was noticed left no "
              "leaked temp file behind in the staging directory",
              stagingFileCount() == stagingCountBefore);

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}

#include "external_drop_test.moc"
