// Headless functional test of Directory Compare-and-Sync — the diff
// engine (DirectoryComparer), the bottom-up non-recursive delete
// orchestration, and an end-to-end copy against real LocalBackend
// instances and a real nested directory tree, not mocked state.
//
// Three scenarios, run sequentially against fixed /tmp paths (same
// pattern folder_transfer_test.cpp already establishes — cleaned and
// recreated at the top of main(), not relying on any external CI
// pre-seed step):
//
// Scenario 1 — diff classification correctness. Two small trees with a
// deliberate mix: only-left file, only-right file, an identical file
// (matching size+modified), a same-size/different-modified file, and a
// different-size/same-modified file — the last two specifically prove
// the comparison rule is size AND modified, not either alone (a
// regression to a single-field rule would misclassify one of them as
// Identical). Also an only-left empty directory and a directory present
// on both sides.
//
// Scenario 2 — bottom-up delete ordering. A synthetic multi-level
// "only in left" subtree (a/, a/b/, a/b/file.txt, a/c.txt) fed straight
// to CompareSyncExecutor::deleteSelected() (bypassing DirectoryComparer —
// the CompareEntry list is built by hand here, since the ordering
// property under test is the executor's own depth-sort, not the diff
// engine). Asserts the whole tree is actually gone afterward AND that
// RemoteBackend::fileOperationFailed was never emitted for it — the
// latter is what would fire if a directory were ever attempted for
// deletion before it was actually empty (RemoteBackend::deleteEntry()'s
// own documented non-recursive contract refusing a non-empty directory).
//
// Scenario 3 — end-to-end. Left is a superset of right (a kept-identical
// file, two only-in-left files, an only-in-left subdirectory containing
// a file, and a changed file). A real DirectoryComparer diff feeds a
// deliberately partial selection into CompareSyncExecutor::copySelected()
// — one only-left file is left OUT of the selection on purpose, to prove
// the executor only acts on what it's given rather than "everything
// only-in-left" implicitly. Asserts the right side's on-disk state
// exactly matches: copied files present with correct content, the
// unselected file still absent, the changed file's new content present
// (proving the Differs row's assumeOverwrite path completed without a
// TransferManager conflict dialog popping unanswered in this headless
// test — it would otherwise hang), and the nested-directory file present
// (proving destination directory creation ran before the file copy).
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QDateTime>
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"
#include "backends/RemoteBackend.h"
#include "transfer/TransferManager.h"
#include "compare/DirectoryComparer.h"
#include "compare/CompareSyncExecutor.h"

namespace {

void writeFileWithTime(const QString &path, const QString &content, const QDateTime &mtime)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(content.toUtf8());
    f.close();   // flushes buffered write data — closing THIS handle first,
                 // before setFileTime(), matters: a real bug found while
                 // writing this test had setFileTime() called before
                 // close() on the same still-open handle, and close()'s own
                 // flush silently reset the timestamp right back to "now"
                 // afterward, since it's itself a further write-affecting
                 // syscall. Reopening fresh below, with nothing left to
                 // flush, avoids that.

    QFile f2(path);
    f2.open(QIODevice::ReadWrite);   // ReadWrite, not WriteOnly — WriteOnly alone implies Truncate
    f2.setFileTime(mtime, QFileDevice::FileModificationTime);
    f2.close();
}

QString readFile(const QString &path)
{
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    return QString::fromUtf8(f.readAll());
}

const CompareEntry *findEntry(const QList<CompareEntry> &results, const QString &relativePath)
{
    for (const CompareEntry &entry : results) {
        if (entry.relativePath == relativePath)
            return &entry;
    }
    return nullptr;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const QString base = QStringLiteral("/tmp/compare_sync_test");
    QDir(base).removeRecursively();

    // --- Scenario 1 fixtures ---
    const QString s1Left = base + "/s1_left";
    const QString s1Right = base + "/s1_right";
    QDir().mkpath(s1Left + "/onlyleftdir");
    QDir().mkpath(s1Left + "/bothdir");
    QDir().mkpath(s1Right + "/bothdir");

    const QDateTime commonTime(QDate(2020, 1, 1), QTime(0, 0, 0));
    const QDateTime laterTime(QDate(2021, 6, 15), QTime(12, 0, 0));

    writeFileWithTime(s1Left + "/onlyleft.txt", "only on left", commonTime);
    writeFileWithTime(s1Right + "/onlyright.txt", "only on right", commonTime);
    writeFileWithTime(s1Left + "/same.txt", "identical payload", commonTime);
    writeFileWithTime(s1Right + "/same.txt", "identical payload", commonTime);
    // Same content (so same size), deliberately different modified times —
    // must classify Differs, not Identical: proves the rule isn't size-only.
    writeFileWithTime(s1Left + "/diffmtime.txt", "same content diff mtime", commonTime);
    writeFileWithTime(s1Right + "/diffmtime.txt", "same content diff mtime", laterTime);
    // Different content length (so different size), deliberately forced to
    // the SAME modified time — must classify Differs, not Identical:
    // proves the rule isn't modified-only.
    writeFileWithTime(s1Left + "/diffsize.txt", "short", commonTime);
    writeFileWithTime(s1Right + "/diffsize.txt", "this is a much longer piece of content", commonTime);

    // --- Scenario 2 fixtures ---
    const QString s2Left = base + "/s2_left";
    QDir().mkpath(s2Left + "/a/b");
    writeFileWithTime(s2Left + "/a/b/file.txt", "deep file", commonTime);
    writeFileWithTime(s2Left + "/a/c.txt", "shallow file", commonTime);

    // --- Scenario 3 fixtures ---
    const QString s3Left = base + "/s3_left";
    const QString s3Right = base + "/s3_right";
    QDir().mkpath(s3Left + "/onlyleftdir");
    QDir().mkpath(s3Right);
    writeFileWithTime(s3Left + "/keep.txt", "kept content", commonTime);
    writeFileWithTime(s3Right + "/keep.txt", "kept content", commonTime);
    writeFileWithTime(s3Left + "/onlyleft1.txt", "copy me", commonTime);
    writeFileWithTime(s3Left + "/onlyleft2.txt", "do not copy me", commonTime);
    // Deliberately different modified times (not just different content) —
    // same-size-same-modified content differing only in bytes is a real,
    // accepted, documented limitation of DirectoryComparer's size+modified
    // classification (no content hashing — see its own class comment), so
    // relying on that alone here would test the wrong thing.
    writeFileWithTime(s3Left + "/changed.txt", "new content", laterTime);
    writeFileWithTime(s3Right + "/changed.txt", "old content", commonTime);
    writeFileWithTime(s3Left + "/onlyleftdir/nested.txt", "nested content", commonTime);

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    auto *manager = new TransferManager(&app);

    // Scenario 2's panes/executor — deleteSelected() resolves paths via
    // FilePaneWidget::currentDirectory(), so a real pane is needed even
    // though DirectoryComparer itself is bypassed for this scenario.
    auto *s2Pane = new FilePaneWidget(new LocalBackend());
    auto *s2Executor = new CompareSyncExecutor(s2Pane, s2Pane, manager, &app);
    int s2FileOpFailures = 0;
    QObject::connect(s2Pane->backend(), &RemoteBackend::fileOperationFailed, &app,
                      [&](const QString &operation, const QString &path, const QString &reason) {
        qDebug() << "[test] UNEXPECTED fileOperationFailed (scenario 2):" << operation << path << reason;
        s2FileOpFailures++;
    });

    // Scenario 3's panes/executor.
    auto *s3LeftPane = new FilePaneWidget(new LocalBackend());
    auto *s3RightPane = new FilePaneWidget(new LocalBackend());
    auto *s3Executor = new CompareSyncExecutor(s3LeftPane, s3RightPane, manager, &app);

    QTimer::singleShot(300, &app, [&]() {
        s2Pane->navigateTo(s2Left);
        s3LeftPane->navigateTo(s3Left);
        s3RightPane->navigateTo(s3Right);
    });

    // ---------- Scenario 1: diff classification ----------
    QList<CompareEntry> s1Results;
    QTimer::singleShot(700, &app, [&]() {
        auto *leftBackend = new LocalBackend();
        auto *rightBackend = new LocalBackend();
        auto *comparer = new DirectoryComparer(leftBackend, s1Left, rightBackend, s1Right, &app);
        QObject::connect(comparer, &DirectoryComparer::failed, &app, [&](const QString &reason) {
            qDebug() << "[test] UNEXPECTED DirectoryComparer::failed (scenario 1):" << reason;
            allPass = false;
        });
        QObject::connect(comparer, &DirectoryComparer::finished, &app,
                          [&](const QList<CompareEntry> &results) {
            s1Results = results;
        });
        comparer->start();
    });

    QTimer::singleShot(1300, &app, [&]() {
        const CompareEntry *onlyLeft = findEntry(s1Results, "onlyleft.txt");
        check("onlyleft.txt classified OnlyLeft", onlyLeft && onlyLeft->status == CompareStatus::OnlyLeft);

        const CompareEntry *onlyRight = findEntry(s1Results, "onlyright.txt");
        check("onlyright.txt classified OnlyRight", onlyRight && onlyRight->status == CompareStatus::OnlyRight);

        const CompareEntry *same = findEntry(s1Results, "same.txt");
        check("same.txt (matching size+modified) classified Identical",
              same && same->status == CompareStatus::Identical);

        const CompareEntry *diffMtime = findEntry(s1Results, "diffmtime.txt");
        check("diffmtime.txt (same size, different modified) classified Differs — "
              "proves the rule isn't size-only",
              diffMtime && diffMtime->status == CompareStatus::Differs);

        const CompareEntry *diffSize = findEntry(s1Results, "diffsize.txt");
        check("diffsize.txt (different size, same modified) classified Differs — "
              "proves the rule isn't modified-only",
              diffSize && diffSize->status == CompareStatus::Differs);

        const CompareEntry *onlyLeftDir = findEntry(s1Results, "onlyleftdir");
        check("onlyleftdir classified OnlyLeft and isDir",
              onlyLeftDir && onlyLeftDir->status == CompareStatus::OnlyLeft && onlyLeftDir->isDir);

        const CompareEntry *bothDir = findEntry(s1Results, "bothdir");
        check("bothdir (present on both, empty) classified Identical",
              bothDir && bothDir->status == CompareStatus::Identical && bothDir->isDir);
    });

    // ---------- Scenario 2: bottom-up delete ordering ----------
    QTimer::singleShot(1300, &app, [&]() {
        check("scenario 2 pane actually navigated to its test directory",
              s2Pane->currentDirectory() == s2Left);

        CompareEntry dirA;
        dirA.relativePath = "a";
        dirA.isDir = true;
        dirA.status = CompareStatus::OnlyLeft;

        CompareEntry dirAB;
        dirAB.relativePath = "a/b";
        dirAB.isDir = true;
        dirAB.status = CompareStatus::OnlyLeft;

        CompareEntry fileABFile;
        fileABFile.relativePath = "a/b/file.txt";
        fileABFile.isDir = false;
        fileABFile.status = CompareStatus::OnlyLeft;

        CompareEntry fileAC;
        fileAC.relativePath = "a/c.txt";
        fileAC.isDir = false;
        fileAC.status = CompareStatus::OnlyLeft;

        // Deliberately NOT in depth order — the executor's own sort is
        // what's under test, not the caller happening to pass them sorted.
        const QList<CompareEntry> selected = {dirA, fileAC, dirAB, fileABFile};
        s2Executor->deleteSelected(selected, CompareSyncExecutor::DeleteSide::Left);
    });

    QTimer::singleShot(1900, &app, [&]() {
        check("scenario 2: the whole 'a' subtree was actually removed from disk",
              !QDir(s2Left + "/a").exists());
        check("scenario 2: no fileOperationFailed was ever emitted — "
              "proves no directory was attempted for deletion before it was actually empty",
              s2FileOpFailures == 0);
    });

    // ---------- Scenario 3: end-to-end copy ----------
    QList<CompareEntry> s3Results;
    QTimer::singleShot(1900, &app, [&]() {
        check("scenario 3 left pane actually navigated to its test directory",
              s3LeftPane->currentDirectory() == s3Left);
        check("scenario 3 right pane actually navigated to its test directory",
              s3RightPane->currentDirectory() == s3Right);

        auto *comparer = new DirectoryComparer(s3LeftPane->backend(), s3Left,
                                                 s3RightPane->backend(), s3Right, &app);
        QObject::connect(comparer, &DirectoryComparer::failed, &app, [&](const QString &reason) {
            qDebug() << "[test] UNEXPECTED DirectoryComparer::failed (scenario 3):" << reason;
            allPass = false;
        });
        QObject::connect(comparer, &DirectoryComparer::finished, &app,
                          [&](const QList<CompareEntry> &results) {
            s3Results = results;

            // Deliberate partial selection: onlyleft2.txt is a real
            // OnlyLeft row in results but is NOT included here — proving
            // copySelected() only acts on what it's given.
            QList<CompareEntry> selected;
            for (const CompareEntry &entry : results) {
                if (entry.relativePath == "onlyleft1.txt" || entry.relativePath == "changed.txt"
                    || entry.relativePath == "onlyleftdir" || entry.relativePath == "onlyleftdir/nested.txt") {
                    selected.append(entry);
                }
            }
            s3Executor->copySelected(selected, CompareSyncExecutor::CopyDirection::ToRight);
        });
        comparer->start();
    });

    QTimer::singleShot(3900, &app, [&]() {
        check("scenario 3: onlyleft1.txt (selected) was copied with correct content",
              readFile(s3Right + "/onlyleft1.txt") == "copy me");
        check("scenario 3: onlyleft2.txt (deliberately unchecked) was NOT copied",
              !QFile::exists(s3Right + "/onlyleft2.txt"));
        check("scenario 3: changed.txt (Differs, assumeOverwrite) now has the left side's content — "
              "proves the bulk-overwrite path completed without a conflict dialog popping unanswered",
              readFile(s3Right + "/changed.txt") == "new content");
        check("scenario 3: onlyleftdir/nested.txt exists with correct content — "
              "proves destination directory creation ran before the nested file copy",
              readFile(s3Right + "/onlyleftdir/nested.txt") == "nested content");

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}
