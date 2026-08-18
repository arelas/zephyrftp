// Headless regression test for two UI-polish features added after v0.3.5
// that ARCHITECTURE.md previously flagged as verified only by
// screenshotting the running app, not by any automated test:
//
//   1. CommandsPaneWidget's live log, and FilePaneWidget's forwarding of
//      RemoteBackend::commandLogged — including that a setBackend() swap
//      (Connect/Disconnect/reconnect) re-targets the forwarding to the
//      NEW backend and stops forwarding the old one's traffic.
//   2. FilePaneWidget's default sort order (folders first, then name
//      descending, applied uniformly across backends), numeric Size
//      sorting via a real header-column sort, and — the specific bug
//      sorting surfaced — that entryForRow() still maps a view row to
//      the correct RemoteEntry once a sort has physically reordered the
//      model's rows, rather than the old row-position indexing that
//      broke the instant rows moved.
//
// Part 1 uses a minimal fake RemoteBackend (same legitimate test-double
// technique transfer_pause_test.cpp's FakePausableBackend uses:
// FilePaneWidget only ever talks to the RemoteBackend interface, so a
// fake honoring that interface's contract exercises the same forwarding
// code a real backend would) — no live server needed, since the actual
// wire-level correctness of what SftpBackend/FtpBackend choose to log
// (including PASS-masking) is already covered by the manual verification
// described in ARCHITECTURE.md's CommandsPaneWidget entry.
//
// Part 2 uses a real LocalBackend against a real temp directory, same
// pattern navigation-test/file-operations-test use, and drives sorting
// via QTreeView::sortByColumn() directly — the exact call Qt's own
// header-click handling invokes once setSortingEnabled(true) is set, so
// this is a faithful simulation of an actual header click, not a
// shortcut around it.
//
// A later addition, unrelated to either of the above: IconTheme::
// tintedIcon()'s own per-(path, color, size) cache, added after code
// review found rebuildModel()'s per-row iconForEntry() calls re-parsing
// and re-painting the same handful of icons from scratch on every
// directory listing. Tested directly against IconTheme (no FilePaneWidget
// needed) since it's a pure function of its three arguments.

#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTreeView>
#include <QAbstractItemModel>
#include <QItemSelectionModel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QMimeData>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QImage>
#include <QStandardPaths>
#include <QLineEdit>
#include "AppSettings.h"
#include "ui/FilePaneWidget.h"
#include "ui/FileTreeView.h"
#include "ui/CommandsPaneWidget.h"
#include "ui/IconTheme.h"
#include "backends/LocalBackend.h"
#include "backends/RemoteBackend.h"

namespace {

// Bare-minimum fake: only the connected()/directoryListed() handshake
// setBackend() triggers automatically and commandLogged() actually
// matter here — everything else is an unused stub required only to
// satisfy RemoteBackend's pure-virtual interface (same shape as
// transfer_pause_test.cpp's FakePausableBackend).
class FakeLoggingBackend : public RemoteBackend {
    Q_OBJECT
public:
    explicit FakeLoggingBackend(QObject *parent = nullptr) : RemoteBackend(parent) {}

    QString currentPath() const override { return QStringLiteral("/fake"); }
    bool isLocalFilesystem() const override { return false; }
    QString connectionIdentity() const override { return QStringLiteral("fake"); }
    void requestCancel() override {}
    void requestPause() override {}

    // Not a slot — called directly by the test, same process/thread, no
    // dispatch needed.
    void emitFakeLine(const QString &line) { emit commandLogged(line); }

    // Same reasoning — a direct-call test hook, not routed through
    // listDirectory()'s own hardcoded-empty stub. Used to hand
    // FilePaneWidget a fabricated listing (e.g. containing a dotfile)
    // that a REAL LocalBackend fixture can't produce, since LocalBackend
    // itself never returns dotfiles at all (see rebuildModel()'s own
    // comment on that pre-existing inconsistency).
    void emitFakeListing(const QString &path, const QList<RemoteEntry> &entries) {
        emit directoryListed(path, entries);
    }

public slots:
    void connectToHost() override { emit connected(); }
    void listDirectory(const QString &path) override { emit directoryListed(path, {}); }
    void downloadFile(const QString &, const QString &, qint64 = 0) override {}
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
    void checkExists(const QString &, int) override {}
};

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    // ---------- Part 1: CommandsPaneWidget on its own ----------
    auto *commandsPane = new CommandsPaneWidget();
    auto *log = commandsPane->findChild<QPlainTextEdit *>();
    check("CommandsPaneWidget has a QPlainTextEdit log", log != nullptr);
    check("log starts with a welcome line, not blank",
          log && !log->toPlainText().trimmed().isEmpty());

    commandsPane->appendLine(QStringLiteral("Status: Connecting to fake.example"));
    commandsPane->appendLine(QStringLiteral("Command: LIST /"));
    const QString logText = log ? log->toPlainText() : QString();
    check("appendLine() appends both lines, in order",
          logText.contains(QStringLiteral("Status: Connecting to fake.example"))
              && logText.contains(QStringLiteral("Command: LIST /"))
              && logText.indexOf(QStringLiteral("Connecting"))
                     < logText.indexOf(QStringLiteral("Command: LIST")));

    // ---------- Regression: appendLine() must not force-scroll to the
    // bottom if the user had already scrolled away to review an earlier
    // line — a real bug found by code review. Confirmed directly (a
    // standalone probe, not assumed) that QPlainTextEdit::appendPlainText()
    // already preserves scroll position on its own; the old code
    // overrode that with an unconditional
    // scrollBar->setValue(scrollBar->maximum()) on every single call,
    // yanking the view straight back down the instant any new line
    // arrived — undermining the whole point of being able to review
    // scrollback while the log is still live. ----------
    {
        auto *scrollPane = new CommandsPaneWidget();
        auto *scrollLog = scrollPane->findChild<QPlainTextEdit *>();
        scrollPane->resize(300, 100);
        scrollPane->show();
        for (int i = 0; i < 100; ++i)
            scrollPane->appendLine(QStringLiteral("line %1").arg(i));

        QScrollBar *scrollBar = scrollLog->verticalScrollBar();
        check("scroll test setup: enough lines to actually need scrolling",
              scrollBar->maximum() > 0);

        scrollBar->setValue(0);   // scroll all the way up, as if re-reading an earlier line
        scrollPane->appendLine(QStringLiteral("new line while scrolled up"));
        check("appendLine() while scrolled away does NOT force the view back to the bottom",
              scrollBar->value() == 0);

        scrollBar->setValue(scrollBar->maximum());   // back at the bottom, tailing live
        scrollPane->appendLine(QStringLiteral("another new line while already at the bottom"));
        check("appendLine() while already at the bottom keeps tailing (stays pinned to the end)",
              scrollBar->value() == scrollBar->maximum());
    }

    // ---------- Part 2: FilePaneWidget forwards the CURRENTLY attached
    // backend's commandLogged, and re-targets (not adds to) that
    // forwarding across a setBackend() swap ----------
    auto *backendA = new FakeLoggingBackend();
    auto *pane = new FilePaneWidget(backendA);

    QString lastForwarded;
    int forwardedCount = 0;
    QObject::connect(pane, &FilePaneWidget::commandLogged, &app, [&](const QString &line) {
        lastForwarded = line;
        ++forwardedCount;
    });

    backendA->emitFakeLine(QStringLiteral("from A"));
    check("pane forwards the attached backend's commandLogged",
          forwardedCount == 1 && lastForwarded == QStringLiteral("from A"));

    auto *backendB = new FakeLoggingBackend();
    pane->setBackend(backendB);   // setBackend() disconnects the old backend synchronously
    backendA->emitFakeLine(QStringLiteral("stale from A"));
    check("pane stops forwarding the OLD backend's commandLogged after setBackend()",
          lastForwarded != QStringLiteral("stale from A"));

    backendB->emitFakeLine(QStringLiteral("from B"));
    check("pane forwards the NEW backend's commandLogged after setBackend()",
          lastForwarded == QStringLiteral("from B"));

    // ---------- Regression: IconTheme::tintedIcon() must cache by
    // (resourcePath, color, size) rather than re-rendering the SVG from
    // scratch on every call — a real efficiency issue found by code
    // review, since rebuildModel()'s per-row iconForEntry() calls this
    // for every entry in a directory listing. Correctness first: two
    // calls with identical arguments must still produce pixel-identical
    // results (proves the cache doesn't return something stale/wrong),
    // and calls that differ in EACH of the three key fields must still
    // produce genuinely different icons (proves the cache key actually
    // discriminates, not just always hitting on the first call). Then
    // the actual performance claim, via wall-clock timing: many repeated
    // calls with the SAME key should be dramatically faster than the
    // same count of calls that are all cache MISSES (a distinct color
    // each time, forcing a real render every time) — if the cache
    // weren't doing anything, these two durations would be roughly
    // equal instead. ----------
    {
        const QString iconPath = QStringLiteral(":/icons/folder.svg");

        const QImage first = IconTheme::tintedIcon(iconPath, IconTheme::Blue, 20).pixmap(20).toImage();
        const QImage second = IconTheme::tintedIcon(iconPath, IconTheme::Blue, 20).pixmap(20).toImage();
        check("IconTheme cache: two calls with identical arguments produce pixel-identical icons",
              !first.isNull() && first == second);

        const QImage differentColor = IconTheme::tintedIcon(iconPath, IconTheme::Red, 20).pixmap(20).toImage();
        check("IconTheme cache: a different color produces a genuinely different icon "
              "(the cache key isn't ignoring color)",
              !differentColor.isNull() && differentColor != first);

        const QImage differentSize = IconTheme::tintedIcon(iconPath, IconTheme::Blue, 32).pixmap(32).toImage();
        check("IconTheme cache: a different size produces a genuinely different icon "
              "(the cache key isn't ignoring size)",
              !differentSize.isNull() && differentSize.size() != first.size());

        const QString otherPath = QStringLiteral(":/icons/file.svg");
        const QImage differentPath = IconTheme::tintedIcon(otherPath, IconTheme::Blue, 20).pixmap(20).toImage();
        check("IconTheme cache: a different resource path produces a genuinely different icon "
              "(the cache key isn't ignoring the path)",
              !differentPath.isNull() && differentPath != first);

        // Warm the cache for iconPath/Blue/20, then time repeated HITS
        // against repeated MISSES (a fresh, never-before-used color each
        // time, via distinct QColor values, so every call is forced to
        // actually render).
        IconTheme::tintedIcon(iconPath, IconTheme::Blue, 20);
        constexpr int iterations = 300;

        QElapsedTimer hitTimer;
        hitTimer.start();
        for (int i = 0; i < iterations; ++i)
            IconTheme::tintedIcon(iconPath, IconTheme::Blue, 20);
        const qint64 hitElapsed = hitTimer.nsecsElapsed();

        QElapsedTimer missTimer;
        missTimer.start();
        for (int i = 0; i < iterations; ++i)
            IconTheme::tintedIcon(iconPath, QColor(i % 256, (i * 3) % 256, (i * 7) % 256), 20);
        const qint64 missElapsed = missTimer.nsecsElapsed();

        qDebug() << "[test] IconTheme cache timing:" << iterations << "cache hits took" << hitElapsed / 1000
                 << "us," << iterations << "cache misses took" << missElapsed / 1000 << "us";
        check("IconTheme cache: repeated cache HITS are substantially faster than the same "
              "number of forced cache MISSES — proves the cache is actually short-circuiting "
              "the SVG render, not just coincidentally correct",
              hitElapsed < missElapsed / 2);
    }

    // ---------- Part 3: default sort order, numeric Size sort via a real
    // header-column sort, and entryForRow()'s row-independence — real
    // LocalBackend, real temp directory ----------
    const QString base = QStringLiteral("/tmp/sort_test");
    QDir(base).removeRecursively();
    QDir().mkpath(base + "/zulu");    // dir
    QDir().mkpath(base + "/alpha");   // dir
    {
        QFile bravo(base + "/bravo.txt");   // file, 100 bytes
        bravo.open(QIODevice::WriteOnly);
        bravo.write(QByteArray(100, 'x'));
    }
    {
        QFile delta(base + "/delta.txt");   // file, 300 bytes
        delta.open(QIODevice::WriteOnly);
        delta.write(QByteArray(300, 'x'));
    }
    {
        // Starts with uppercase 'A' (0x41) — for the Name-header-sort
        // regression below: well before '[' (0x5B), the character every
        // directory's bracketed display text starts with, so this file
        // would sort ahead of every directory under plain ASCII
        // code-point comparison (which is exactly the bug).
        QFile aaa(base + "/Aaa.txt");
        aaa.open(QIODevice::WriteOnly);
        aaa.write("x");
    }

    auto *sortPane = new FilePaneWidget(new LocalBackend());
    auto *view = sortPane->findChild<QTreeView *>();
    check("found the file pane's QTreeView", view != nullptr);

    // navigateTo() dispatches asynchronously (Qt::QueuedConnection), same
    // as every other navigation test in this project — sequenced with
    // QTimer::singleShot rather than assumed instantaneous. The first
    // delay lets the pane's own construction-time home-directory listing
    // settle before overriding it with our target directory.
    QTimer::singleShot(200, &app, [&, view]() {
        Q_UNUSED(view);
        sortPane->navigateTo(base);
    });

    QTimer::singleShot(500, &app, [&, view]() {
        auto *model = view->model();
        check("directory listing has all 5 entries", model->rowCount() == 5);

        // Column indices mirror FilePaneWidget.cpp's own (private, header-
        // less) ColName=0/ColSize=1 constants.
        auto textAt = [&](int row, int col) {
            return model->data(model->index(row, col), Qt::DisplayRole).toString();
        };

        // Default order: folders first, then name descending — see
        // rebuildModel()'s std::stable_sort. Directory names render
        // bracketed ("[name]"); this is the display text, not the real
        // name entryForRow() matches against.
        check("default order: folders first, then name descending",
              textAt(0, 0) == QStringLiteral("[zulu]")
                  && textAt(1, 0) == QStringLiteral("[alpha]")
                  && textAt(2, 0) == QStringLiteral("delta.txt")
                  && textAt(3, 0) == QStringLiteral("bravo.txt")
                  && textAt(4, 0) == QStringLiteral("Aaa.txt"));

        // A real header-column sort: this is the exact call Qt's own
        // header-click handling invokes once setSortingEnabled(true) is
        // set (buildUi()), so it's a faithful stand-in for an actual
        // click, not a shortcut around it. Ascending by Size: dirs sort
        // first (SizeItem's SortDataRole is -1 for a directory), then
        // files ascending by real byte count.
        view->sortByColumn(1, Qt::AscendingOrder);

        int bravoRow = -1, deltaRow = -1;
        for (int row = 0; row < model->rowCount(); ++row) {
            if (textAt(row, 0) == QStringLiteral("bravo.txt")) bravoRow = row;
            if (textAt(row, 0) == QStringLiteral("delta.txt")) deltaRow = row;
        }
        check("both files found after the Size sort", bravoRow >= 0 && deltaRow >= 0);
        check("numeric Size sort: bravo.txt (100 bytes) sorts before delta.txt (300 bytes) ascending",
              bravoRow >= 0 && deltaRow >= 0 && bravoRow < deltaRow);

        // The actual regression check for entryForRow(): select exactly
        // the row bravo.txt now occupies (which the Size sort just moved
        // away from its default-order position) and confirm
        // selectedFileNames() — which looks the entry up via
        // entryForRow(row) — returns bravo.txt, not whatever entry used
        // to sit at that row position before the sort. The bug this
        // fixed indexed a parallel list by row position directly, which
        // would silently return the wrong file (or a directory) here
        // once sorting moved rows around.
        if (bravoRow >= 0) {
            view->selectionModel()->select(
                model->index(bravoRow, 0),
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            const QStringList selected = sortPane->selectedFileNames();
            check("entryForRow() maps the post-sort row to the correct entry (bravo.txt), "
                  "not a stale position-indexed one",
                  selected.size() == 1 && selected.first() == QStringLiteral("bravo.txt"));
        } else {
            check("entryForRow() row-independence check", false);
        }

        // ---------- Regression: clicking the Name header must keep
        // folders grouped together, not interleave them with files by
        // sorting on the raw bracketed "[Folder]" display text via plain
        // ASCII code-point order — a real bug found by code review.
        // Aaa.txt starts with 'A' (0x41), well before '[' (0x5B, what
        // every directory's display text starts with), so under the old
        // buggy comparator it would sort ahead of BOTH directories —
        // breaking the folders-first grouping every other order in this
        // app maintains. ----------
        view->sortByColumn(0, Qt::AscendingOrder);

        int alphaRow = -1, zuluRow = -1, aaaRow = -1;
        for (int row = 0; row < model->rowCount(); ++row) {
            if (textAt(row, 0) == QStringLiteral("[alpha]")) alphaRow = row;
            if (textAt(row, 0) == QStringLiteral("[zulu]")) zuluRow = row;
            if (textAt(row, 0) == QStringLiteral("Aaa.txt")) aaaRow = row;
        }
        check("all three rows found after the Name sort", alphaRow >= 0 && zuluRow >= 0 && aaaRow >= 0);
        check("Name sort: both folders stayed grouped together ahead of Aaa.txt, not interleaved "
              "by the bracket character's ASCII position",
              alphaRow >= 0 && zuluRow >= 0 && aaaRow >= 0
                  && alphaRow < aaaRow && zuluRow < aaaRow);
        check("Name sort: folders themselves are still ordered by real name (alpha before zulu), "
              "not the bracketed display text",
              alphaRow >= 0 && zuluRow >= 0 && alphaRow < zuluRow);

        // ---------- Regression: rebuildModel() must preserve the current
        // selection across a same-directory refresh. A real bug:
        // rebuildModel() used to clear and rebuild the model unconditionally
        // with no attempt to preserve selection, so Refresh (navigateTo()
        // the SAME directory again — exactly what's triggered here) or
        // toggling "Show hidden files" mid-selection silently dropped
        // whatever was selected, losing the target set of a pending
        // Transfer/Move with no indication why. bravo.txt is still
        // selected from the check just above; re-navigating to the same
        // directory re-enters onDirectoryListed() -> rebuildModel() via
        // the exact real path a Refresh click uses. ----------
        sortPane->navigateTo(base);
    });

    QTimer::singleShot(800, &app, [&]() {
        const QStringList stillSelected = sortPane->selectedFileNames();
        check("selection survived a same-directory refresh (bravo.txt still selected)",
              stillSelected.size() == 1 && stillSelected.first() == QStringLiteral("bravo.txt"));

        // ---------- Filename filter: narrows rowCount(), clearing
        // restores the full set, and matching is case-insensitive
        // substring — reuses this fixture's already-known 5 entries
        // (zulu/, alpha/, delta.txt, bravo.txt, Aaa.txt). ----------
        {
            auto *filterModel = sortPane->findChild<QTreeView *>()->model();
            auto filterTextAt = [&](int row) {
                return filterModel->data(filterModel->index(row, 0), Qt::DisplayRole).toString();
            };
            auto *filterEdit = sortPane->findChild<QLineEdit *>(QStringLiteral("filterEdit"));
            check("found the file pane's filter QLineEdit", filterEdit != nullptr);
            if (filterEdit) {
                filterEdit->setText(QStringLiteral("ravo"));
                check("filter \"ravo\": narrows to exactly bravo.txt",
                      filterModel->rowCount() == 1 && filterTextAt(0) == QStringLiteral("bravo.txt"));

                filterEdit->setText(QStringLiteral("RAVO"));
                check("filter is case-insensitive (\"RAVO\" still matches bravo.txt)",
                      filterModel->rowCount() == 1 && filterTextAt(0) == QStringLiteral("bravo.txt"));

                filterEdit->setText(QStringLiteral("zzz-no-match"));
                check("filter with no matches: rowCount is 0", filterModel->rowCount() == 0);

                filterEdit->clear();
                check("clearing the filter restores the full 5-entry listing",
                      filterModel->rowCount() == 5);
            }
        }

        // ---------- Regression: rebuildModel()'s selection-restore-by-
        // name must NOT fire across a genuine navigation to a DIFFERENT
        // directory, even when a same-named file exists there — a real
        // bug found by code review (the old comment's assumption was
        // that an old name "essentially never" matches an unrelated
        // directory's listing, which is false for common names).
        // bravo.txt is still selected from the check just above; this
        // sibling directory has its OWN, unrelated bravo.txt. ----------
        const QString siblingBase = QStringLiteral("/tmp/sort_test_sibling");
        QDir(siblingBase).removeRecursively();
        QDir().mkpath(siblingBase);
        QFile siblingBravo(siblingBase + "/bravo.txt");
        siblingBravo.open(QIODevice::WriteOnly);
        siblingBravo.write(QByteArray(50, 'y'));
        siblingBravo.close();

        sortPane->navigateTo(siblingBase);
    });

    QTimer::singleShot(1100, &app, [&]() {
        check("directory-change regression: landed on the sibling directory",
              sortPane->currentDirectory() == QStringLiteral("/tmp/sort_test_sibling"));
        check("directory-change regression: bravo.txt was NOT auto-selected in the new "
              "directory just because a same-named file happens to live there too",
              sortPane->selectedFileNames().isEmpty());

        // ---------- Regression: FileTreeView::dropEvent() must reject a
        // drop whose MIME payload doesn't carry a token matching THIS
        // process's own per-process random token — the real bug this
        // closes: dragging between two SEPARATE ZephyrFTP processes used
        // to reconstruct and genuinely dereference the OTHER process's
        // raw FilePaneWidget pointer (see FileTreeView.h's own comment).
        // Can't spawn a second real process here, but the property under
        // test is exactly "a drop carrying the wrong token is safely
        // ignored, with the pointer bytes never even touched" — proven
        // directly by forging a drop with a deliberately wrong token and
        // a deliberately poisoned pointer value that would almost
        // certainly crash this process outright if it were ever actually
        // dereferenced. No crash occurring (this test process is still
        // running to check the result at all) plus filesDroppedFrom
        // never firing is the proof. ----------
        auto *dropView = sortPane->findChild<FileTreeView *>();
        check("found the file pane's FileTreeView", dropView != nullptr);

        bool sawFilesDropped = false;
        QObject::connect(dropView, &FileTreeView::filesDroppedFrom, &app,
                          [&](FilePaneWidget *, const QList<RemoteEntry> &) {
            sawFilesDropped = true;
        });

        auto sendForgedDrop = [&](const QByteArray &sourcePanePayload) {
            auto *mimeData = new QMimeData;
            mimeData->setData(QStringLiteral("application/x-zephyrftp-sourcepane"), sourcePanePayload);
            mimeData->setData(QStringLiteral("application/x-zephyrftp-filenames"),
                               QByteArrayLiteral("0\tsomefile.txt"));
            // Sent to the view's VIEWPORT, not the QTreeView/FileTreeView
            // widget itself — QAbstractScrollArea (which QTreeView derives
            // from) delivers mouse/drag-and-drop events to its internal
            // viewport child widget, not the outer widget directly;
            // confirmed directly (a send to dropView itself never reached
            // dropEvent() at all) before landing on this.
            QDragEnterEvent enterEvent(QPoint(5, 5), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(dropView->viewport(), &enterEvent);
            QDropEvent dropEvent(QPointF(5, 5), Qt::CopyAction, mimeData,
                                  Qt::LeftButton, Qt::NoModifier, QEvent::Drop);
            QApplication::sendEvent(dropView->viewport(), &dropEvent);
        };

        // Wrong token (16 bytes: 8-byte bogus token + 8-byte poison
        // pointer). The poison pointer value would be a near-certain
        // crash if this were ever actually dereferenced as a
        // FilePaneWidget*.
        const quint64 wrongToken = 0xDEADBEEFCAFEBABEULL;
        const quintptr poisonPtr = 0x4141414141414141ULL;
        QByteArray forgedPayload(reinterpret_cast<const char *>(&wrongToken), sizeof(wrongToken));
        forgedPayload.append(reinterpret_cast<const char *>(&poisonPtr), sizeof(poisonPtr));
        sendForgedDrop(forgedPayload);

        check("a drop with a wrong token was safely rejected — filesDroppedFrom never fired",
              !sawFilesDropped);

        // Old-format payload (just a lone pointer, no token at all —
        // what a pre-fix version of this app would have sent) must also
        // be safely rejected, on size mismatch alone.
        sawFilesDropped = false;
        QByteArray oldFormatPayload(reinterpret_cast<const char *>(&poisonPtr), sizeof(poisonPtr));
        sendForgedDrop(oldFormatPayload);
        check("an old-format (pre-fix, token-less) payload was safely rejected on size mismatch alone",
              !sawFilesDropped);

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    // ---------- Filename filter composes with showHiddenFiles as AND,
    // not two independent filters that happen to both work alone — a
    // dotfile matching the filter text must stay excluded while
    // showHiddenFiles is off, and appear once it's turned on. Needs a
    // fake backend supplying a dotfile directly: LocalBackend itself
    // never returns dotfiles at all (see rebuildModel()'s own comment),
    // so the fixture above (real LocalBackend) can't exercise this.
    // Fully synchronous — emitFakeListing() is a direct call, not routed
    // through Qt::QueuedConnection, so no event-loop wait is needed
    // (same reasoning Part 2's emitFakeLine() checks already rely on).
    // QStandardPaths::setTestModeEnabled(true) isolates the real
    // AppSettings instance this needs (to actually toggle
    // showHiddenFiles) from a real developer's own settings.json. ----------
    {
        QStandardPaths::setTestModeEnabled(true);
        auto *hiddenSettings = new AppSettings();
        // A real bug found by running this test twice in a row: the
        // test-mode settings.json path persists ACROSS separate runs of
        // this binary (unlike a fresh in-memory default), so a
        // freshly-constructed AppSettings here can start with
        // showHiddenFiles() already true if a previous run (of this
        // test, or any other AppSettings-touching test sharing the same
        // test-mode directory) last left it that way — silently
        // invalidating the "starts off" assumption the first check below
        // depends on. Explicit, not assumed.
        hiddenSettings->setShowHiddenFiles(false);
        auto *hiddenBackend = new FakeLoggingBackend();
        auto *hiddenPane = new FilePaneWidget(hiddenBackend, nullptr, hiddenSettings);

        RemoteEntry alpha;
        alpha.name = QStringLiteral("alpha.txt");
        alpha.isDir = false;
        RemoteEntry hiddenAlpha;
        hiddenAlpha.name = QStringLiteral(".alpha_secret");
        hiddenAlpha.isDir = false;
        hiddenBackend->emitFakeListing(QStringLiteral("/fake"), {alpha, hiddenAlpha});

        auto *hiddenModel = hiddenPane->findChild<QTreeView *>()->model();
        auto *hiddenFilterEdit = hiddenPane->findChild<QLineEdit *>(QStringLiteral("filterEdit"));
        check("filter+hidden composition fixture: found the filter field", hiddenFilterEdit != nullptr);
        if (hiddenFilterEdit) {
            hiddenFilterEdit->setText(QStringLiteral("alpha"));
            check("filter+hidden composition: a dotfile matching the filter text stays "
                  "excluded while showHiddenFiles is off",
                  hiddenModel->rowCount() == 1);

            hiddenSettings->setShowHiddenFiles(true);
            check("filter+hidden composition: turning showHiddenFiles on reveals the "
                  "matching dotfile too (both rows now match \"alpha\")",
                  hiddenModel->rowCount() == 2);
        }
    }

    return app.exec();
}

#include "sort_and_commands_test.moc"
