#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <QIcon>
#include "ui/MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("ZephyrFTP");
    QApplication::setOrganizationName("Bad Cluster");

    // Multi-resolution window/taskbar icon — Qt picks whichever size fits
    // the context (title bar, Alt-Tab, taskbar) rather than scaling one
    // image. The Windows .exe's own native icon (File Explorer, taskbar
    // before launch) is separate — see resources/app-icon.rc.
    QIcon appIcon;
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        appIcon.addFile(QStringLiteral(":/icons/app-icon-%1.png").arg(size), QSize(size, size));
    }
    QApplication::setWindowIcon(appIcon);

    // Dark theme ported from the design package's assets/zephyr-theme.css
    // (see resources/theme.qss for the token-by-token mapping notes).
    // Failure here is non-fatal — the app just falls back to Qt's default
    // platform style — so this doesn't need to be more than a warning.
    QFile themeFile(":/theme/theme.qss");
    if (themeFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QTextStream(&themeFile).readAll());
    } else {
        qWarning("Could not load theme.qss from resources — falling back to default style");
    }

    MainWindow window;
    window.show();

    return QApplication::exec();
}
