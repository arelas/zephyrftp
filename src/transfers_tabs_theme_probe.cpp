// Disposable probe — NOT part of the required suite. Screenshots the
// Transfers pane's tabs with the app's REAL dark theme QSS actually
// loaded (unlike the earlier transfers-tabs-probe, which only tested
// text clipping and didn't need theming) — the whole point this time
// is confirming the tab bar's own background follows the APP's theme
// choice, not the macOS system appearance, and that there's no native
// visual seam between the tab bar and the pane below it. Removed
// after use.
#include <QApplication>
#include <QStandardPaths>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QEventLoop>
#include "ui/TransferQueueWidget.h"
#include "ui/FilePaneWidget.h"
#include "backends/LocalBackend.h"
#include "transfer/TransferManager.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);

    // Same order main.cpp itself uses: the alignment fix must run
    // before the stylesheet is applied — replicated here since that's
    // exactly the mechanism under test.
    TransferQueueWidget::installTabBarAlignmentFix();

    // Same theme-loading step main.cpp itself does — real dark theme,
    // not the platform default this probe would otherwise render with.
    QFile themeFile(":/theme/theme.qss");
    if (themeFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QTextStream(&themeFile).readAll());
        qDebug() << "[probe] theme.qss loaded, length:" << app.styleSheet().length();
    } else {
        qDebug() << "[probe] FAILED to load theme.qss";
        return 1;
    }

    const QString base = "/tmp/transfers_tabs_theme_probe";
    QDir(base).removeRecursively();
    QDir().mkpath(base + "/src");
    QDir().mkpath(base + "/dst");
    QFile ok(base + "/src/clear_ok.txt");
    ok.open(QIODevice::WriteOnly);
    ok.write("hi");
    ok.close();

    auto *manager = new TransferManager(&app);
    auto *widget = new TransferQueueWidget(manager);
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

        qDebug() << "[probe] widget ready with real dark theme applied";
    });

    return app.exec();
}
