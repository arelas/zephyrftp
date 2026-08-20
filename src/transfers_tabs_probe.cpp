// Disposable probe — NOT part of the required suite. Screenshots the
// Transfers pane's Active/Completed/Failed tabs with real "(N)" counts
// (the case most likely to clip) for visual verification of the
// QTabBar::setElideMode(Qt::ElideNone) fix. Removed after use.
#include <QApplication>
#include <QStandardPaths>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QEventLoop>
#include "ui/TransferQueueWidget.h"
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"
#include "transfer/TransferManager.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);

    const QString base = "/tmp/transfers_tabs_probe";
    QDir(base).removeRecursively();
    QDir().mkpath(base + "/src");
    QDir().mkpath(base + "/dst");
    QFile ok(base + "/src/clear_ok.txt");
    ok.open(QIODevice::WriteOnly);
    ok.write("hi");
    ok.close();

    auto *manager = new TransferManager(&app);
    auto *widget = new TransferQueueWidget(manager);
    // A moderate, realistic docked-panel width — not generous enough to
    // trivially avoid ever exercising the bug, not so narrow it clips
    // regardless of the fix.
    widget->resize(420, 300);
    widget->show();
    widget->raise();
    widget->activateWindow();

    auto *leftPane = new FilePaneWidget(new LocalBackend());
    auto *rightPane = new FilePaneWidget(new LocalBackend());
    leftPane->navigateTo(base + "/src");
    rightPane->navigateTo(base + "/dst");

    QTimer::singleShot(300, &app, [&]() {
        manager->enqueue(leftPane, rightPane, "clear_ok.txt");
        manager->enqueue(leftPane, rightPane, "clear_missing_1.bin");
        manager->enqueue(leftPane, rightPane, "clear_missing_2.bin");

        QEventLoop settle;
        QTimer::singleShot(600, &settle, &QEventLoop::quit);
        settle.exec();

        qDebug() << "[probe] widget ready, one Completed + two Failed items should be showing";
    });

    return app.exec();
}
