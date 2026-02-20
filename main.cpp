#include "mainwindow.h"

#include <QApplication>
#include <QMetaObject>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    auto *window = new MainWindow;
    window->show();

    QObject::connect(&a, &QCoreApplication::aboutToQuit, window, [window]() {
        if (!window) return;
        // Run one explicit stop path before app shutdown.
        QMetaObject::invokeMethod(window, "on_stopMeetingButton_clicked", Qt::DirectConnection);
    });

    const int rc = a.exec();
    // Avoid complex teardown at process end; OS reclaims memory.
    return rc;
}
