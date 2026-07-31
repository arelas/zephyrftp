// Real end-to-end test of destination-conflict resolution — Overwrite/
// Skip for files, Write Into/Skip for folders, and critically the
// "apply to all remaining conflicts" checkbox that's supposed to make
// the SECOND conflict in a batch resolve automatically without a second
// prompt.
//
// This actually drives the real QMessageBox — not a mock. A QTimer
// scheduled to fire during the dialog's still-blocking exec() call
// (exec() pumps the event loop internally, which is what makes this
// possible at all) finds it via QApplication::activeModalWidget(),
// toggles its checkbox, and clicks the matching button by text. Same
// technique already proven for capturing an open context menu in this
// project's development, applied here to actually verify behavior
// rather than just appearance.
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QCheckBox>
#include <QAbstractButton>
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"
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
    f.open(QIODevice::ReadOnly);
    return QString::fromUtf8(f.readAll());
}

// Finds the currently-open QMessageBox (if any), sets its checkbox to
// checkApplyToAll, and clicks whichever button's text contains
// buttonText (case-insensitive) — avoids needing to know the exact
// QMessageBox::StandardButton role, since these are custom buttons
// added via addButton(). Returns false (and does nothing) if no
// QMessageBox is currently open — callers use this to confirm a dialog
// that shouldn't appear (a remembered decision in effect) really didn't.
bool clickConflictDialog(bool checkApplyToAll, const QString &buttonText)
{
    auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (!box)
        return false;

    if (QCheckBox *checkbox = box->checkBox())
        checkbox->setChecked(checkApplyToAll);

    for (QAbstractButton *button : box->buttons()) {
        if (button->text().contains(buttonText, Qt::CaseInsensitive)) {
            button->click();
            return true;
        }
    }
    return false;
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Without this, closing the conflict QMessageBox (the only actually-
    // visible window in this test — the panes are constructed but never
    // shown) triggers Qt's default "quit when the last window closes"
    // behavior, ending the test's event loop before any of the later
    // phases get a chance to run.
    app.setQuitOnLastWindowClosed(false);

    const QString base = "/tmp/conflict_test";
    QDir().mkpath(base + "/src");
    QDir().mkpath(base + "/dst");

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    auto *srcPane = new FilePaneWidget(new LocalBackend());
    auto *dstPane = new FilePaneWidget(new LocalBackend());
    auto *manager = new TransferManager(&app);

    int itemsUpdatedToDone = 0;
    int itemsUpdatedToSkipped = 0;
    QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
        if (item.status == TransferStatus::Done) itemsUpdatedToDone++;
        if (item.status == TransferStatus::Skipped) itemsUpdatedToSkipped++;
    });

    // ===================================================================
    // Phase A: file conflict, Overwrite + "apply to all"
    // ===================================================================
    writeFile(base + "/src/a.txt", "NEW A");
    writeFile(base + "/src/b.txt", "NEW B");
    writeFile(base + "/dst/a.txt", "OLD A");
    writeFile(base + "/dst/b.txt", "OLD B");

    QTimer::singleShot(200, &app, [&]() {
        srcPane->navigateTo(base + "/src");
        dstPane->navigateTo(base + "/dst");
    });

    QTimer::singleShot(400, &app, [&]() {
        manager->enqueue(srcPane, dstPane, "a.txt");
        manager->enqueue(srcPane, dstPane, "b.txt");
    });

    // The dialog for a.txt should be open by now — check the box, click Overwrite.
    QTimer::singleShot(600, &app, [&]() {
        const bool clicked = clickConflictDialog(/*checkApplyToAll=*/true, "Overwrite");
        check("Phase A: conflict dialog for a.txt actually appeared", clicked);
    });

    QTimer::singleShot(1200, &app, [&]() {
        check("Phase A: a.txt was overwritten with the new content", readFile(base + "/dst/a.txt") == "NEW A");
        check("Phase A: b.txt was ALSO overwritten — remembered decision, no second prompt needed",
              readFile(base + "/dst/b.txt") == "NEW B");
        check("Phase A: exactly 2 items reached Done", itemsUpdatedToDone == 2);
    });

    // ===================================================================
    // Phase B: file conflict, Skip + "apply to all" (queue has drained
    // between phases, so the remembered decision from Phase A is gone —
    // that reset behavior is itself being exercised here, not assumed)
    // ===================================================================
    QTimer::singleShot(1600, &app, [&]() {
        writeFile(base + "/src/c.txt", "NEW C");
        writeFile(base + "/src/d.txt", "NEW D");
        writeFile(base + "/dst/c.txt", "OLD C");
        writeFile(base + "/dst/d.txt", "OLD D");

        manager->enqueue(srcPane, dstPane, "c.txt");
        manager->enqueue(srcPane, dstPane, "d.txt");
    });

    QTimer::singleShot(1800, &app, [&]() {
        const bool clicked = clickConflictDialog(/*checkApplyToAll=*/true, "Skip");
        check("Phase B: a fresh conflict dialog appeared for c.txt — proves the Phase A "
              "remembered decision reset when the queue drained, not leaked across batches", clicked);
    });

    QTimer::singleShot(2400, &app, [&]() {
        check("Phase B: c.txt was left untouched (skip, not overwrite)",
              readFile(base + "/dst/c.txt") == "OLD C");
        check("Phase B: d.txt was ALSO skipped automatically — remembered decision",
              readFile(base + "/dst/d.txt") == "OLD D");
        check("Phase B: exactly 2 items reached Skipped", itemsUpdatedToSkipped == 2);
    });

    // ===================================================================
    // Phase C: directory conflict, Skip — nothing should be enumerated
    // or transferred at all
    // ===================================================================
    bool sawFolderSkipped = false;
    bool sawFolderStartedDuringPhaseC = false;
    QObject::connect(manager, &TransferManager::folderTransferSkipped, &app, [&](const QString &name) {
        if (name == "confdir") sawFolderSkipped = true;
    });
    QObject::connect(manager, &TransferManager::folderTransferStarted, &app, [&](const QString &name) {
        if (name == "confdir") sawFolderStartedDuringPhaseC = true;
    });

    QTimer::singleShot(2800, &app, [&]() {
        QDir().mkpath(base + "/src/confdir");
        writeFile(base + "/src/confdir/e.txt", "should never arrive");
        QDir().mkpath(base + "/dst/confdir");   // pre-existing conflict

        manager->enqueueFolder(srcPane, dstPane, "confdir");
    });

    QTimer::singleShot(3300, &app, [&]() {
        const bool clicked = clickConflictDialog(/*checkApplyToAll=*/false, "Skip");
        check("Phase C: folder conflict dialog appeared for confdir", clicked);
    });

    QTimer::singleShot(3700, &app, [&]() {
        check("Phase C: folderTransferSkipped fired", sawFolderSkipped);
        check("Phase C: enumeration never started (folderTransferStarted never fired) — "
              "skip happens before paying for a walk of the source tree",
              !sawFolderStartedDuringPhaseC);
        check("Phase C: e.txt never arrived in the destination folder",
              !QFile::exists(base + "/dst/confdir/e.txt"));
    });

    // ===================================================================
    // Phase D: directory conflict, Write Into — should merge, not replace.
    // Given its own independent top-level timers (not nested inside
    // Phase C's completion callback) with generous margins — an earlier
    // version nested this and used tighter timing, which passed most of
    // the time but occasionally missed the dialog: adding debug logging
    // to investigate the failure made it start passing reliably again,
    // which is itself the signature of a timing race, not a real logic
    // bug (confirmed by rerunning with the same margins the debug prints
    // had incidentally introduced). Fixed properly here with real margin
    // instead of relying on incidental logging overhead.
    // ===================================================================
    QTimer::singleShot(4000, &app, [&]() {
        QDir().mkpath(base + "/src/mergedir");
        writeFile(base + "/src/mergedir/f.txt", "new file via write-into");
        QDir().mkpath(base + "/dst/mergedir");
        writeFile(base + "/dst/mergedir/preexisting.txt", "already here before the transfer");

        manager->enqueueFolder(srcPane, dstPane, "mergedir");
    });

    QTimer::singleShot(4500, &app, [&]() {
        const bool clicked = clickConflictDialog(/*checkApplyToAll=*/false, "Write Into");
        check("Phase D: folder conflict dialog appeared for mergedir", clicked);
    });

    QTimer::singleShot(5300, &app, [&]() {
        check("Phase D: the new file was transferred into the existing folder",
              readFile(base + "/dst/mergedir/f.txt") == "new file via write-into");
        check("Phase D: the pre-existing file in that folder was left alone (merge, not replace)",
              readFile(base + "/dst/mergedir/preexisting.txt") == "already here before the transfer");

        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}
