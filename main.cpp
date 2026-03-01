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
        // 应用退出前，显示调用窗口的on_stopMeetingButton_clicked方法
        QMetaObject::invokeMethod(window, "on_stopMeetingButton_clicked", Qt::DirectConnection);
    });

    const int rc = a.exec();//阻塞，直到应用退出
    //操作系统自动回收内存
    return rc;
}
