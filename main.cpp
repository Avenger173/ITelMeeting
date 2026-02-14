#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    MainWindow *mw = new MainWindow;
    mw->show();
    QObject::connect(&a, &QCoreApplication::aboutToQuit, mw, &QObject::deleteLater);
    return a.exec();
}
