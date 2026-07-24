#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include "ui/MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("ZephyrFTP");
    QApplication::setOrganizationName("Bad Cluster");

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
