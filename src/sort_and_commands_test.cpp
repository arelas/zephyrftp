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

#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTreeView>
#include <QAbstractItemModel>
#include <QItemSelectionModel>
#include <QPlainTextEdit>
#include "ui/FilePaneWidget.h"
#include "ui/CommandsPaneWidget.h"
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
    void requestCancel() override {}
    void requestPause() override {}

    // Not a slot — called directly by the test, same process/thread, no
    // dispatch needed.
    void emitFakeLine(const QString &line) { emit commandLogged(line); }

public slots:
    void connectToHost() override { emit connected(); }
    void listDirectory(const QString &path) override { emit directoryListed(path, {}); }
    void downloadFile(const QString &, const QString &, qint64 = 0) override {}
    void uploadFile(const QString &, const QString &, qint64 = 0) override {}
    void deleteEntry(const QString &, bool) override {}
    void renameEntry(const QString &, const QString &) override {}
    void createDirectory(const QString &) override {}
    void createFile(const QString &) override {}
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
        check("directory listing has all 4 entries", model->rowCount() == 4);

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
                  && textAt(3, 0) == QStringLiteral("bravo.txt"));

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

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}

#include "sort_and_commands_test.moc"
