/********************************************************************************
** Form generated from reading UI file 'mainwindow.ui'
**
** Created by: Qt User Interface Compiler version 6.9.0
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_MAINWINDOW_H
#define UI_MAINWINDOW_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_MainWindow
{
public:
    QWidget *centralwidget;
    QLabel *remoteVideolabel;
    QLabel *localVideolabel;
    QPushButton *startMeetingButton;
    QPushButton *captureImageButton;
    QComboBox *cameraDevicecomboBox;
    QPushButton *switchCameraButton;
    QPushButton *startAudioButton;
    QPushButton *stopAudioButton;
    QCheckBox *enableAudioSavecheckBox;
    QCheckBox *enableAudioPlaycheckBox;
    QComboBox *audioDevicecomboBox;
    QPushButton *startRecordButton;
    QPushButton *stopRecordButton;
    QPushButton *startReceiveButton;
    QPushButton *stopMeetingButton;
    QMenuBar *menubar;
    QStatusBar *statusbar;

    void setupUi(QMainWindow *MainWindow)
    {
        if (MainWindow->objectName().isEmpty())
            MainWindow->setObjectName("MainWindow");
        MainWindow->resize(1087, 668);
        centralwidget = new QWidget(MainWindow);
        centralwidget->setObjectName("centralwidget");
        remoteVideolabel = new QLabel(centralwidget);
        remoteVideolabel->setObjectName("remoteVideolabel");
        remoteVideolabel->setGeometry(QRect(130, 60, 391, 331));
        localVideolabel = new QLabel(centralwidget);
        localVideolabel->setObjectName("localVideolabel");
        localVideolabel->setGeometry(QRect(650, 50, 320, 240));
        localVideolabel->setMinimumSize(QSize(320, 240));
        localVideolabel->setBaseSize(QSize(320, 240));
        localVideolabel->setAlignment(Qt::AlignmentFlag::AlignCenter);
        startMeetingButton = new QPushButton(centralwidget);
        startMeetingButton->setObjectName("startMeetingButton");
        startMeetingButton->setGeometry(QRect(610, 330, 93, 28));
        captureImageButton = new QPushButton(centralwidget);
        captureImageButton->setObjectName("captureImageButton");
        captureImageButton->setGeometry(QRect(750, 320, 93, 28));
        cameraDevicecomboBox = new QComboBox(centralwidget);
        cameraDevicecomboBox->setObjectName("cameraDevicecomboBox");
        cameraDevicecomboBox->setGeometry(QRect(80, 430, 161, 31));
        switchCameraButton = new QPushButton(centralwidget);
        switchCameraButton->setObjectName("switchCameraButton");
        switchCameraButton->setGeometry(QRect(250, 430, 93, 28));
        startAudioButton = new QPushButton(centralwidget);
        startAudioButton->setObjectName("startAudioButton");
        startAudioButton->setGeometry(QRect(600, 370, 101, 41));
        stopAudioButton = new QPushButton(centralwidget);
        stopAudioButton->setObjectName("stopAudioButton");
        stopAudioButton->setGeometry(QRect(930, 380, 111, 41));
        enableAudioSavecheckBox = new QCheckBox(centralwidget);
        enableAudioSavecheckBox->setObjectName("enableAudioSavecheckBox");
        enableAudioSavecheckBox->setGeometry(QRect(600, 450, 98, 23));
        enableAudioPlaycheckBox = new QCheckBox(centralwidget);
        enableAudioPlaycheckBox->setObjectName("enableAudioPlaycheckBox");
        enableAudioPlaycheckBox->setGeometry(QRect(600, 420, 98, 23));
        audioDevicecomboBox = new QComboBox(centralwidget);
        audioDevicecomboBox->setObjectName("audioDevicecomboBox");
        audioDevicecomboBox->setGeometry(QRect(720, 380, 191, 31));
        startRecordButton = new QPushButton(centralwidget);
        startRecordButton->setObjectName("startRecordButton");
        startRecordButton->setGeometry(QRect(440, 500, 93, 28));
        stopRecordButton = new QPushButton(centralwidget);
        stopRecordButton->setObjectName("stopRecordButton");
        stopRecordButton->setGeometry(QRect(550, 500, 93, 28));
        startReceiveButton = new QPushButton(centralwidget);
        startReceiveButton->setObjectName("startReceiveButton");
        startReceiveButton->setGeometry(QRect(340, 570, 93, 28));
        stopMeetingButton = new QPushButton(centralwidget);
        stopMeetingButton->setObjectName("stopMeetingButton");
        stopMeetingButton->setGeometry(QRect(880, 330, 93, 28));
        MainWindow->setCentralWidget(centralwidget);
        menubar = new QMenuBar(MainWindow);
        menubar->setObjectName("menubar");
        menubar->setGeometry(QRect(0, 0, 1087, 25));
        MainWindow->setMenuBar(menubar);
        statusbar = new QStatusBar(MainWindow);
        statusbar->setObjectName("statusbar");
        MainWindow->setStatusBar(statusbar);

        retranslateUi(MainWindow);

        QMetaObject::connectSlotsByName(MainWindow);
    } // setupUi

    void retranslateUi(QMainWindow *MainWindow)
    {
        MainWindow->setWindowTitle(QCoreApplication::translate("MainWindow", "MainWindow", nullptr));
        remoteVideolabel->setText(QCoreApplication::translate("MainWindow", "TextLabel", nullptr));
        localVideolabel->setText(QCoreApplication::translate("MainWindow", "TextLabel", nullptr));
        startMeetingButton->setText(QCoreApplication::translate("MainWindow", "\345\220\257\345\212\250\344\274\232\350\256\256", nullptr));
        captureImageButton->setText(QCoreApplication::translate("MainWindow", "\346\213\215\347\205\247", nullptr));
        switchCameraButton->setText(QCoreApplication::translate("MainWindow", "\345\210\207\346\215\242\346\221\204\345\203\217\345\244\264", nullptr));
        startAudioButton->setText(QCoreApplication::translate("MainWindow", "\345\274\200\345\247\213\351\237\263\351\242\221\351\207\207\351\233\206", nullptr));
        stopAudioButton->setText(QCoreApplication::translate("MainWindow", "\345\201\234\346\255\242\351\237\263\351\242\221\351\207\207\351\233\206", nullptr));
        enableAudioSavecheckBox->setText(QCoreApplication::translate("MainWindow", "\344\277\235\345\255\230\351\237\263\351\242\221", nullptr));
        enableAudioPlaycheckBox->setText(QCoreApplication::translate("MainWindow", "\346\222\255\346\224\276\351\237\263\351\242\221", nullptr));
        startRecordButton->setText(QCoreApplication::translate("MainWindow", "\345\274\200\345\247\213\345\275\225\345\203\217", nullptr));
        stopRecordButton->setText(QCoreApplication::translate("MainWindow", "\345\201\234\346\255\242\345\275\225\345\203\217", nullptr));
        startReceiveButton->setText(QCoreApplication::translate("MainWindow", "\345\274\200\345\247\213\346\216\245\346\224\266", nullptr));
        stopMeetingButton->setText(QCoreApplication::translate("MainWindow", "\345\201\234\346\255\242\344\274\232\350\256\256", nullptr));
    } // retranslateUi

};

namespace Ui {
    class MainWindow: public Ui_MainWindow {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_MAINWINDOW_H
