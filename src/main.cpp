#include <QApplication>
#include "ui/MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("ZephyrFTP");
    QApplication::setOrganizationName("Bad Cluster");

    MainWindow window;
    window.show();

    return QApplication::exec();
}
