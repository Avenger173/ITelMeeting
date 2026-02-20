#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <QImage>
#include <QPixmap>
#include <QDateTime>
#include <QMessageBox>
#include <QDebug>
#include <QThread>
#include <QAudioFormat>
#include <QPainter>
#include <QApplication>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLineEdit>
#include <QUrl>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QToolButton>
#include <QSlider>
#include <QSignalBlocker>
#include <QSettings>
#include <QFileDialog>
#include <QTextStream>
#include <QStringConverter>
#include <QDir>
#include <QStatusBar>
#include <algorithm>
#include<QHBoxLayout>
#include<QTabWidget>
#include<QGuiApplication>
#include<QScreen>
#include<QActionGroup>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include<Windows.h>
#endif

namespace {
static bool stopThreadAndDelete(QThread *&thr, const char *tag, int quitWaitMs = 3000) {
    if (!thr) return true;
    thr->quit();
    if (!thr->wait(quitWaitMs)) {
        qWarning() << "[Mainwindow]" << tag << "stop timeout, detach";
        thr->requestInterruption();
        thr->setParent(nullptr);
        QObject::connect(thr, &QThread::finished, thr, &QObject::deleteLater, Qt::UniqueConnection);
        thr = nullptr;
        return false;
    }
    delete thr;
    thr = nullptr;
    return true;
}
}
#ifdef Q_OS_WIN
namespace{
struct  WinShareEntry
{
    quint64 hwnd=0;
    QString title;
};

static BOOL CALLBACK enumShareWindowProc(HWND hwnd,LPARAM lParam){
    auto *out=reinterpret_cast<QVector<WinShareEntry>*>(lParam);
    if(!out) return TRUE;
    if(!IsWindowVisible(hwnd)||IsIconic(hwnd)) return TRUE;
    const LONG exStyle=GetWindowLong(hwnd,GWL_EXSTYLE);
    if(exStyle & WS_EX_TOOLWINDOW) return TRUE;

    wchar_t titleBuf[512]={0};
    const int len=GetWindowTextW(hwnd,titleBuf,511);
    if(len<=0) return TRUE;

    const QString title=QString::fromWCharArray(titleBuf,len).trimmed();
    if(title.isEmpty()) return TRUE;

    WinShareEntry e;
    e.hwnd=static_cast<quint64>(reinterpret_cast<quintptr>(hwnd));
    e.title=title;
    out->push_back(e);
    return TRUE;
}
}
#endif

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , timer(new QTimer(this))
{
    ui->setupUi(this);
    loadLoginPrefs();
    qInfo() << "[Build] SmartMeet" << __DATE__ << __TIME__;

    setWindowTitle("SmartMeet视频会议系统");
    if (ui->startMeetingButton) ui->startMeetingButton->setText("开始会议");
    if (ui->stopMeetingButton) ui->stopMeetingButton->setText("结束会议");
    if (ui->startReceiveButton) ui->startReceiveButton->setText("连接信令");
    if (ui->startRecordButton) ui->startRecordButton->setText("开始AV录制");
    if (ui->stopRecordButton) ui->stopRecordButton->setText("停止AV录制");

    for (int i = 0; i < 5; ++i) {
        cv::VideoCapture temp(i);
        if (temp.isOpened()) {
            ui->cameraDevicecomboBox->addItem("摄像头" + QString::number(i), i);
            temp.release();
        }
    }
    for (const auto &device : QMediaDevices::audioInputs()) {
        ui->audioDevicecomboBox->addItem(device.description());
    }

    recorder = new AvRecorder(this);
    connect(recorder, &AvRecorder::videoPacketReady, this, [](const QByteArray &pkt, quint32 pts){
        qDebug() << "[Recorder] 视频包" << pkt.size() << "pts" << pts;
    });
    connect(recorder, &AvRecorder::audioPacketReady, this, [](const QByteArray &pkt, quint32 pts){
        qDebug() << "[Recorder] 音频包" << pkt.size() << "pts" << pts;
    });

    setupSignalUi();
    setupBottomMenus();
    setupRemoteGridUi();
    refreshRemoteTiles();
    setupMeetingStatsUi();
    setupLoginUi();

    signalReconnectTimer = new QTimer(this);
    signalReconnectTimer->setSingleShot(true);
    connect(signalReconnectTimer, &QTimer::timeout, this, [this]() {
        if (shuttingDown || meetingStopped || manualSignalDisconnect) return;
        if (signalConnected) return;
        if (!ensureSignalCredential()) return;
        if (!ensureRoomIdentity(false)) return;
        openSignalConnection();
    });
    setupAdaptiveNetworkControl();

    showLoginOverlay(true, "请输入账号、密码和房间号后登录");
}

MainWindow::~MainWindow()
{
    qDebug()<<"[MainWindow] 析构";
    shuttingDown = true;

    on_stopMeetingButton_clicked();
    if(signalSocket){
        disconnect(signalSocket, nullptr, this, nullptr);
        signalSocket->close();
        delete signalSocket;
        signalSocket=nullptr;
    }
    QCoreApplication::removePostedEvents(this);

    auto tmp=ui;
    ui=nullptr;
    delete tmp;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (shuttingDown) {
        event->accept();
        return;
    }
    shuttingDown = true;
    on_stopMeetingButton_clicked();
    QMainWindow::closeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    syncRemoteContainerGeometry();
    syncLoginOverlayGeometry();
}

void MainWindow::on_startMeetingButton_clicked()
{
    localScreenShareOn=false;
    meetingStopped = false;
    audioStopped = false;
    localAudioOn = true;
    localVideoOn = true;
    // 设为未初始化，保证本次会议首次应用策略时也会输出档位日志
    adaptiveProfileLevel = -1;
    adaptiveStressScore = 0;
    lastAdaptiveIssueMs = 0;
    lastAdaptiveLogMs = 0;
    totalSignalReconnectCount = 0;
    totalPullRetryCount = 0;
    recentEventLogs.clear();
    updateMeetingStatsUi();
    refreshSelfControlActions();

    if (videoWorker || videoThread) {
        qInfo() << "[Mainwindow] 会议已启动（重复启动忽略）";
        return;
    }

    if (!ensureRoomIdentity(false)) {
        qWarning() << "[Room] 房间身份初始化失败";
        return;
    }
    appendRoomEvent(QString("房间: %1 用户流: %2").arg(roomId, selfStream));
    currentPushUrl = QString("rtmp://127.0.0.1/live/%1").arg(selfStream);

    if (!videoWorker) {
        videoWorker = new VideoCapture;
        videoThread = new QThread(this);
        videoThread->setObjectName("videoThread");
        videoWorker->moveToThread(videoThread);

        connect(videoThread, &QThread::started, videoWorker, &VideoCapture::captureLoop);
        connect(videoThread, &QThread::finished, videoWorker, &QObject::deleteLater, Qt::UniqueConnection);

        QPointer<QLabel> localLabel = ui->localVideolabel;
        QPointer<MainWindow> self(this);
        disconnect(videoWorker, &VideoCapture::frameCaptured, this, nullptr);
        connect(videoWorker, &VideoCapture::frameCaptured, this, [self,localLabel](const QImage &img){
            if(!self) return;
            if(self->meetingStopped) return;
            if(!localLabel) return;
            localLabel->setPixmap(QPixmap::fromImage(img).scaled(
                localLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            if (self->camRecording && self->recorder && self->recorder->isOpen()) {
                self->recorder->pushVideoFrame(img);
            }
        }, Qt::QueuedConnection);

        int camIndex = 0;
        if (ui && ui->cameraDevicecomboBox) {
            camIndex = ui->cameraDevicecomboBox->currentData().toInt();
        }
        if (!videoWorker->open(camIndex)) {
            QMessageBox::warning(this, "错误", "无法打开摄像头");
            videoWorker->deleteLater();
            videoWorker = nullptr;
            videoThread->deleteLater();
            videoThread = nullptr;
            return;
        }

        videoThread->start(QThread::HighPriority);
        applyLocalVideoSource();
        applyBeautyToWorker();
    }

    if (!encThread) {
        encThread = new QThread(this);
        encThread->setObjectName("encThread");
        encThread->start(QThread::HighPriority);
    }
    if (!pushThread) {
        pushThread = new QThread(this);
        pushThread->setObjectName("pushThread");
        pushThread->start(QThread::HighPriority);
    }
    if (!audioEncThread) {
        audioEncThread = new QThread(this);
        audioEncThread->setObjectName("audioEncThread");
        audioEncThread->start(QThread::HighPriority);
    }

    if (!pusher) {
        pusher = new RtmpPusher(nullptr);
        pusher->moveToThread(pushThread);
        connect(pushThread, &QThread::finished, pusher, &QObject::deleteLater, Qt::UniqueConnection);
        connect(pusher, &RtmpPusher::writeError, this, [this](const QString &err, bool videoPacket) {
            const int w = videoPacket ? 2 : 1;
            handleNetworkIssue(QString("推流写包异常: %1").arg(err), w);
        });
    }

    if (!netEnc) {
        netEnc = new AvNetEncoder(nullptr);
        netEnc->moveToThread(encThread);
        connect(encThread, &QThread::finished, netEnc, &QObject::deleteLater, Qt::UniqueConnection);
    }
    if (!audioEnc) {
        audioEnc = new AvAudioEncoder(nullptr);
        audioEnc->moveToThread(audioEncThread);
        connect(audioEncThread, &QThread::finished, audioEnc, &QObject::deleteLater, Qt::UniqueConnection);
    }

    if (!audioWorker) startAudioCapture();

    // 启动会议时先应用默认档位（高清）
    applyAdaptiveProfile(0, false);

    int targetW = 1280;
    int targetH = 720;
    int targetFps = 30;
    int targetBitrate = 2200000;
    if (adaptiveProfileLevel == 1) {
        targetW = 960;
        targetH = 540;
        targetFps = 24;
        targetBitrate = 1400000;
    } else if (adaptiveProfileLevel >= 2) {
        targetW = 640;
        targetH = 360;
        targetFps = 20;
        targetBitrate = 900000;
    }

    bool aok = false;
    QMetaObject::invokeMethod(audioEnc, [&]() {
        aok = audioEnc->open(44100, 1);
    }, Qt::BlockingQueuedConnection);
    if (!aok) {
        qWarning() << "[Mainwindow] 音频编码器打开失败";
    }

    bool encOk = false;
    QMetaObject::invokeMethod(netEnc, [&]() {
        encOk = netEnc->openVideo(targetW, targetH, targetFps, targetBitrate);
    }, Qt::BlockingQueuedConnection);
    if (!encOk) {
        qWarning() << "[Mainwindow] netEnc openVideo 失败";
        return;
    }

    disconnect(netEnc, &AvNetEncoder::videoPacketReady, pusher, nullptr);
    connect(netEnc, &AvNetEncoder::videoPacketReady, pusher, &RtmpPusher::pushEncodeVideo, Qt::QueuedConnection);
    disconnect(audioEnc, &AvAudioEncoder::audioPacketReady, pusher, nullptr);
    connect(audioEnc, &AvAudioEncoder::audioPacketReady, pusher, &RtmpPusher::pushEncodeAudio, Qt::QueuedConnection);

    if (videoSendConn) disconnect(videoSendConn);
    videoSendConn = connect(videoWorker, &VideoCapture::frameCaptured, this, [this](const QImage &img) {
        if (!netEnc) return;
        QImage out = img;
        if (!localVideoOn) {
            static QImage blackFrame;
            if (blackFrame.size() != img.size() || blackFrame.format() != QImage::Format_RGB888) {
                blackFrame = QImage(img.size(), QImage::Format_RGB888);
                blackFrame.fill(Qt::black);
            }
            out = blackFrame;
        }
        QMetaObject::invokeMethod(netEnc, "pushVideoFrame", Qt::QueuedConnection, Q_ARG(QImage, out));
    }, Qt::QueuedConnection);

    if (audioWorker) {
        if (audioSendConn) disconnect(audioSendConn);
        audioSendConn = connect(audioWorker, &AudioCapture::audioFrameReady, this, [this](const QByteArray &pcm) {
            if (!audioEnc) return;
            QByteArray toSend = pcm;
            if (!localAudioOn) {
                toSend.fill('\0');
            }
            QMetaObject::invokeMethod(audioEnc, "pushPcm", Qt::QueuedConnection, Q_ARG(QByteArray, toSend));
        }, Qt::QueuedConnection);
    }

    bool rtmpOk = false;
    QMetaObject::invokeMethod(pusher, [&]() {
        pusher->setVideoParams(targetW, targetH, targetFps);
        pusher->setAudioParams(44100, 1);
        rtmpOk = pusher->start(currentPushUrl, targetFps, 44100);
    },Qt::BlockingQueuedConnection);

    if (!rtmpOk) {
        qWarning() << "[Mainwindow] RTMP推流启动失败";
        isPublishing = false;
    } else {
        qDebug() << "[Mainwindow] RTMP推流已启动";
        isPublishing = true;
    }
    updateMeetingStatsUi();

    appendRoomEvent(QString("开始会议，推流: %1").arg(currentPushUrl));
    if (signalConnected) {
        sendSignalUpdate();
    }
}

void MainWindow::on_stopMeetingButton_clicked()
{
    if (stopMeetingInProgress) {
        qDebug() << "[Mainwindow] 会议停止流程执行中（重复调用忽略）";
        return;
    }
    stopMeetingInProgress = true;

    if (meetingStopped) {
        qDebug() << "[Mainwindow] 会议已停止（重复调用忽略）";
        stopMeetingInProgress = false;
        return;
    }
    meetingStopped = true;
    manualSignalDisconnect = true;
    resetSignalReconnectState();
    isPublishing = false;
    currentPushUrl.clear();
    adaptiveStressScore = 0;
    localScreenShareOn=false;
    qInfo() << "[Mainwindow] stop meeting begin";

    if (signalConnected) {
        sendSignalUpdate();
    }

    stopCurrentPull();
    qInfo() << "[Mainwindow] stop pull done";

    if (videoSendConn) disconnect(videoSendConn);
    if (audioSendConn) disconnect(audioSendConn);
    if (netEnc && pusher) disconnect(netEnc, &AvNetEncoder::videoPacketReady, pusher, nullptr);
    if (audioEnc && pusher) disconnect(audioEnc, &AvAudioEncoder::audioPacketReady, pusher, nullptr);

    if (receiver) disconnect(receiver, nullptr, this, nullptr);

    stopAudioCapture();
    qInfo() << "[Mainwindow] stop audio done";

    if (receiver) {
        disconnect(receiver, nullptr, this, nullptr);
        receiver->stop();
        delete receiver;
        receiver = nullptr;
    }

    if (sender) {
        sender->stop();
        delete sender;
        sender = nullptr;
    }

    if (pusher) {
        if (pushThread && pushThread->isRunning()) {
            QMetaObject::invokeMethod(pusher, [&]() {
                pusher->stop();
            }, Qt::BlockingQueuedConnection);
        } else {
            pusher->stop();
        }
    }

    stopThreadAndDelete(pushThread, "pushThread", 3000);
    qInfo() << "[Mainwindow] stop pushThread done";
    pusher = nullptr;

    if (audioEnc) {
        if (audioEncThread && audioEncThread->isRunning()) {
            QMetaObject::invokeMethod(audioEnc, [&]() {
                audioEnc->close();
            }, Qt::BlockingQueuedConnection);
        } else {
            audioEnc->close();
        }
    }
    stopThreadAndDelete(audioEncThread, "audioEncThread", 3000);
    qInfo() << "[Mainwindow] stop audioEncThread done";
    audioEnc = nullptr;

    if (netEnc) {
        if (encThread && encThread->isRunning()) {
            QMetaObject::invokeMethod(netEnc, [&]() {
                netEnc->close();
            }, Qt::BlockingQueuedConnection);
        } else {
            netEnc->close();
        }
    }
    stopThreadAndDelete(encThread, "encThread", 3000);
    qInfo() << "[Mainwindow] stop encThread done";
    netEnc = nullptr;

    if (videoWorker) {
        videoWorker->stop();
    }
    stopThreadAndDelete(videoThread, "videoThread", 3000);
    qInfo() << "[Mainwindow] stop videoThread done";
    videoWorker = nullptr;

    if (signalSocket) {
        disconnect(signalSocket, nullptr, this, nullptr);
        sendSignalLeave();
        signalSocket->close();
        appendRoomEvent("已断开信令");
    }
    signalConnected = false;
    if (signalStateLabel) signalStateLabel->setText("信令: 未连接");
    if (ui && ui->startReceiveButton) ui->startReceiveButton->setText("连接信令");
    memberStates.clear();
    roomHostStream.clear();
    preferredRemoteStream.clear();
    currentRemoteStream.clear();
    focusedStream.clear();
    focusMode = false;
    refreshRoomUserList();
    clearAllTileFrames();
    latestRemoteFrames.clear();
    if (remoteStack && remoteGridPage) {
        remoteStack->setCurrentWidget(remoteGridPage);
    }
    if (ui && ui->remoteVideolabel) {
        ui->remoteVideolabel->clear();
    }
    updateMeetingStatsUi();

    qDebug() << "[Mainwindow] 会议已结束";
    stopMeetingInProgress = false;
}

void MainWindow::on_captureImageButton_clicked()
{
    const QString filename = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss") + ".jpg";

    QImage remoteImg;
    if (captureFocusedRemoteImage(&remoteImg)) {
        if (remoteImg.save(filename, "JPG")) {
            QMessageBox::information(this, "提示", "已保存照片：" + filename);
        } else {
            QMessageBox::warning(this, "错误", "保存照片失败：" + filename);
        }
        return;
    }

    if (videoWorker) {
        videoWorker->capturePhoto(filename);
        QMessageBox::information(this, "提示", "已保存照片：" + filename);
        return;
    }

    QMessageBox::warning(this, "提示", "当前没有可用画面可拍照");
}



void MainWindow::startAudioCapture()
{
    if (!audioWorker) {
        audioWorker = new AudioCapture;
        audioThread = new QThread(this);
        audioThread->setObjectName("audioThread");
        audioWorker->moveToThread(audioThread);
        connect(audioThread, &QThread::finished, audioWorker, &QObject::deleteLater, Qt::UniqueConnection);

        connect(audioThread, &QThread::started, audioWorker, &AudioCapture::captureLoop);
        connect(audioWorker, &AudioCapture::logMessage, this, [](const QString &msg){
            qDebug() << "AudioLog:" << msg;
        });

        const QString selectedMic = (ui && ui->audioDevicecomboBox) ? ui->audioDevicecomboBox->currentText() : QString();
        QString device = "audio=" + selectedMic;
        qDebug() << "FFmpeg 采集设备名:" << device;

        // 采集端参数固定: 44100 Hz / 单声道 / Int16 (仅采集，不本地播放)
        QAudioFormat fmt;
        fmt.setSampleRate(44100);
        fmt.setChannelCount(1);
        fmt.setSampleFormat(QAudioFormat::Int16);

        if (!audioWorker->startCapture(device, false, false, fmt)) {
            QMessageBox::warning(this, "错误", "音频采集启动失败");
            return;
        }

        audioThread->start(QThread::HighPriority);
    }

    if (recordConn) disconnect(recordConn);
    QPointer<MainWindow> self(this);
    recordConn = connect(audioWorker, &AudioCapture::audioFrameReady, this,
            [self](const QByteArray &data){
                if(!self) return;
                if(self->meetingStopped||self->audioStopped) return;

                if(self->recorder&&self->recorder->isOpen()){
                    // 计算样本数
                    int nb_samples=data.size()/2;//S16 mono
                    self->recorder->pushAudioPCM(reinterpret_cast<const uint8_t*>(data.constData()),nb_samples);
                }

            },
            Qt::QueuedConnection
            );
}

void MainWindow::stopAudioCapture()
{

    // 如果已经停过一次，直接返回，避免重复释放
    if(audioStopped){
        qDebug()<<"[Mainwindow] 音频采集已停止(重复调用忽略)";
        return;
    }
    audioStopped=true;
    if (audioWorker) {
        audioWorker->stop();
        disconnect(audioWorker, nullptr, this, nullptr); // 断开所有信号
        recordConn = QMetaObject::Connection();
        audioSendConn = QMetaObject::Connection();
    }
    stopThreadAndDelete(audioThread, "audioThread", 5000);
    audioWorker = nullptr;

    qDebug()<<"[Mainwindow] 音频采集已停止";
}

void MainWindow::applyLocalVideoSource()
{
    if(!videoWorker||!videoThread||!videoThread->isRunning()) return;
    const int mode=localScreenShareOn?1:0;

    if(localScreenShareOn){
        videoWorker->setShareTarget(shareScreenIndex,shareWindowId);
    }

    //不能走QueuedConnection:captureLoop常驻会导致槽不执行
    videoWorker->setCaptureMode(mode);
    qInfo()<<"[Share] local source="<<(mode==1?shareSourceName:"camera");
}

void MainWindow::applyBeautyToWorker()
{
    if(!videoWorker) return;

    const int level=qBound(0,localBeautyLevel,100);
    const int style=localBeautyStyle;
    QPointer<VideoCapture> worker(videoWorker);

    QMetaObject::invokeMethod(videoWorker,[worker,style,level](){
        if(!worker) return;
        worker->setBeautyStyle(style);
        worker->setBeautyLevel(level);
        worker->setBeautyEnabled(level>0&&style>0);
    },Qt::QueuedConnection);
}

void MainWindow::setBeautyMode(const QString &modeName, int level)
{
    localBeautyMode=modeName;
    int sliderLevel=localBeautyLevel;
    if(beautyStrengthSlider){
        sliderLevel=qBound(0,beautyStrengthSlider->value(),100);
    }
    int finalLevel=(level>=0)?qBound(0,level,100):sliderLevel;

    if(modeName=="关闭"){
        localBeautyStyle=0;
        localBeautyLevel=0;
    }else{
        if(modeName=="自然") localBeautyStyle=1;
        else if(modeName=="清晰") localBeautyStyle=2;
        else if(modeName=="柔和") localBeautyStyle=3;
        else if(modeName=="磨皮") localBeautyStyle=4;
        else if(modeName=="瘦脸") localBeautyStyle=5;
        else if(modeName=="祛皱") localBeautyStyle=6;
        else localBeautyStyle=1;

        localBeautyLevel=qBound(1,finalLevel,100);
        if(beautyStrengthSlider && beautyStrengthSlider->value()!=localBeautyLevel){
            beautyStrengthSlider->setValue(localBeautyLevel);
        }
    }
    if(beautyStrengthValueLabel){
        beautyStrengthValueLabel->setText(QString::number(localBeautyLevel));
    }
    applyBeautyToWorker();
    if(localBeautyStyle<=0){
        appendRoomEvent("美颜已关闭");
    }else{
        appendRoomEvent(QString("美颜模式：%1（强度%2）").arg(localBeautyMode).arg(localBeautyLevel));
    }
}

void MainWindow::setupWhiteboardUi()
{
    if(whiteboardCanvasLabel){
        whiteboardCanvasLabel->installEventFilter(this);
        whiteboardCanvasLabel->setMouseTracking(true);
    }
    if(whiteboardColorCombo){
        if(whiteboardColorCombo->count()>=4){
            whiteboardColorCombo->setItemData(0,"#e03131",Qt::UserRole);//红
            whiteboardColorCombo->setItemData(1,"#1971c2",Qt::UserRole);//蓝
            whiteboardColorCombo->setItemData(2,"#2b8a3e",Qt::UserRole);//绿
            whiteboardColorCombo->setItemData(3,"#111111",Qt::UserRole);//黑
        }
    }
    if(whiteboardWidthSpin){
        whiteboardWidthSpin->setRange(1,12);
        if(whiteboardWidthSpin->value()<=0){
            whiteboardWidthSpin->setValue(3);
        }
    }
    if(whiteboardPenButton){
        disconnect(whiteboardPenButton,&QToolButton::toggled,this,nullptr);
        connect(whiteboardPenButton,&QToolButton::toggled,this,[this](bool on){
            whiteboardPenEnabled=on;
            appendRoomEvent(on?"白板画笔已开启":"白板画笔已关闭");
        });
    }
    if(whiteboardUndoButton){
        disconnect(whiteboardUndoButton,&QPushButton::clicked,this,nullptr);
        connect(whiteboardUndoButton,&QPushButton::clicked,this,[this](){
            if(!canWriteWhiteboard()){
                appendRoomEvent("白板已锁定，仅主持人/联席主持人可撤销");
                return;
            }
            undoLastLocalStroke(true);
        });
    }
    if(whiteboardClearButton){
        disconnect(whiteboardClearButton,&QPushButton::clicked,this,nullptr);
        connect(whiteboardClearButton,&QPushButton::clicked,this,[this](){
            if(!canClearWhiteboard()){
                appendRoomEvent("仅主持人/联席主持人可清空白板");
                return;
            }
            clearWhiteboard(true,selfStream);
        });
        whiteboardClearButton->setEnabled(canClearWhiteboard());
    }
    if(whiteboardLockButton){
        disconnect(whiteboardLockButton,&QToolButton::toggled,this,nullptr);
        connect(whiteboardLockButton,&QToolButton::toggled,this,[this](bool on){
            const bool canManage=canManageWhiteboard();
            if(!canManage){
                QSignalBlocker guard(whiteboardLockButton);
                whiteboardLockButton->setChecked(whiteboardLocked);
                appendRoomEvent("仅主持人/联席主持人可锁定白板");
                return;
            }
            whiteboardLocked=on;
            applyWhiteboardLockUi();
            sendSignalWhiteboardLock(on);
            appendRoomEvent(on ? "白板已锁定(仅主持人/联席主持人可写)":"白板已解锁(全员可写)");
        });
    }
    applyWhiteboardLockUi();
    ensureWhiteboardCanvas();
}

void MainWindow::ensureWhiteboardCanvas()
{
    if(!whiteboardCanvasLabel) return;
    QSize target=whiteboardCanvasLabel->size();
    if(target.width()<64||target.height()<64) target=QSize(960,540);

    if(whiteboardCanvas.size()==target&&!whiteboardCanvas.isNull()) return;

    QImage newCanvas(target,QImage::Format_ARGB32_Premultiplied);
    newCanvas.fill(Qt::white);

    if(!whiteboardCanvas.isNull()){
        QPainter p(&newCanvas);
        p.setRenderHint(QPainter::SmoothPixmapTransform,true);
        p.drawImage(QRect(QPoint(0,0),target),whiteboardCanvas);
    }
    whiteboardCanvas=newCanvas;
    updateWhiteboardCanvasLabel();
}

void MainWindow::updateWhiteboardCanvasLabel()
{
    if(!whiteboardCanvasLabel||whiteboardCanvas.isNull()) return;
    whiteboardCanvasLabel->setPixmap(QPixmap::fromImage(whiteboardCanvas));
}

QPoint MainWindow::mapWhiteboardPoint(const QPoint &widgetPos) const
{
    if(!whiteboardCanvasLabel||whiteboardCanvas.isNull()) return QPoint();
    const int w=qMax(1,whiteboardCanvasLabel->width()-1);
    const int h=qMax(1,whiteboardCanvasLabel->height()-1);
    const int x=qBound(0,widgetPos.x(),w);
    const int y=qBound(0,widgetPos.y(),h);

    const int cx=(x*qMax(1,whiteboardCanvas.width()-1))/w;
    const int cy=(y*qMax(1,whiteboardCanvas.height()-1))/h;
    return QPoint(cx,cy);
}

QColor MainWindow::whiteboardSelectedColor() const
{
    if(whiteboardColorCombo){
        const QString hex=whiteboardColorCombo->currentData(Qt::UserRole).toString();
        if(!hex.isEmpty()) return QColor(hex);
    }
    return QColor("#e03131");
}

int MainWindow::whiteboardSelectedWidth() const
{
    if(whiteboardWidthSpin) return qBound(1,whiteboardWidthSpin->value(),12);
    return 3;
}

bool MainWindow::canClearWhiteboard() const
{
    if(!signalConnected) return true;
    if(memberStates.isEmpty()) return true;
    return canManageWhiteboard();
}

void MainWindow::redrawWhiteboardFromStrokes()
{
    ensureWhiteboardCanvas();
    if(whiteboardCanvas.isNull()) return;

    whiteboardCanvas.fill(Qt::white);
    QPainter p(&whiteboardCanvas);
    p.setRenderHint(QPainter::Antialiasing,true);

    for(const auto &stroke : WhiteboardStrokes){
        QPen pen(stroke.color,stroke.width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin);
        p.setPen(pen);
        for(const QLine &seg : stroke.segments){
            p.drawLine(seg);
        }
    }
    updateWhiteboardCanvasLabel();
}

void MainWindow::removeStrokeById(const QString &strokeId, const QString &byStream)
{
    if(strokeId.isEmpty()) return;

    QString owner;
    bool removed=false;
    for(int i=WhiteboardStrokes.size()-1;i>=0;--i){
        if(WhiteboardStrokes[i].strokeId==strokeId){
            owner=WhiteboardStrokes[i].ownerStream;
            WhiteboardStrokes.removeAt(i);
            removed=true;
            break;
        }
    }
    if(!removed) return;

    redrawWhiteboardFromStrokes();

    if(!byStream.isEmpty()){
        if(byStream==selfStream){
            appendRoomEvent("你撤销了一笔白板");
        }else{
            appendRoomEvent(QString("%1 撤销了一笔白板").arg(byStream));
        }
    }
}

void MainWindow::undoLastLocalStroke(bool broadcast)
{
    for(int i=WhiteboardStrokes.size()-1;i>=0;--i){
        if(WhiteboardStrokes[i].ownerStream==selfStream){
            const QString sid=WhiteboardStrokes[i].strokeId;
            removeStrokeById(sid,selfStream);
            if(broadcast) sendSignalWhiteboardUndo(sid);
            return;
        }
    }
    appendRoomEvent("没有可撤销的本地笔迹");
}

void MainWindow::drawWhiteboardLine(const QPoint &from,const QPoint &to,bool broadcast,
                                    const QColor &color,int width,const QString &strokeId,const QString &ownerStream)
{
    ensureWhiteboardCanvas();
    if(whiteboardCanvas.isNull()) return;

    const QString sid=strokeId.isEmpty()
        ? QString("st_%1_%2").arg(ownerStream.isEmpty()? selfStream : ownerStream)
                            .arg(QDateTime::currentMSecsSinceEpoch())
        :strokeId;
    const QString owner=ownerStream.isEmpty()?selfStream:ownerStream;
    const QColor penColor=color.isValid() ? color : QColor("#e03131");
    const int penWidth=qBound(1,width,12);

    int idx=-1;
    for(int i=WhiteboardStrokes.size()-1;i>=0;--i){
        if(WhiteboardStrokes[i].strokeId==sid){
            idx=i;
            break;
        }
    }
    if(idx<0){
        WhiteboardStroke stroke;
        stroke.strokeId=sid;
        stroke.ownerStream=owner;
        stroke.color=penColor;
        stroke.width=penWidth;
        WhiteboardStrokes.push_back(stroke);
        idx=WhiteboardStrokes.size()-1;
    }

    WhiteboardStrokes[idx].segments.push_back(QLine(from,to));

    QPainter p(&whiteboardCanvas);
    p.setRenderHint(QPainter::Antialiasing,true);
    QPen pen(WhiteboardStrokes[idx].color,WhiteboardStrokes[idx].width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin);
    p.setPen(pen);
    p.drawLine(from,to);

    updateWhiteboardCanvasLabel();

    if(broadcast){
        sendSignalWhiteboardDraw(from,to,WhiteboardStrokes[idx].color,WhiteboardStrokes[idx].width,sid);
    }
}

void MainWindow::clearWhiteboard(bool broadcast, const QString &byStream)
{
    ensureWhiteboardCanvas();
    if(whiteboardCanvas.isNull()) return;

    WhiteboardStrokes.clear();
    whiteboardActionStrokeId.clear();

    whiteboardCanvas.fill(Qt::white);
    updateWhiteboardCanvasLabel();

    if(broadcast){
        sendSignalWhiteboardClear();
    }

    if(!byStream.isEmpty()){
        appendRoomEvent(byStream==selfStream?"你已清空白板"
                                            :QString("%1 已清空白板").arg(byStream));
    }
}

void MainWindow::sendSignalWhiteboardDraw(const QPoint &from,const QPoint &to,
                                          const QColor &color,int width,const QString &strokeId)
{
    if(!signalSocket||!signalConnected||whiteboardCanvas.isNull()) return;

    const int w=qMax(1,whiteboardCanvas.width()-1);
    const int h=qMax(1,whiteboardCanvas.height()-1);

    QJsonObject obj;
    obj["type"]="wb";
    obj["op"]="draw";
    obj["room"]=roomId;
    obj["stream"]=selfStream;
    obj["x1n"]=(from.x()*10000)/w;
    obj["y1n"]=(from.y()*10000)/h;
    obj["x2n"]=(to.x()*10000)/w;
    obj["y2n"]=(to.y()*10000)/h;
    obj["color"]=color.name(QColor::HexRgb);
    obj["pw"]=qBound(1,width,12);
    obj["stroke_id"]=strokeId;

    const qint64 nowMs=QDateTime::currentMSecsSinceEpoch();
    obj["ts"]=nowMs;
    obj["msg_id"]=QString("wb_%1_%2_%3").arg(selfStream).arg(nowMs).arg(++whiteboardLocalSeq);

    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void MainWindow::sendSignalWhiteboardClear()
{
    if(!signalSocket||!signalConnected) return;

    QJsonObject obj;
    obj["type"]="wb";
    obj["op"]="clear";
    obj["room"]=roomId;
    obj["stream"]=selfStream;

    const qint64 nowMs=QDateTime::currentMSecsSinceEpoch();
    obj["ts"]=nowMs;
    obj["msg_id"]=QString("wb_%1_%2_%3").arg(selfStream).arg(nowMs).arg(++whiteboardLocalSeq);

    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void MainWindow::sendSignalWhiteboardUndo(const QString &strokeId)
{
    if(!signalSocket||!signalConnected||strokeId.isEmpty()) return;

    QJsonObject obj;
    obj["type"]="wb";
    obj["op"]="undo";
    obj["room"]=roomId;
    obj["stream"]=selfStream;
    obj["stroke_id"]=strokeId;

    const qint64 nowMs=QDateTime::currentMSecsSinceEpoch();
    obj["ts"]=nowMs;
    obj["msg_id"]=QString("wb_%1_%2_%3").arg(selfStream).arg(nowMs).arg(++whiteboardLocalSeq);

    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

bool MainWindow::canWriteWhiteboard() const
{
    if(!whiteboardLocked) return true;
    if(!signalConnected) return true;
    if(!roomHostStream.isEmpty()&&selfStream==roomHostStream) return true;
    const MemberState st=memberStates.value(selfStream);
    return st.host||st.cohost;
}

bool MainWindow::canManageWhiteboard() const
{
    if(!signalConnected) return true;
    if(!roomHostStream.isEmpty()&&selfStream==roomHostStream) return true;
    if(!memberStates.contains(selfStream)) return false;
    const MemberState st=memberStates.value(selfStream);
    return st.host||st.cohost;
}

void MainWindow::applyWhiteboardLockUi()
{
    const bool canManage=canManageWhiteboard();

    if(whiteboardLockButton){
        QSignalBlocker guard(whiteboardLockButton);
        whiteboardLockButton->setChecked(whiteboardLocked);
        whiteboardLockButton->setEnabled(canManage);
        whiteboardLockButton->setText(whiteboardLocked ? "白板已锁":"白板解锁");
    }
    const bool canWrite = canWriteWhiteboard();
    if(whiteboardPenButton){
        whiteboardPenButton->setEnabled(canWrite);
    }
    if(whiteboardUndoButton){
        whiteboardUndoButton->setEnabled(canWrite);
    }
}

bool MainWindow::captureFocusedRemoteImage(QImage *outImage) const
{
    if(!outImage) return false;
    QString target=focusedStream;
    if(target.isEmpty()) target=currentRemoteStream;
    if(target.isEmpty()) return false;
    const auto it=latestRemoteFrames.constFind(target);
    if(it==latestRemoteFrames.constEnd()||it->isNull()) return false;
    *outImage=*it;
    return !outImage->isNull();
}

void MainWindow::sendSignalWhiteboardLock(bool locked)
{
    if(!signalSocket||!signalConnected) return;

    QJsonObject obj;
    obj["type"]="wb";
    obj["op"]=locked ? "lock":"unlock";
    obj["room"]=roomId;
    obj["stream"]=selfStream;
    const qint64 nowMs=QDateTime::currentMSecsSinceEpoch();
    obj["ts"]=nowMs;
    obj["msg_id"]=QString("wb_%1_%2_%3").arg(selfStream).arg(nowMs).arg(++whiteboardLocalSeq);

    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void MainWindow::loadLoginPrefs()
{
    QSettings settings("SmartMeet", "SmartMeet");
    const bool remember = settings.value("auth/remember_user", true).toBool();
    const QString savedUser = settings.value("auth/last_user").toString().trimmed();
    const QString savedRoom = settings.value("auth/last_room").toString().trimmed();

    if (remember && !savedUser.isEmpty()) {
        loginUser = savedUser;
    }
    if (!savedRoom.isEmpty()) {
        roomId = savedRoom;
    }
}

void MainWindow::saveLoginPrefs() const
{
    QSettings settings("SmartMeet", "SmartMeet");
    const bool remember = rememberLoginCheck ? rememberLoginCheck->isChecked() : true;
    settings.setValue("auth/remember_user", remember);
    if (remember) {
        settings.setValue("auth/last_user", loginUser);
    } else {
        settings.remove("auth/last_user");
    }
    settings.setValue("auth/last_room", roomId);
}

void MainWindow::setupLoginUi()
{
    if (!ui || !ui->centralwidget || loginOverlay) return;

    loginOverlay = new QFrame(ui->centralwidget);
    loginOverlay->setObjectName("loginOverlay");
    loginOverlay->setStyleSheet(
        "QFrame#loginOverlay{background:rgba(9,12,19,205);}"
        "QFrame#loginCard{background:#1b2332;border:1px solid #34415a;border-radius:12px;}"
        "QLabel#loginTitleLabel{color:#f0f4ff;font-size:20px;font-weight:700;}"
        "QLabel#loginSubTitleLabel{color:#b8c4dc;font-size:13px;}"
        "QLabel#loginHintLabel{color:#ffd27a;font-size:12px;}"
        "QLineEdit#loginUserEdit,QLineEdit#loginPasswordEdit,QLineEdit#loginRoomEdit{"
        "background:#111826;color:#eaf0ff;border:1px solid #3d4e6b;border-radius:6px;padding:7px 10px;min-height:30px;}"
        "QPushButton#loginLoginButton{background:#2e86de;color:white;border:1px solid #4a9eee;border-radius:6px;padding:7px 14px;font-weight:700;}"
        "QPushButton#loginRegisterButton{background:#2d3a52;color:#dce7ff;border:1px solid #4a5f84;border-radius:6px;padding:7px 14px;font-weight:700;}"
        "QPushButton#loginLoginButton:hover{background:#3b92ea;}"
        "QPushButton#loginRegisterButton:hover{background:#364765;}"
    );
    loginOverlay->setFrameShape(QFrame::NoFrame);
    loginOverlay->setAttribute(Qt::WA_StyledBackground, true);

    loginCard = new QFrame(loginOverlay);
    loginCard->setObjectName("loginCard");
    loginCard->setFrameShape(QFrame::StyledPanel);
    loginCard->setAttribute(Qt::WA_StyledBackground, true);

    auto *cardLayout = new QVBoxLayout(loginCard);
    cardLayout->setContentsMargins(22, 22, 22, 22);
    cardLayout->setSpacing(12);

    auto *titleLabel = new QLabel("欢迎使用 SmartMeet", loginCard);
    titleLabel->setObjectName("loginTitleLabel");
    auto *subTitleLabel = new QLabel("登录后进入会议主界面", loginCard);
    subTitleLabel->setObjectName("loginSubTitleLabel");

    loginHintLabel = new QLabel(loginCard);
    loginHintLabel->setObjectName("loginHintLabel");
    loginHintLabel->setWordWrap(true);
    loginHintLabel->setText("请输入账号、密码和房间号");

    loginUserEdit = new QLineEdit(loginCard);
    loginUserEdit->setObjectName("loginUserEdit");
    loginUserEdit->setPlaceholderText("账号（例如：u1001）");
    loginUserEdit->setClearButtonEnabled(true);

    loginPasswordEdit = new QLineEdit(loginCard);
    loginPasswordEdit->setObjectName("loginPasswordEdit");
    loginPasswordEdit->setPlaceholderText("密码");
    loginPasswordEdit->setEchoMode(QLineEdit::Password);
    loginPasswordEdit->setClearButtonEnabled(true);

    loginRoomEdit = new QLineEdit(loginCard);
    loginRoomEdit->setObjectName("loginRoomEdit");
    loginRoomEdit->setPlaceholderText("房间号（6位数字）");
    loginRoomEdit->setClearButtonEnabled(true);

    rememberLoginCheck = new QCheckBox("记住账号", loginCard);
    const bool rememberUser = QSettings("SmartMeet", "SmartMeet").value("auth/remember_user", true).toBool();
    rememberLoginCheck->setChecked(rememberUser);
    rememberLoginCheck->setStyleSheet("QCheckBox{color:#c8d5ee;}");

    if (loginUser.isEmpty()) {
        loginUser = QString("u%1").arg(QRandomGenerator::global()->bounded(1000, 9999));
    }
    if (roomId.isEmpty()) {
        roomId = QString::number(QRandomGenerator::global()->bounded(100000, 999999));
    }
    loginUserEdit->setText(loginUser);
    loginRoomEdit->setText(roomId);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(10);
    loginLoginButton = new QPushButton("登录并进入", loginCard);
    loginLoginButton->setObjectName("loginLoginButton");
    loginRegisterButton = new QPushButton("注册并进入", loginCard);
    loginRegisterButton->setObjectName("loginRegisterButton");
    buttonRow->addWidget(loginLoginButton);
    buttonRow->addWidget(loginRegisterButton);

    cardLayout->addWidget(titleLabel);
    cardLayout->addWidget(subTitleLabel);
    cardLayout->addWidget(loginHintLabel);
    cardLayout->addWidget(loginUserEdit);
    cardLayout->addWidget(loginPasswordEdit);
    cardLayout->addWidget(loginRoomEdit);
    cardLayout->addWidget(rememberLoginCheck);
    cardLayout->addLayout(buttonRow);

    connect(loginLoginButton, &QPushButton::clicked, this, [this]() {
        triggerLoginAction(false);
    });
    connect(loginRegisterButton, &QPushButton::clicked, this, [this]() {
        triggerLoginAction(true);
    });
    connect(loginUserEdit, &QLineEdit::returnPressed, this, [this]() {
        triggerLoginAction(false);
    });
    connect(loginPasswordEdit, &QLineEdit::returnPressed, this, [this]() {
        triggerLoginAction(false);
    });
    connect(loginRoomEdit, &QLineEdit::returnPressed, this, [this]() {
        triggerLoginAction(false);
    });
    connect(rememberLoginCheck, &QCheckBox::toggled, this, [this](bool){
        saveLoginPrefs();
    });

    syncLoginOverlayGeometry();
    loginOverlay->hide();
}

void MainWindow::syncLoginOverlayGeometry()
{
    if (!ui || !ui->centralwidget || !loginOverlay || !loginCard) return;

    loginOverlay->setGeometry(ui->centralwidget->rect());

    const int overlayW = loginOverlay->width();
    const int overlayH = loginOverlay->height();
    const int cardW = qBound(340, overlayW - 40, 460);
    loginCard->setFixedWidth(cardW);
    loginCard->adjustSize();

    const int x = (overlayW - loginCard->width()) / 2;
    const int y = qMax(20, (overlayH - loginCard->height()) / 2);
    loginCard->move(x, y);

    loginOverlay->raise();
}

void MainWindow::showLoginOverlay(bool show, const QString &hint)
{
    if (!loginOverlay) {
        setupLoginUi();
    }
    if (!loginOverlay) return;

    if (loginHintLabel) {
        if (!hint.isEmpty()) loginHintLabel->setText(hint);
    }
    if (loginUserEdit && !loginUser.isEmpty()) {
        loginUserEdit->setText(loginUser);
    }
    if (loginRoomEdit && !roomId.isEmpty()) {
        loginRoomEdit->setText(roomId);
    }

    loginOverlay->setVisible(show);
    if (show) {
        syncLoginOverlayGeometry();
        if (loginPasswordEdit && loginPasswordEdit->text().isEmpty()) {
            loginPasswordEdit->setFocus();
        } else if (loginUserEdit) {
            loginUserEdit->setFocus();
            loginUserEdit->selectAll();
        }
    }
}

void MainWindow::triggerLoginAction(bool registerFirst)
{
    if (!loginUserEdit || !loginPasswordEdit || !loginRoomEdit) return;

    const QString user = loginUserEdit->text().trimmed();
    const QString password = loginPasswordEdit->text();
    const QString room = loginRoomEdit->text().trimmed();

    if (user.isEmpty() || password.isEmpty() || room.isEmpty()) {
        showLoginOverlay(true, "账号、密码、房间号都不能为空");
        return;
    }

    loginUser = user;
    loginPassword = password;
    roomId = room;
    userId = loginUser;
    saveLoginPrefs();
    pendingAuthRegister = registerFirst;
    signalAuthed = false;
    authRegisterTried = registerFirst;

    if (signalConnected) {
        showLoginOverlay(true, registerFirst ? "正在注册并登录..." : "正在登录...");
        if (registerFirst) sendSignalAuthRegister();
        else sendSignalAuthLogin();
        return;
    }

    showLoginOverlay(true, registerFirst ? "正在连接并注册..." : "正在连接并登录...");
    on_startReceiveButton_clicked();
}

void MainWindow::on_logoutButton_clicked()
{
    appendRoomEvent("手动退出登录");
    manualSignalDisconnect = true;
    resetSignalReconnectState();

    if (signalSocket &&
        (signalSocket->state() == QAbstractSocket::ConnectedState
         || signalSocket->state() == QAbstractSocket::ConnectingState)) {
        if (signalConnected) {
            sendSignalLeave();
        }
        signalSocket->close();
    }

    signalAuthed = false;
    pendingAuthRegister = false;
    authRegisterTried = false;
    signalConnected = false;
    loginPassword.clear();
    userId.clear();
    selfStream.clear();

    if (rememberLoginCheck && !rememberLoginCheck->isChecked()) {
        loginUser.clear();
    }
    saveLoginPrefs();

    if (loginPasswordEdit) loginPasswordEdit->clear();
    if (signalStateLabel) signalStateLabel->setText("信令: 未连接");
    if (auto *signalBadgeLabel = findChild<QLabel*>("signalBadgeLabel")) {
        signalBadgeLabel->setText("信令离线");
    }
    if (ui && ui->startReceiveButton) ui->startReceiveButton->setText("连接信令");

    showLoginOverlay(true, "已退出登录，请重新登录");
}

bool MainWindow::ensureSignalCredential()
{
    if (loginUser.isEmpty() && loginUserEdit) {
        loginUser = loginUserEdit->text().trimmed();
    }
    if (loginPassword.isEmpty() && loginPasswordEdit) {
        loginPassword = loginPasswordEdit->text();
    }
    if (roomId.isEmpty() && loginRoomEdit) {
        roomId = loginRoomEdit->text().trimmed();
    }

    if (loginUser.isEmpty() || loginPassword.isEmpty()) {
        showLoginOverlay(true, "请先登录后再连接信令");
        return false;
    }
    if (roomId.isEmpty()) {
        showLoginOverlay(true, "请填写房间号");
        return false;
    }

    if (userId.isEmpty()) userId = loginUser;
    return true;
}

void MainWindow::sendSignalAuthLogin()
{
    if (!signalSocket || !signalConnected) return;
    QJsonObject obj;
    obj["type"] = "auth_login";
    obj["user"] = loginUser;
    obj["password"] = loginPassword;
    obj["ver"] = 1;
    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void MainWindow::sendSignalAuthRegister()
{
    if (!signalSocket || !signalConnected) return;
    QJsonObject obj;
    obj["type"] = "auth_register";
    obj["user"] = loginUser;
    obj["password"] = loginPassword;
    obj["ver"] = 1;
    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void MainWindow::on_startRecordButton_clicked()
{
    onDebugStartAVRecord();
}

void MainWindow::on_stopRecordButton_clicked()
{
    onDebugStopAVRecord();
}

void MainWindow::onDebugStartEmptyRecord()
{
    if(recorder&&recorder->isOpen()){
        QMessageBox::information(this,"提示","录制器已打开");
        return;
    }
    if(!recorder) recorder=new AvRecorder(this);

    const QString filename="test_empty.mp4";
    const int width = 640, height = 480, fps = 30;

    if(!recorder->open(filename, width, height, fps)){
        QMessageBox::warning(this,"错误","打开录制器失败，请查看控制台日志。");
        return;
    }
    QMessageBox::information(this,"提示",QString("已创建文件头: %1").arg(filename));
}

void MainWindow::onDebugStopEmptyRecord()
{
    if(!recorder||!recorder->isOpen()){
        QMessageBox::information(this,"提示","录制器未打开");
        return;
    }
    recorder->close();
    QMessageBox::information(this,"提示","已写入文件尾并关闭");
}

void MainWindow::onDebugGen3sTestVideo()
{
    // 1) 打开录制器
    if(recorder&&recorder->isOpen()){
        recorder->close();
    }
    if(!recorder) recorder=new AvRecorder(this);

    const int W=640,H=480,FPS=30,DUR_SEC=3;
    const int totalFrames=FPS*DUR_SEC;
    const QString filename="test_video.mp4";

    if(!recorder->open(filename,W,H,FPS)){
        QMessageBox::warning(this,"错误","打开录制器失败(无法写入头部).");
        return;
    }

    // 2) 生成3秒彩条测试画面并写入
    QElapsedTimer timer;
    const qint64 frameIntervalMs=1000/FPS;

    // 预先分配可复用的QImage(避免重复分配)
    QImage frame(W,H,QImage::Format_RGB888);
    if(frame.isNull()){
        QMessageBox::warning(this,"错误","创建QImage失败");
        recorder->close();
        return;
    }

    timer.start();
    for(int i=0;i<totalFrames;++i){
        // 2.1 生成彩色背景
        {
            QPainter p(&frame);
            // 背景渐变/彩条
            for(int y=0;y<H;++y){
                int r=(y*3+i*5)%256;
                int g=(y*5+i*3)%256;
                int b=(y*7+i*2)%256;
                // 画一条水平线
                p.setPen(QColor(r,g,b));
                p.drawLine(0,y,W-1,y);
            }
            // 画网格
            p.setPen(QColor(255, 255, 255, 80));
            for (int x = 0; x < W; x += 80) p.drawLine(x, 0, x, H);
            for (int y = 0; y < H; y += 60) p.drawLine(0, y, W, y);

            // 写时间戳与帧号
            p.setPen(Qt::yellow);
            p.setFont(QFont("Consolas", 18));
            p.drawText(10, 30, QString("SmartMeet 测试  %1x%2 @ %3fps").arg(W).arg(H).arg(FPS));
            p.drawText(10, 60, QString("帧: %1 / %2").arg(i+1).arg(totalFrames));
        }

        // 2.2 写入一帧
        recorder->pushVideoFrame(frame);

        // 2.3 简单帧率节流(避免写入过快)
        const qint64 elapsed=timer.elapsed();
        const qint64 target=(i+1)*frameIntervalMs;
        if(elapsed<target){
            QThread::msleep(static_cast<unsigned long>(target-elapsed));
        }
        // 处理UI事件，避免3秒卡顿
        qApp->processEvents();
    }

    // 3) 关闭录制器(写入尾部并落盘)
    recorder->close();

    QMessageBox::information(this,"完成",QString("已生成 %1 (%2 秒)").arg(filename).arg(DUR_SEC));
}

void MainWindow::onDebugStartCamRecord()
{
    if(!videoWorker){
        QMessageBox::warning(this,"错误","请先开始会议(打开摄像头)后再录制");
        return;
    }
    if(camRecording){
        QMessageBox::information(this,"提示","正在录制中");
        return;
    }
    if(recorder&&recorder->isOpen()){
        recorder->close();
    }
    if(!recorder) recorder=new AvRecorder(this);

    // 录制参数: 建议与编码器open参数保持一致
    const int W=640,H=480,FPS=30;
    recFps=FPS;
    lastPushMs=0;

    const QString filename=QDateTime::currentDateTime().toString("'record_'yyyyMMdd_hhmmss'.mp4'");
    if(!recorder->open(filename,W,H,FPS)){
        QMessageBox::warning(this,"错误","录制器打开失败，请查看日志");
        return;
    }
    camRecording=true;
    QMessageBox::information(this,"提示",QString("开始录制 %1").arg(filename));
}

void MainWindow::onDebugStopCamRecord()
{
    if(!camRecording){
        QMessageBox::information(this,"提示","当前没有进行中的录制");
        return;
    }
    camRecording=false;

    if(recorder&&recorder->isOpen()){
        recorder->close();
        QMessageBox::information(this,"提示","已停止并写入文件");
    }else{
        QMessageBox::information(this,"提示","录制器未打开");
    }
}

void MainWindow::onDebugStartEmptyAV()
{
    if(recorder&&recorder->isOpen()){
        recorder->close();
    }
    if(!recorder) recorder=new AvRecorder(this);

    const int W=640,H=480,FPS=30,SR=44100;
    const QString filename="test_empty_av.mp4";

    if(!recorder->openAV(filename,W,H,FPS,SR)){
        QMessageBox::warning(this,"错误","openAV失败，请查看控制台日志");
        return;
    }

    recorder->close();
    QMessageBox::information(this,"完成",QString("已创建(含空音轨+空视频轨): %1").arg(filename));
}

void MainWindow::onDebugStartAudioRecord()
{
    if(recorder&&recorder->isOpen()){
        recorder->close();
    }
    if(!recorder) recorder=new AvRecorder(this);

    const int SR=44100;// 采样率
    const QString filename="test_audio_only.mp4";

    // 仅音频
    if(!recorder->openAV(filename,640,480,30,SR)){
        QMessageBox::warning(this,"错误","openAV失败(音频初始化失败)");
        return;
    }

    QMessageBox::information(this,"提示",QString("开始音频录制 %1").arg(filename));
}

void MainWindow::onDebugStopAudioRecord()
{
    if(!recorder||!recorder->isOpen()){
        QMessageBox::information(this,"提示","录制器未打开");
        return;
    }
    recorder->close();
    QMessageBox::information(this,"提示","已写入文件尾并关闭");
}

void MainWindow::onDebugStartAVRecord()
{
    if(recorder&&recorder->isOpen()){
        recorder->close();
    }
    if(!recorder){
        recorder=new AvRecorder(this);
    }

    const int W=640,H=480,FPS=30,SR=44100;
    const QString filename=QDateTime::currentDateTime().toString("'record_av_'yyyyMMdd_hhmmss'.mp4'");

    if(!recorder->openAV(filename,W,H,FPS,SR)){
        QMessageBox::warning(this,"错误","openAV失败");
        return;
    }

    camRecording=true;// 视频线程开始push
    isRecording=true;// 标记音频线程也能push

    QMessageBox::information(this,"提示",QString("开始AV录制: %1").arg(filename));
}

void MainWindow::onDebugStopAVRecord()
{
    if(recorder&&recorder->isOpen()){
        recorder->close();
        QMessageBox::information(this,"提示","AV录制已停止并保存");
    }
    camRecording=false;
    isRecording=false;
}

void MainWindow::setupAdaptiveNetworkControl()
{
    if (adaptiveRecoverTimer) return;
    adaptiveRecoverTimer = new QTimer(this);
    adaptiveRecoverTimer->setInterval(5000);
    connect(adaptiveRecoverTimer, &QTimer::timeout, this, [this]() {
        tryRecoverAdaptiveProfile();
    });
    adaptiveRecoverTimer->start();
}

void MainWindow::applyAdaptiveProfile(int level, bool restartPipeline)
{
    static const PublishProfile kProfiles[] = {
        {QStringLiteral("高清"), 1280, 720, 30, 2200000, 1920, 1080},
        {QStringLiteral("均衡"), 960, 540, 24, 1400000, 1600, 900},
        {QStringLiteral("流畅"), 640, 360, 20, 900000, 1280, 720}
    };

    const int targetLevel = qBound(0, level, 2);
    const bool changed = (adaptiveProfileLevel != targetLevel);
    adaptiveProfileLevel = targetLevel;
    const PublishProfile &p = kProfiles[adaptiveProfileLevel];

    if (videoWorker) {
        QMetaObject::invokeMethod(videoWorker, "setTargetFps", Qt::QueuedConnection, Q_ARG(int, p.fps));
        QMetaObject::invokeMethod(videoWorker, "setShareMaxSize", Qt::QueuedConnection, Q_ARG(int, p.shareMaxW), Q_ARG(int, p.shareMaxH));
    }

    if (changed) {
        appendRoomEvent(QString("网络策略切换：%1（%2x%3@%4, %5kbps）")
                            .arg(p.name).arg(p.width).arg(p.height).arg(p.fps).arg(p.bitrate / 1000));
        updateMeetingStatsUi();
    }

    if (!restartPipeline) return;
    if (meetingStopped || !isPublishing) return;
    if (!netEnc || !pusher || !encThread || !pushThread) return;
    if (!encThread->isRunning() || !pushThread->isRunning()) return;

    if (currentPushUrl.isEmpty() && !selfStream.isEmpty()) {
        currentPushUrl = QString("rtmp://127.0.0.1/live/%1").arg(selfStream);
    }
    if (currentPushUrl.isEmpty()) return;

    bool encOk = false;
    QMetaObject::invokeMethod(netEnc, [&]() {
        netEnc->close();
        encOk = netEnc->openVideo(p.width, p.height, p.fps, p.bitrate);
    }, Qt::BlockingQueuedConnection);

    bool pushOk = false;
    QMetaObject::invokeMethod(pusher, [&]() {
        pusher->stop();
        pusher->setVideoParams(p.width, p.height, p.fps);
        pusher->setAudioParams(44100, 1);
        pushOk = pusher->start(currentPushUrl, p.fps, 44100);
    }, Qt::BlockingQueuedConnection);

    if (!encOk || !pushOk) {
        appendRoomEvent("网络策略切换失败：推流重建失败");
        qWarning() << "[Adaptive] profile rebuild failed encOk=" << encOk << " pushOk=" << pushOk;
        isPublishing = false;
        updateMeetingStatsUi();
        return;
    }

    qInfo() << "[Adaptive] profile applied" << p.name << p.width << p.height << p.fps << p.bitrate;
    updateMeetingStatsUi();
}

void MainWindow::handleNetworkIssue(const QString &reason, int weight)
{
    if (meetingStopped || !isPublishing) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    lastAdaptiveIssueMs = now;
    adaptiveStressScore = qMin(12, adaptiveStressScore + qMax(1, weight));

    if (now - lastAdaptiveLogMs > 1500) {
        appendRoomEvent(QString("网络波动: %1").arg(reason));
        lastAdaptiveLogMs = now;
    }

    if (adaptiveStressScore >= 3 && adaptiveProfileLevel < 2) {
        adaptiveStressScore = 0;
        applyAdaptiveProfile(adaptiveProfileLevel + 1, true);
    }
}

void MainWindow::tryRecoverAdaptiveProfile()
{
    if (meetingStopped || !isPublishing) return;
    if (adaptiveProfileLevel <= 0) return;
    if (lastAdaptiveIssueMs <= 0) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastAdaptiveIssueMs < 25000) return;

    adaptiveStressScore = 0;
    lastAdaptiveIssueMs = now;
    applyAdaptiveProfile(adaptiveProfileLevel - 1, true);
}

void MainWindow::setupMeetingStatsUi()
{
    meetingStatsLabel = findChild<QLabel*>("meetingStatsLabel");
    if (!meetingStatsLabel && statusBar()) {
        meetingStatsLabel = new QLabel(this);
        meetingStatsLabel->setObjectName("meetingStatsLabelFallback");
        statusBar()->addPermanentWidget(meetingStatsLabel, 1);
    }
    if (!meetingStatsTimer) {
        meetingStatsTimer = new QTimer(this);
        meetingStatsTimer->setInterval(1000);
        connect(meetingStatsTimer, &QTimer::timeout, this, &MainWindow::updateMeetingStatsUi);
        meetingStatsTimer->start();
    }
    updateMeetingStatsUi();
}

void MainWindow::updateMeetingStatsUi()
{
    struct ProfileView {
        const char *name;
        int w;
        int h;
        int fps;
        int bitrate;
    };
    static const ProfileView kProfiles[] = {
        {"高清", 1280, 720, 30, 2200000},
        {"均衡", 960, 540, 24, 1400000},
        {"流畅", 640, 360, 20, 900000}
    };

    QString stageText = isPublishing ? "推流中" : "未推流";
    QString profileText = "--";
    QString avText = "--";
    QString brText = "--";
    if (adaptiveProfileLevel >= 0 && adaptiveProfileLevel <= 2) {
        const ProfileView &p = kProfiles[adaptiveProfileLevel];
        profileText = QString::fromUtf8(p.name);
        avText = QString("%1x%2@%3").arg(p.w).arg(p.h).arg(p.fps);
        brText = QString("%1kbps").arg(p.bitrate / 1000);
    }

    const QString text = QString("%1 | 档位:%2 | %3 | %4 | 重连:信令%5 拉流%6")
                             .arg(stageText)
                             .arg(profileText)
                             .arg(avText)
                             .arg(brText)
                             .arg(totalSignalReconnectCount)
                             .arg(totalPullRetryCount);
    if (meetingStatsLabel) {
        meetingStatsLabel->setText(text);
    }
}

void MainWindow::exportRecentMeetingLogs()
{
    if (recentEventLogs.isEmpty()) {
        QMessageBox::information(this, "提示", "当前没有可导出的会议日志");
        return;
    }

    const QString defaultName = QString("meeting_log_%1.txt")
                                    .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    const QString defaultPath = QDir::current().absoluteFilePath(defaultName);
    const QString filePath = QFileDialog::getSaveFileName(
        this, "导出会中日志", defaultPath, "文本文件 (*.txt);;所有文件 (*)");
    if (filePath.isEmpty()) return;

    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "导出失败", QString("无法写入文件：%1").arg(filePath));
        return;
    }

    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << "SmartMeet 会中日志导出\n";
    out << "导出时间: " << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss") << "\n";
    out << "房间: " << roomId << "  用户流: " << selfStream << "\n";
    out << "------------------------------\n";
    for (const QString &line : recentEventLogs) {
        out << line << '\n';
    }
    f.close();

    appendRoomEvent(QString("会中日志已导出：%1").arg(filePath));
}

void MainWindow::resetSignalReconnectState()
{
    signalReconnectAttempt = 0;
    if (signalReconnectTimer && signalReconnectTimer->isActive()) {
        signalReconnectTimer->stop();
    }
}

void MainWindow::scheduleSignalReconnect(const QString &reason)
{
    if (manualSignalDisconnect || meetingStopped || shuttingDown) return;
    if (signalConnected) return;
    if (!signalReconnectTimer) return;
    if (signalReconnectTimer->isActive()) return;

    if (signalReconnectAttempt >= signalReconnectMaxAttempt) {
        appendRoomEvent(QString("信令重连已停止（超过 %1 次）").arg(signalReconnectMaxAttempt));
        return;
    }

    ++signalReconnectAttempt;
    const int exp = qMin(signalReconnectAttempt - 1, 5);
    const int delayMs = qMin(15000, 600 * (1 << exp));

    if (!reason.isEmpty()) {
        appendRoomEvent(QString("%1，准备重连（%2/%3）")
                            .arg(reason)
                            .arg(signalReconnectAttempt)
                            .arg(signalReconnectMaxAttempt));
    }
    appendRoomEvent(QString("将在 %1 ms 后重连信令").arg(delayMs));
    signalReconnectTimer->start(delayMs);
}

void MainWindow::openSignalConnection()
{
    if (!signalSocket) {
        signalSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        connect(signalSocket, &QWebSocket::connected, this, &MainWindow::onSignalConnected);
        connect(signalSocket, &QWebSocket::disconnected, this, &MainWindow::onSignalDisconnected);
        connect(signalSocket, &QWebSocket::textMessageReceived, this, &MainWindow::onSignalTextMessage);
        connect(signalSocket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError){
            if (!signalSocket) return;
            appendRoomEvent(QString("信令错误: %1").arg(signalSocket->errorString()));
        });
    }

    if (signalSocket->state() == QAbstractSocket::ConnectedState
        || signalSocket->state() == QAbstractSocket::ConnectingState) {
        appendRoomEvent("信令连接进行中");
        return;
    }

    if (signalStateLabel) signalStateLabel->setText("信令: 连接中");
    if (auto *signalBadgeLabel = findChild<QLabel*>("signalBadgeLabel")) {
        signalBadgeLabel->setText("信令连接中");
    }
    appendRoomEvent(QString("连接信令服务器: %1").arg(signalUrl));
    signalSocket->open(QUrl(signalUrl));
}


void MainWindow::on_startReceiveButton_clicked()
{
    const bool connectedOrConnecting =
        signalSocket && (signalSocket->state() == QAbstractSocket::ConnectedState
                         || signalSocket->state() == QAbstractSocket::ConnectingState);

    if (signalConnected || connectedOrConnecting) {
        manualSignalDisconnect = true;
        resetSignalReconnectState();
        appendRoomEvent("手动断开信令");
        if (signalSocket) {
            if (signalConnected) sendSignalLeave();
            signalSocket->close();
        }
        return;
    }

    manualSignalDisconnect = false;
    resetSignalReconnectState();

    if (this->QObject::sender() == ui->startReceiveButton) {
        pendingAuthRegister = false;
    }
    if (!ensureSignalCredential()) {
        return;
    }
    if (!ensureRoomIdentity(true)) {
        return;
    }

    openSignalConnection();
    qInfo() << "[Mainwindow] 接收端已启动(信令)";
}

void MainWindow::setupSignalUi()
{
    if (roomDock && roomUserList && roomEventLog && signalStateLabel && roomCountLabel &&
        chatMessageLog && chatInputEdit && sendChatButton) return;

    // 优先使用 UI(XML) 中已有控件。
    roomDock = findChild<QDockWidget*>("roomDock");
    signalStateLabel = findChild<QLabel*>("signalStateLabel");
    roomCountLabel = findChild<QLabel*>("roomCountLabel");
    roomUserList = findChild<QListWidget*>("roomUserList");
    roomEventLog = findChild<QPlainTextEdit*>("roomEventLog");
    chatMessageLog=findChild<QPlainTextEdit*>("chatMessageLog");
    chatInputEdit=findChild<QTextEdit*>("chatInputEdit");
    sendChatButton=findChild<QPushButton*>("sendChatButton");
    whiteboardCanvasLabel=findChild<QLabel*>("whiteboardCanvasLabel");
    whiteboardClearButton=findChild<QPushButton*>("whiteboardClearButton");
    whiteboardPenButton=findChild<QToolButton*>("whiteboardPenButton");
    whiteboardColorCombo=findChild<QComboBox*>("whiteboardColorCombo");
    whiteboardWidthSpin=findChild<QSpinBox*>("whiteboardWidthSpin");
    whiteboardUndoButton=findChild<QPushButton*>("whiteboardUndoButton");
    whiteboardLockButton=findChild<QToolButton*>("whiteboardLockButton");
    setupWhiteboardUi();

    // 若 UI 尚未放入这些控件，则回退到代码创建，保证兼容旧 ui 文件。
    if (!roomDock || !signalStateLabel || !roomCountLabel || !roomUserList || !roomEventLog) {
        roomDock = new QDockWidget("房间成员", this);
        roomDock->setObjectName("roomDock");

        QWidget *panel = new QWidget(roomDock);
        QVBoxLayout *layout = new QVBoxLayout(panel);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);

        signalStateLabel = new QLabel("信令: 未连接", panel);
        signalStateLabel->setObjectName("signalStateLabel");
        roomCountLabel = new QLabel("在线: 0", panel);
        roomCountLabel->setObjectName("roomCountLabel");
        roomUserList = new QListWidget(panel);
        roomUserList->setObjectName("roomUserList");
        roomUserList->setContextMenuPolicy(Qt::CustomContextMenu);
        roomEventLog = new QPlainTextEdit(panel);
        roomEventLog->setObjectName("roomEventLog");
        roomEventLog->setReadOnly(true);
        roomEventLog->setMaximumBlockCount(200);
        roomEventLog->setPlaceholderText("会议事件日志");

        layout->addWidget(signalStateLabel);
        layout->addWidget(roomCountLabel);
        layout->addWidget(roomUserList, 1);
        layout->addWidget(roomEventLog, 1);
        panel->setLayout(layout);

        roomDock->setWidget(panel);
        addDockWidget(Qt::RightDockWidgetArea, roomDock);
    } else {
        roomUserList->setContextMenuPolicy(Qt::CustomContextMenu);
        roomEventLog->setReadOnly(true);
        roomEventLog->setMaximumBlockCount(200);
    }

    // M3: 聊天输入区初始化（优先使用 chatTab；兼容旧 UI 时回退到 eventTab）。
    if (!chatMessageLog) {
        chatMessageLog = roomEventLog;
    }
    if (!chatInputEdit || !sendChatButton) {
        QWidget *chatTab = findChild<QWidget*>("chatTab");
        auto *chatLayout = chatTab ? qobject_cast<QVBoxLayout*>(chatTab->layout()) : nullptr;
        if (!chatLayout) {
            QWidget *eventTab = findChild<QWidget*>("eventTab");
            chatLayout = eventTab ? qobject_cast<QVBoxLayout*>(eventTab->layout()) : nullptr;
            chatTab = eventTab;
        }
        if (chatLayout) {
            QWidget *parentWidget = chatTab ? chatTab : this;
            auto *chatRow = new QHBoxLayout;
            chatRow->setSpacing(6);

            chatInputEdit = new QTextEdit(parentWidget);
            chatInputEdit->setObjectName("chatInputEdit");
            chatInputEdit->setPlaceholderText(QStringLiteral("输入消息，Enter发送，Shift+Enter换行"));
            chatInputEdit->setAcceptRichText(false);
            chatInputEdit->setFixedHeight(56);

            sendChatButton = new QPushButton(QStringLiteral("发送"), parentWidget);
            sendChatButton->setObjectName("sendChatButton");
            sendChatButton->setMinimumWidth(56);

            chatRow->addWidget(chatInputEdit, 1);
            chatRow->addWidget(sendChatButton, 0);

            chatLayout->addLayout(chatRow);
        }
    }
    if (sendChatButton) {
        connect(sendChatButton, &QPushButton::clicked, this, &MainWindow::onSendChatClicked, Qt::UniqueConnection);
    }
    if (chatInputEdit) {
        chatInputEdit->setAcceptRichText(false);
        chatInputEdit->installEventFilter(this);
    }


    connect(roomUserList, &QListWidget::itemDoubleClicked, this, &MainWindow::onRoomUserDoubleClicked, Qt::UniqueConnection);
    connect(roomUserList, &QListWidget::customContextMenuRequested, this, &MainWindow::onRoomListContextMenu, Qt::UniqueConnection);
}

void MainWindow::setupBottomMenus()//聊天面板入口
{
    beautyStrengthSlider=findChild<QSlider*>("beautyStrengthSlider");
    beautyStrengthValueLabel=findChild<QLabel*>("beautyStrengthValueLabel");
    if(beautyStrengthSlider){
        beautyStrengthSlider->setRange(0,100);
        if(localBeautyLevel<=0){
            localBeautyLevel=60;
        }
        beautyStrengthSlider->setValue(qBound(0,localBeautyLevel,100));
        if(beautyStrengthValueLabel){
            beautyStrengthValueLabel->setText(QString::number(beautyStrengthSlider->value()));
        }
        disconnect(beautyStrengthSlider,nullptr,this,nullptr);
        connect(beautyStrengthSlider,&QSlider::valueChanged,this,[this](int v){
            const int vv=qBound(0,v,100);
            localBeautyLevel=vv;
            if(beautyStrengthValueLabel){
                beautyStrengthValueLabel->setText(QString::number(vv));
            }
            if(localBeautyStyle>0){
                applyBeautyToWorker();
            }
        });
    }

    auto *beautyBtn = findChild<QToolButton*>("beautyMenuButton");
    if (beautyBtn && !beautyBtn->menu()) {
        auto *beautyMenu = new QMenu(beautyBtn);
        auto *group=new QActionGroup(beautyMenu);
        group->setExclusive(true);

        auto *off=beautyMenu->addAction("关闭");
        auto *natural = beautyMenu->addAction("自然");
        auto *clear = beautyMenu->addAction("清晰");
        auto *soft = beautyMenu->addAction("柔和");
        beautyMenu->addSeparator();
        auto *skin = beautyMenu->addAction("磨皮");
        auto *slim = beautyMenu->addAction("瘦脸");
        auto *wrinkle = beautyMenu->addAction("祛皱");

        for(QAction *a : {off,natural,clear,soft,skin,slim,wrinkle}){
            a->setCheckable(true);
            a->setActionGroup(group);
        }

        switch (localBeautyStyle) {
        case 1: natural->setChecked(true); break;
        case 2: clear->setChecked(true); break;
        case 3: soft->setChecked(true); break;
        case 4: skin->setChecked(true); break;
        case 5: slim->setChecked(true); break;
        case 6: wrinkle->setChecked(true); break;
        default: off->setChecked(true); break;
        }

        connect(off,&QAction::triggered,this,[this](){setBeautyMode("关闭",0);});
        connect(natural,&QAction::triggered,this,[this](){setBeautyMode("自然",-1);});
        connect(clear,&QAction::triggered,this,[this](){setBeautyMode("清晰",-1);});
        connect(soft,&QAction::triggered,this,[this](){setBeautyMode("柔和",-1);});
        connect(skin,&QAction::triggered,this,[this](){setBeautyMode("磨皮",-1);});
        connect(slim,&QAction::triggered,this,[this](){setBeautyMode("瘦脸",-1);});
        connect(wrinkle,&QAction::triggered,this,[this](){setBeautyMode("祛皱",-1);});

        beautyBtn->setMenu(beautyMenu);
    }

    auto *moreBtn = findChild<QToolButton*>("moreMenuButton");
    if (moreBtn && !moreBtn->menu()) {
        auto *moreMenu = new QMenu(moreBtn);

        auto *pickShareSourceAction=moreMenu->addAction("选择共享源...");
        connect(pickShareSourceAction,&QAction::triggered,this,&MainWindow::chooseShareSource);

        selfShareToggleAction=moreMenu->addAction(localScreenShareOn?"停止屏幕共享":"开始屏幕共享");
        connect(selfShareToggleAction,&QAction::triggered,this,[this](){
            if(meetingStopped||!videoWorker){
                appendRoomEvent("请先开始会议后再进行屏幕共享");
                return;
            }
            localScreenShareOn=!localScreenShareOn;
            applyLocalVideoSource();
            sendSignalUpdate();
            appendRoomEvent(localScreenShareOn?"你已开启屏幕共享":"你已停止屏幕共享");
            refreshSelfControlActions();
        });

        auto *whiteboard = moreMenu->addAction("协作白板");
        connect(whiteboard,&QAction::triggered,this,[this](){
            if(roomDock) roomDock->show();
            if(auto *tabs=findChild<QTabWidget*>("roomTabWidget")){
                if(auto *wbTab=findChild<QWidget*>("whiteboardTab")){
                    tabs->setCurrentWidget(wbTab);
                }
            }
        });

        auto *chat = moreMenu->addAction("聊天面板");
        connect(chat, &QAction::triggered, this, [this]() {
            if(roomDock) roomDock->show();
            if (auto *tabs = findChild<QTabWidget*>("roomTabWidget")) {
                if (auto *chatTab = findChild<QWidget*>("chatTab")) {
                    tabs->setCurrentWidget(chatTab);
                } else if (auto *eventTab = findChild<QWidget*>("eventTab")) {
                    tabs->setCurrentWidget(eventTab);
                }
            }
            if (chatInputEdit) chatInputEdit->setFocus();
        });
        auto *exportLogs = moreMenu->addAction("导出最近日志");
        connect(exportLogs, &QAction::triggered, this, [this]() {
            exportRecentMeetingLogs();
        });
        moreMenu->addSeparator();
        selfMicToggleAction = moreMenu->addAction("静音我自己");
        selfCamToggleAction = moreMenu->addAction("关闭我的摄像头");
        connect(selfMicToggleAction, &QAction::triggered, this, [this]() {
            localAudioOn = !localAudioOn;
            sendSignalUpdate();
            appendRoomEvent(localAudioOn ? "你已恢复麦克风" : "你已静音自己");
            refreshSelfControlActions();
        });
        connect(selfCamToggleAction, &QAction::triggered, this, [this]() {
            localVideoOn = !localVideoOn;
            sendSignalUpdate();
            appendRoomEvent(localVideoOn ? "你已开启摄像头" : "你已关闭自己的摄像头");
            refreshSelfControlActions();
        });
        moreBtn->setMenu(moreMenu);
    }
    refreshSelfControlActions();
}

void MainWindow::refreshSelfControlActions()
{
    if (selfMicToggleAction) {
        selfMicToggleAction->setText(localAudioOn ? "静音我自己" : "恢复我的麦克风");
    }
    if (selfCamToggleAction) {
        selfCamToggleAction->setText(localVideoOn ? "关闭我的摄像头" : "开启我的摄像头");
    }
    if(selfShareToggleAction){
        selfShareToggleAction->setText(localScreenShareOn?"停止屏幕共享":"开始屏幕共享");
    }
    if(auto *shareBadgeLabel=findChild<QLabel*>("shareBadgeLabel")){
        shareBadgeLabel->setText(localScreenShareOn?QString("共享:%1").arg(shareSourceName):"共享:关");
    }
    if(whiteboardClearButton){
        whiteboardClearButton->setEnabled(canClearWhiteboard());
    }
    applyWhiteboardLockUi();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == chatInputEdit && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            if (keyEvent->modifiers().testFlag(Qt::ShiftModifier)) {
                return false;
            }
            onSendChatClicked();
            return true;
        }
    }

    if(watched==whiteboardCanvasLabel){
        if(event->type()==QEvent::Show||event->type()==QEvent::Resize){
            ensureWhiteboardCanvas();
            return false;
        }

        if(!canWriteWhiteboard()){
            if(event->type()==QEvent::MouseButtonPress){
                appendRoomEvent("白板已锁定,仅主持人/联席主持人可书写");
            }
            if(event->type()==QEvent::MouseButtonPress
                || event->type()==QEvent::MouseButtonRelease
                || event->type()==QEvent::MouseMove
                || event->type()==QEvent::MouseButtonDblClick){
                return true;
            }
            return false;
        }

        if(!whiteboardPenEnabled) return false;

        auto *me=dynamic_cast<QMouseEvent*>(event);
        if(!me) return false;

        if(event->type()==QEvent::MouseButtonPress&&me->button()==Qt::LeftButton){
            whiteboardMouseDown=true;
            whiteboardLastPoint=mapWhiteboardPoint(me->pos());
            whiteboardActionStrokeId=QString("st_%1_%2_%3")
                                           .arg(selfStream)
                                           .arg(QDateTime::currentMSecsSinceEpoch())
                                           .arg(++whiteboardLocalSeq);
            return true;
        }

        if(event->type()==QEvent::MouseMove && whiteboardMouseDown && (me->buttons() & Qt::LeftButton)){
            const QPoint cur=mapWhiteboardPoint(me->pos());
            drawWhiteboardLine(whiteboardLastPoint,cur,true,
                                whiteboardSelectedColor(),whiteboardSelectedWidth(),
                                whiteboardActionStrokeId,selfStream);
            whiteboardLastPoint=cur;
            return true;
        }

        if(event->type()==QEvent::MouseButtonRelease&&me->button()==Qt::LeftButton){
            if(whiteboardMouseDown){
                const QPoint cur=mapWhiteboardPoint(me->pos());
                drawWhiteboardLine(whiteboardLastPoint,cur,true,
                                    whiteboardSelectedColor(),whiteboardSelectedWidth(),
                                    whiteboardActionStrokeId,selfStream);
            }
            whiteboardMouseDown=false;
            whiteboardActionStrokeId.clear();
            return true;
        }
    }

    if (watched && watched->objectName() == "remoteStageFrame" &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show)) {
        syncRemoteContainerGeometry();
    }

    if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
        bool ok = false;
        const int idx = watched->property("tileIndex").toInt(&ok);
        if (ok && idx >= 0 && idx < remoteTiles.size()) {
            auto &tile = remoteTiles[idx];
            if (tile.cornerBadge && tile.frame) {
                tile.cornerBadge->adjustSize();
                tile.cornerBadge->move(tile.frame->width() - tile.cornerBadge->width() - 8, 8);
                tile.cornerBadge->raise();
            }
        }
    }

    if (event->type() == QEvent::MouseButtonDblClick) {
        bool ok = false;
        const int idx = watched->property("tileIndex").toInt(&ok);
        if (ok && idx >= 0 && idx < remoteTiles.size()) {
            const QString stream = remoteTiles[idx].stream;
            if (stream.isEmpty()) return true;

            const MemberState st = memberStates.value(stream);
            if (!st.pub) {
                appendRoomEvent("该成员未推流，无法拉取");
                return true;
            }

            if (focusMode && focusedStream == stream) {
                focusMode = false;
                focusedStream.clear();
                currentRemoteStream.clear();
                if (remoteStack && remoteGridPage) {
                    remoteStack->setCurrentWidget(remoteGridPage);
                }
                applyFocusAudioRouting();
                appendRoomEvent("已退出聚焦视图");
                refreshRemoteTiles();
                return true;
            }

            focusedStream = stream;
            focusMode = true;
            preferredRemoteStream = stream;
            if (remoteStack && ui && ui->remoteVideolabel) {
                remoteStack->setCurrentWidget(ui->remoteVideolabel);
            }
            startPullStream(stream);
            appendRoomEvent(QString("聚焦成员: %1").arg(stream));
            refreshRemoteTiles();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setupRemoteGridUi()
{
    if (!ui || !ui->centralwidget || !ui->remoteVideolabel) return;
    if (remoteContainer || remoteStack) return;

    QWidget *hostParent = findChild<QWidget*>("remoteStageFrame");
    if (!hostParent) hostParent = ui->centralwidget;
    remoteContainer = new QWidget(hostParent);
    remoteContainer->setObjectName("remoteContainer");
    syncRemoteContainerGeometry();

    remoteStack = new QStackedLayout(remoteContainer);
    remoteStack->setContentsMargins(0, 0, 0, 0);
    remoteStack->setSpacing(0);

    remoteGridPage = new QWidget(remoteContainer);
    remoteGridLayout = new QGridLayout(remoteGridPage);
    remoteGridLayout->setContentsMargins(2, 2, 2, 2);
    remoteGridLayout->setHorizontalSpacing(6);
    remoteGridLayout->setVerticalSpacing(6);

    constexpr int kTileCount = 9;   // 3x3 宫格，满足 2~6 人并预留扩展
    constexpr int kTileColumns = 3;
    remoteTiles.clear();
    remoteTiles.reserve(kTileCount);

    for (int i = 0; i < kTileCount; ++i) {
        RemoteTile tile;
        tile.frame = new QFrame(remoteGridPage);
        tile.frame->setFrameShape(QFrame::StyledPanel);
        tile.frame->setStyleSheet("QFrame{background:#101010;border:1px solid #4a4a4a;border-radius:6px;}");
        tile.frame->setProperty("tileIndex", i);
        tile.frame->installEventFilter(this);

        auto *vbox = new QVBoxLayout(tile.frame);
        vbox->setContentsMargins(6, 6, 6, 6);
        vbox->setSpacing(4);

        tile.videoLabel = new QLabel("空席位", tile.frame);
        tile.videoLabel->setAlignment(Qt::AlignCenter);
        tile.videoLabel->setMinimumSize(120, 80);
        tile.videoLabel->setStyleSheet("QLabel{background:#1a1a1a;color:#d0d0d0;}");
        tile.videoLabel->setProperty("tileIndex", i);
        tile.videoLabel->installEventFilter(this);

        tile.cornerBadge = new QLabel(tile.frame);
        tile.cornerBadge->setTextFormat(Qt::RichText);
        tile.cornerBadge->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        tile.cornerBadge->setStyleSheet("QLabel{background:transparent;color:#f0f4ff;font-size:11px;}");
        tile.cornerBadge->hide();

        tile.nameLabel = new QLabel("", tile.frame);
        tile.nameLabel->setStyleSheet("QLabel{color:#f0f0f0;font-weight:600;}");
        tile.stateLabel = new QLabel("", tile.frame);
        tile.stateLabel->setStyleSheet("QLabel{color:#a0a0a0;}");

        vbox->addWidget(tile.videoLabel, 1);
        vbox->addWidget(tile.nameLabel);
        vbox->addWidget(tile.stateLabel);
        remoteGridLayout->addWidget(tile.frame, i / kTileColumns, i % kTileColumns);

        remoteTiles.push_back(tile);
    }

    ui->remoteVideolabel->setParent(remoteContainer);
    ui->remoteVideolabel->setMinimumSize(120, 80);
    ui->remoteVideolabel->setAlignment(Qt::AlignCenter);
    ui->remoteVideolabel->setStyleSheet("QLabel{background:black;color:#d0d0d0;}");

    remoteStack->addWidget(remoteGridPage);
    remoteStack->addWidget(ui->remoteVideolabel);
    remoteStack->setCurrentWidget(remoteGridPage);

    focusStatusLabel = new QLabel(remoteContainer);
    focusStatusLabel->setObjectName("focusStatusLabel");
    focusStatusLabel->setStyleSheet("QLabel{background:rgba(0,0,0,160);color:#f0f0f0;padding:4px 8px;border-radius:4px;font-weight:600;}");
    focusStatusLabel->move(10, 10);
    focusStatusLabel->hide();
    focusStatusLabel->raise();

    hostParent->installEventFilter(this);
    remoteContainer->show();
}

void MainWindow::syncRemoteContainerGeometry()
{
    if (!remoteContainer || !ui) return;
    QWidget *stage = findChild<QWidget*>("remoteStageFrame");
    if (!stage) {
        stage = ui->centralwidget;
    }
    if (remoteContainer->parentWidget() != stage) {
        remoteContainer->setParent(stage);
    }
    const QRect full = stage->rect();
    remoteContainer->setGeometry(full.adjusted(8, 8, -8, -8));
    remoteContainer->raise();
}

void MainWindow::refreshRemoteTiles()
{
    if (remoteTiles.isEmpty()) return;

    QStringList streams = memberStates.keys();
    std::sort(streams.begin(), streams.end(), [this](const QString &a, const QString &b) {
        const MemberState sa = memberStates.value(a);
        const MemberState sb = memberStates.value(b);
        const int ra = sa.host ? 2 : (sa.cohost ? 1 : 0);
        const int rb = sb.host ? 2 : (sb.cohost ? 1 : 0);
        if (ra != rb) return ra > rb;
        return a < b;
    });

    int displayCount = qMin(streams.size(), remoteTiles.size());
    int columns = 3;
    if (displayCount <= 1) columns = 1;
    else if (displayCount <= 4) columns = 2;
    const int rows = qMax(1, (displayCount + columns - 1) / columns);

    if (remoteGridLayout) {
        while (QLayoutItem *item = remoteGridLayout->takeAt(0)) {
            (void)item;
        }
        for (int i = 0; i < remoteTiles.size(); ++i) {
            auto &tile = remoteTiles[i];
            if (!tile.frame) continue;

            if (i < displayCount) {
                const int row = i / columns;
                const int col = i % columns;
                remoteGridLayout->addWidget(tile.frame, row, col);
                tile.frame->show();
            } else {
                tile.frame->hide();
            }
        }
        for (int c = 0; c < 3; ++c) {
            remoteGridLayout->setColumnStretch(c, c < columns ? 1 : 0);
        }
        for (int r = 0; r < 3; ++r) {
            remoteGridLayout->setRowStretch(r, r < rows ? 1 : 0);
        }
    }

    streamToTile.clear();
    for (int i = 0; i < remoteTiles.size(); ++i) {
        auto &tile = remoteTiles[i];
        const QString oldStream = tile.stream;
        tile.stream.clear();

        if (i >= streams.size()) {
            tile.hasFrame = false;
            tile.videoLabel->setPixmap(QPixmap());
            tile.videoLabel->setText("空席位");
            tile.nameLabel->clear();
            tile.stateLabel->clear();
            if (tile.cornerBadge) tile.cornerBadge->hide();
            tile.frame->setStyleSheet("QFrame{background:#101010;border:1px solid #4a4a4a;border-radius:6px;}");
            continue;
        }

        const QString stream = streams[i];
        const MemberState st = memberStates.value(stream);
        tile.stream = stream;
        streamToTile.insert(stream, i);

        QString name = stream;
        if (st.host) name += " [主持人]";
        else if (st.cohost) name += " [联席主持人]";
        tile.nameLabel->setText(name);

        QStringList flags;
        if (!st.pub) flags << "未推流";
        tile.stateLabel->setText(flags.isEmpty() ? "双击聚焦" : flags.join(" | "));

        if(st.share) flags<<"共享中";

        auto chip = [](const QString &txt, const QString &bg) {
            return QString("<span style=\"background:%1;color:#ffffff;padding:1px 5px;border-radius:7px;\">%2</span>")
                .arg(bg, txt);
        };
        if (tile.cornerBadge) {
            QStringList chips;
            if (st.host) chips << chip("主", "#f59f00");
            else if (st.cohost) chips << chip("管", "#1971c2");
            chips << chip("麦", st.audio ? "#2f9e44" : "#c92a2a");
            chips << chip("摄", st.video ? "#2f9e44" : "#c92a2a");
            if (!st.pub) chips << chip("停", "#6c757d");
            if(st.share) chips<<chip("享","#2b8a3e");
            tile.cornerBadge->setText(chips.join(" "));
            tile.cornerBadge->adjustSize();
            tile.cornerBadge->move(tile.frame->width() - tile.cornerBadge->width() - 8, 8);
            tile.cornerBadge->show();
            tile.cornerBadge->raise();
        }

        if (oldStream != stream) {
            tile.hasFrame = false;
            tile.videoLabel->setPixmap(QPixmap());
        }
        if (!tile.hasFrame) {
            tile.videoLabel->setText(st.pub ? "双击聚焦并拉流" : "未推流");
        } else {
            tile.videoLabel->setText("");
        }

        const bool focused = focusMode && focusedStream == stream;
        if (focused) {
            tile.frame->setStyleSheet("QFrame{background:#101010;border:2px solid #2e86de;border-radius:6px;}");
        } else {
            tile.frame->setStyleSheet("QFrame{background:#101010;border:1px solid #4a4a4a;border-radius:6px;}");
        }
    }

    updateFocusStatusBadge();

    if (remoteStack) {
        if (focusMode && ui && ui->remoteVideolabel) {
            remoteStack->setCurrentWidget(ui->remoteVideolabel);
        } else if (remoteGridPage) {
            remoteStack->setCurrentWidget(remoteGridPage);
        }
    }
}

void MainWindow::applyTileFrame(const QString &stream, const QImage &img)
{
    if (stream.isEmpty()) return;
    const int idx = streamToTile.value(stream, -1);
    if (idx < 0 || idx >= remoteTiles.size()) return;

    auto &tile = remoteTiles[idx];
    if (!tile.videoLabel) return;

    tile.videoLabel->setPixmap(
        QPixmap::fromImage(img).scaled(tile.videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)
    );
    tile.videoLabel->setText("");
    tile.hasFrame = true;
}

void MainWindow::clearAllTileFrames()
{
    for (auto &tile : remoteTiles) {
        tile.hasFrame = false;
        if (tile.videoLabel) {
            tile.videoLabel->setPixmap(QPixmap());
            if (tile.stream.isEmpty()) {
                tile.videoLabel->setText("空席位");
            } else {
                const MemberState st = memberStates.value(tile.stream);
                tile.videoLabel->setText(st.pub ? "双击聚焦并拉流" : "未推流");
            }
        }
        if (tile.cornerBadge) {
            if (tile.stream.isEmpty()) tile.cornerBadge->hide();
            else tile.cornerBadge->show();
        }
    }
    updateFocusStatusBadge();
}

void MainWindow::updateFocusStatusBadge()
{
    if (!focusStatusLabel) return;

    if (!focusMode || focusedStream.isEmpty()) {
        focusStatusLabel->hide();
        return;
    }

    const MemberState st = memberStates.value(focusedStream);
    QStringList flags;
    flags << QString("焦点: %1").arg(focusedStream);
    flags << (st.audio ? "麦克风开" : "麦克风关");
    flags << (st.video ? "摄像头开" : "摄像头关");
    if (!st.pub) flags << "未推流";
    if(st.share) flags<<"共享中";

    focusStatusLabel->setText(flags.join("  |  "));
    focusStatusLabel->adjustSize();
    focusStatusLabel->move(10, 10);
    focusStatusLabel->show();
    focusStatusLabel->raise();
}

bool MainWindow::ensureRoomIdentity(bool askRoomIfEmpty)
{
    if (roomId.isEmpty() && loginRoomEdit) {
        roomId = loginRoomEdit->text().trimmed();
    }
    if (askRoomIfEmpty && roomId.isEmpty()) {
        showLoginOverlay(true, "请先输入房间号后再连接信令");
        return false;
    }
    if (roomId.isEmpty()) {
        roomId = QString::number(QRandomGenerator::global()->bounded(100000, 999999));
    }
    if (userId.isEmpty()) {
        if (!loginUser.isEmpty()) userId = loginUser;
        else userId = QString("u%1").arg(QRandomGenerator::global()->bounded(1000, 9999));
    }

    selfStream = QString("%1_%2").arg(roomId, userId);
    if (auto *meetingCodeLabel = findChild<QLabel*>("meetingCodeLabel")) {
        meetingCodeLabel->setText(QString("房间: %1  我: %2").arg(roomId, selfStream));
    }
    return true;
}

void MainWindow::appendRoomEvent(const QString &text)
{
    const QString msg = QString("[%1] %2")
                            .arg(QDateTime::currentDateTime().toString("hh:mm:ss"), text);
    recentEventLogs.enqueue(msg);
    while (recentEventLogs.size() > 200) {
        recentEventLogs.dequeue();
    }
    if (roomEventLog) {
        roomEventLog->appendPlainText(msg);
    }
    if (chatMessageLog && chatMessageLog != roomEventLog) {
        const QString chatLine = QString("[%1] 系统: %2")
                                     .arg(QDateTime::currentDateTime().toString("hh:mm:ss"), text);
        chatMessageLog->appendPlainText(chatLine);
    }
    qInfo() << msg;
}

void MainWindow::refreshRoomUserList()
{
    if (!roomUserList) return;

    // 聚焦流离开房间或停止推流时，自动退回宫格，避免主画面停留在失效流
    if (focusMode && !focusedStream.isEmpty()) {
        const bool exists = memberStates.contains(focusedStream);
        const bool isPublishingNow = exists && memberStates.value(focusedStream).pub;
        if (!exists || !isPublishingNow) {
            focusMode = false;
            focusedStream.clear();
            currentRemoteStream.clear();
            if (ui && ui->remoteVideolabel) {
                ui->remoteVideolabel->clear();
            }
            if (remoteStack && remoteGridPage) {
                remoteStack->setCurrentWidget(remoteGridPage);
            }
            appendRoomEvent("聚焦成员已离开或停推，已返回宫格视图");
        }
    }

    QString selectedStream;
    if (auto *cur = roomUserList->currentItem()) {
        selectedStream = cur->data(Qt::UserRole).toString();
    }

    QStringList streams = memberStates.keys();
    std::sort(streams.begin(), streams.end(), [this](const QString &a, const QString &b) {
        const MemberState sa = memberStates.value(a);
        const MemberState sb = memberStates.value(b);
        const int ra = sa.host ? 2 : (sa.cohost ? 1 : 0);
        const int rb = sb.host ? 2 : (sb.cohost ? 1 : 0);
        if (ra != rb) return ra > rb;
        return a < b;
    });

    auto makeStateIcon = [](const MemberState &st) -> QIcon {
        QPixmap pm(66, 18);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);

        auto drawChip = [&p](int x, const QColor &bg, const QString &txt) {
            QRect r(x, 1, 20, 16);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawRoundedRect(r, 7, 7);
            p.setPen(Qt::white);
            QFont f = p.font();
            f.setPointSize(8);
            f.setBold(true);
            p.setFont(f);
            p.drawText(r, Qt::AlignCenter, txt);
        };

        drawChip(0,  st.pub   ? QColor("#2f9e44") : QColor("#6c757d"), "P");
        drawChip(23, st.audio ? QColor("#2f9e44") : QColor("#c92a2a"), "M");
        drawChip(46, st.video ? QColor("#2f9e44") : QColor("#c92a2a"), "V");

        if (st.host || st.cohost) {
            const QColor roleColor = st.host ? QColor("#ffd43b") : QColor("#4dabf7");
            p.setPen(QPen(roleColor, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRect(0, 0, 65, 17), 8, 8);
        }
        return QIcon(pm);
    };

    roomUserList->blockSignals(true);
    roomUserList->clear();
    roomUserList->setIconSize(QSize(66, 18));
    for (const QString &stream : streams) {
        const MemberState st = memberStates.value(stream);
        QString text = stream;
        if (st.host) text += "  ·  主持人";
        else if (st.cohost) text += "  ·  联席主持人";
        else text += "  ·  成员";
        if (!st.pub) text += "  ·  未推流";
        if(st.share) text+="  .  共享中";

        auto *item = new QListWidgetItem(text, roomUserList);
        item->setData(Qt::UserRole, stream);
        item->setIcon(makeStateIcon(st));
        const QString roleText = st.host ? "主持人" : (st.cohost ? "联席主持人" : "成员");
        item->setToolTip(QString("角色: %1\n推流: %2\n麦克风: %3\n摄像头: %4\n共享: %5")
                             .arg(roleText)
                             .arg(st.pub ? "开启" : "关闭")
                             .arg(st.audio ? "开启" : "关闭")
                             .arg(st.video ? "开启" : "关闭")
                             .arg(st.share?"是":"否"));

        if (st.host || st.cohost) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
        if (!selectedStream.isEmpty() && stream == selectedStream) {
            roomUserList->setCurrentItem(item);
        }
    }
    roomUserList->blockSignals(false);

    if (roomCountLabel) {
        roomCountLabel->setText(QString("在线: %1").arg(streams.size()));
    }
    applyWhiteboardLockUi();
    refreshRemoteTiles();
    syncGridPullers();
}

void MainWindow::sendSignalJoin()
{
    if (!signalAuthed) {
        appendRoomEvent("尚未完成登录，已阻止入会");
        return;
    }
    if (!signalSocket || !signalConnected) return;
    if (!ensureRoomIdentity(false)) return;

    QJsonObject obj;
    obj["type"] = "join";
    obj["room"] = roomId;
    obj["user"] = userId;
    obj["stream"] = selfStream;
    obj["audio"] = localAudioOn;
    obj["video"] = localVideoOn;
    obj["pub"] = isPublishing;
    obj["share"]=localScreenShareOn;

    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    appendRoomEvent(QString("已加入房间 %1，用户 %2").arg(roomId, selfStream));
}

void MainWindow::sendSignalLeave()
{
    if (!signalSocket || signalSocket->state() != QAbstractSocket::ConnectedState) return;

    QJsonObject obj;
    obj["type"] = "leave";
    obj["room"] = roomId;
    obj["stream"] = selfStream;
    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void MainWindow::sendSignalUpdate()
{
    if (!signalSocket || !signalConnected) return;

    QJsonObject obj;
    obj["type"] = "update";
    obj["room"] = roomId;
    obj["stream"] = selfStream;
    obj["audio"] = localAudioOn;
    obj["video"] = localVideoOn;
    obj["pub"] = isPublishing;
    obj["share"]=localScreenShareOn;

    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void MainWindow::sendSignalCmd(const QString &toStream, const QString &action)
{
    if (!signalSocket || !signalConnected || toStream.isEmpty() || action.isEmpty()) return;

    QJsonObject obj;
    obj["type"] = "cmd";
    obj["room"] = roomId;
    obj["to"] = toStream;
    obj["action"] = action;
    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    qInfo() << "[SignalCmd] room=" << roomId << "to=" << toStream << "action=" << action;
}

void MainWindow::sendSignalChat(const QString &content)
{
    if(!signalSocket||!signalConnected) return;
    if(content.trimmed().isEmpty()) return;

    QJsonObject obj;
    obj["type"]="chat";
    obj["room"]=roomId;
    obj["user"]=userId;
    obj["stream"]=selfStream;
    obj["content"]=content;

    const qint64 nowMs=QDateTime::currentMSecsSinceEpoch();
    obj["msg_id"]=QString("%1_%2_%3").arg(selfStream).arg(nowMs).arg(++chatLocalSeq);
    obj["ts"]=nowMs ;
    obj["ver"]=1;

    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void MainWindow::chooseShareSource()
{
    const auto candidates=buildShareSourceCandidates();
    if(candidates.isEmpty()){
        appendRoomEvent("未发现可共享源");
        return;
    }

    QStringList labels;
    int currentIndex=0;
    for(int i=0;i<candidates.size();++i){
        labels<<candidates[i].label;
        const bool match=candidates[i].isWindow
                            ?(shareWindowId!=0&&candidates[i].windowId==shareWindowId)
                            :(shareWindowId==0&&candidates[i].screenIndex==shareScreenIndex);
        if(match) currentIndex=i;
    }

    bool ok=false;
    const QString picked=QInputDialog::getItem(this,"选择共享源","请选择共享对象:",labels,currentIndex,false,&ok);
    if(!ok||picked.isEmpty()) return;

    const int idx=labels.indexOf(picked);
    if(idx<0) return;

    const auto &sel=candidates[idx];
    shareWindowId=sel.windowId;
    shareScreenIndex=sel.screenIndex;
    shareSourceName=sel.label;

    appendRoomEvent(QString("共享源已切换：%1").arg(shareSourceName));

    if(localScreenShareOn){
        applyLocalVideoSource();//共享中切源即时生效
    }
    refreshSelfControlActions();
}

QVector<MainWindow::ShareSourceCandidate> MainWindow::buildShareSourceCandidates() const
{
    QVector<ShareSourceCandidate> out;

    const auto screens=QGuiApplication::screens();
    for(int i=0;i<screens.size();++i){
        ShareSourceCandidate c;
        c.isWindow=false;
        c.screenIndex=i;
        c.windowId=0;
        c.label=QString("屏幕%1 (%2x%3) ").arg(i+1).arg(screens[i]->size().width()).arg(screens[i]->size().height());
        out.push_back(c);
    }

#ifdef Q_OS_WIN
    QVector<WinShareEntry> winList;
    EnumWindows(enumShareWindowProc,reinterpret_cast<LPARAM>(&winList));
    QSet<quint64> seen;
    for(const auto &w:winList){
        if(seen.contains(w.hwnd)) continue;
        seen.insert(w.hwnd);
        ShareSourceCandidate c;
        c.isWindow=true;
        c.windowId=w.hwnd;
        c.label=QString("窗口：%1").arg(w.title);
        out.push_back(c);
    }
#endif
    return out;
}

void MainWindow::appendChatMessage(const QString &fromStream, const QString &content, qint64 tsMs, bool isSelf, const QString &msgId)
{
    if(content.trimmed().isEmpty()) return;

    //消息去重，同一个msg_id只显示一次
    if(!msgId.isEmpty()){
        if(seenChatMsgIds.contains(msgId)) return;
        seenChatMsgIds.insert(msgId);
        if(seenChatMsgIds.size()>1000){
            seenChatMsgIds.clear();//简单限界，避免集合无限增长
        }
    }

    const QString timeText=
        (tsMs>0)
        ? QDateTime::fromMSecsSinceEpoch(tsMs).toString("hh:mm:ss")
        : QDateTime::currentDateTime().toString("hh:mm:ss");

    const QString who=isSelf ? QStringLiteral("我"):fromStream;
    const QString line=QString("[%1] %2: %3").arg(timeText,who,content);

    if(chatMessageLog){
        chatMessageLog->appendPlainText(line);
    }
    qInfo()<<"[chat]"<<line;
}

void MainWindow::onSignalConnected()
{
    if (shuttingDown) return;
    const int reconnectSnapshot = signalReconnectAttempt;
    manualSignalDisconnect = false;
    resetSignalReconnectState();

    signalConnected = true;
    signalAuthed = false;
    authRegisterTried = pendingAuthRegister;
    if (signalStateLabel) signalStateLabel->setText("信令: 已连接");
    if (auto *signalBadgeLabel = findChild<QLabel*>("signalBadgeLabel")) {
        signalBadgeLabel->setText("信令在线");
    }
    if (ui && ui->startReceiveButton) ui->startReceiveButton->setText("断开信令");

    if (reconnectSnapshot > 0) {
        ++totalSignalReconnectCount;
        appendRoomEvent(QString("信令重连成功（第 %1 次）").arg(reconnectSnapshot));
    }
    appendRoomEvent(pendingAuthRegister ? "信令已连接，正在注册..." : "信令已连接，正在登录...");
    if (pendingAuthRegister) {
        sendSignalAuthRegister();
    } else {
        sendSignalAuthLogin();
    }
    refreshSelfControlActions();
    updateMeetingStatsUi();
}

void MainWindow::onSignalDisconnected()
{
    signalAuthed = false;
    authRegisterTried = false;
    signalConnected = false;
    if (shuttingDown) return;
    if (signalStateLabel) signalStateLabel->setText("信令: 未连接");
    if (auto *signalBadgeLabel = findChild<QLabel*>("signalBadgeLabel")) {
        signalBadgeLabel->setText("信令离线");
    }
    if (ui && ui->startReceiveButton) ui->startReceiveButton->setText("连接信令");
    appendRoomEvent("信令已断开");

    // 手动断开信令时，立即停止当前拉流，避免继续播放远端。
    stopCurrentPull(true);
    focusedStream.clear();
    focusMode = false;
    clearAllTileFrames();
    if (remoteStack && remoteGridPage) {
        remoteStack->setCurrentWidget(remoteGridPage);
    }
    if (ui && ui->remoteVideolabel) {
        ui->remoteVideolabel->clear();
    }
    memberStates.clear();
    roomHostStream.clear();
    refreshRoomUserList();
    refreshSelfControlActions();
    seenChatMsgIds.clear();
    if(chatInputEdit) chatInputEdit->clear();
    seenWhiteboardMsgIds.clear();
    whiteboardMouseDown=false;
    clearWhiteboard(false);

    if (!manualSignalDisconnect && !meetingStopped && !shuttingDown) {
        scheduleSignalReconnect("检测到信令断开");
    }
    updateMeetingStatsUi();
}

void MainWindow::onSignalTextMessage(const QString &msg)
{
    if (shuttingDown) return;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
    const QJsonObject obj = doc.object();
    const QString type = obj.value("type").toString();

    if (type == "auth_ok") {
        signalAuthed = true;
        authRegisterTried = false;
        pendingAuthRegister = false;
        loginUser = obj.value("user").toString(loginUser);
        if (!loginUser.isEmpty()) userId = loginUser;
        appendRoomEvent(QString("登录成功: %1").arg(loginUser));
        showLoginOverlay(false);
        sendSignalJoin();
        return;
    }

    if (type == "auth_registered") {
        appendRoomEvent("账号注册成功，正在登录...");
        pendingAuthRegister = false;
        sendSignalAuthLogin();
        return;
    }

    if (type == "auth_fail") {
        const QString code = obj.value("code").toString();
        const QString msgText = obj.value("msg").toString("登录失败");
        appendRoomEvent(QString("登录失败[%1]: %2").arg(code, msgText));
        pendingAuthRegister = false;
        signalAuthed = false;
        QString hint;
        if (code == "no_user") {
            hint = "账号不存在，请点击“注册并进入”";
        } else if (code == "bad_password") {
            hint = "密码错误，请检查后重试";
        } else if (code == "exists") {
            hint = "账号已存在，请点击“登录并进入”";
        } else if (code == "bad_args") {
            hint = "账号或密码格式不正确";
        } else if (code == "db_error") {
            hint = "服务端数据库异常，请稍后重试";
        } else {
            hint = QString("登录失败：%1").arg(msgText);
        }
        showLoginOverlay(true, hint);
        return;
    }

    if (type == "auth_required") {
        signalAuthed = false;
        appendRoomEvent("服务端要求先登录");
        showLoginOverlay(true, "服务端要求先登录");
        return;
    }


    if(type=="chat"){
        const QString room=obj.value("room").toString();
        if(!roomId.isEmpty()&&room!=roomId) return;

        const QString fromStream=obj.value("stream").toString();
        const QString content=obj.value("content").toString();
        const QString msgId=obj.value("msg_id").toString();
        const qint64 tsMs=obj.value("ts").toVariant().toLongLong();

        if(fromStream.isEmpty()||content.trimmed().isEmpty()) return;

        appendChatMessage(fromStream,content,tsMs,fromStream==selfStream,msgId);
        return;
    }

    if(type=="wb"){
        const QString room=obj.value("room").toString();
        if(!roomId.isEmpty()&&room!=roomId) return;

        const QString fromStream=obj.value("stream").toString();
        if(fromStream.isEmpty()) return;
        if(fromStream==selfStream) return;//自己发的本地已画，避免重复

        const QString msgId=obj.value("msg_id").toString();
        if(!msgId.isEmpty()){
            if(seenWhiteboardMsgIds.contains(msgId)) return;
            seenWhiteboardMsgIds.insert(msgId);
            if(seenWhiteboardMsgIds.size()>3000) seenWhiteboardMsgIds.clear();
        }

        const QString op=obj.value("op").toString();
        if(op=="lock"||op=="unlock"){
            whiteboardLocked=(op=="lock");
            applyWhiteboardLockUi();
            appendRoomEvent(whiteboardLocked
                            ? QString("%1 锁定了白板").arg(fromStream)
                            : QString("%1 解锁了白板").arg(fromStream));
            return;
        }
        if(op=="clear"){
            clearWhiteboard(false,fromStream);
            return;
        }
        if(op=="draw"){
            ensureWhiteboardCanvas();
            if(whiteboardCanvas.isNull()) return;

            const int w=qMax(1,whiteboardCanvas.width()-1);
            const int h=qMax(1,whiteboardCanvas.height()-1);

            auto denormX=[&](int n){ return qBound(0,(n*w)/10000,w);};
            auto denormY=[&](int n){ return qBound(0,(n*h)/10000,h);};

            const QPoint p1(denormX(obj.value("x1n").toInt()),denormY(obj.value("y1n").toInt()));
            const QPoint p2(denormX(obj.value("x2n").toInt()),denormY(obj.value("y2n").toInt()));

            const QColor c(obj.value("color").toString("#e03131"));
            const int pw=qBound(1,obj.value("pw").toInt(3),12);
            const QString sid=obj.value("stroke_id").toString(obj.value("msg_id").toString());

            drawWhiteboardLine(p1,p2,false,c,pw,sid,fromStream);
            return;
        }
        if(op=="undo"){
            const QString sid=obj.value("stroke_id").toString();
            removeStrokeById(sid,fromStream);
            return;
        }
    }
    if (type == "members") {
        const QString room = obj.value("room").toString();
        if (!roomId.isEmpty() && room != roomId) return;

        const bool serverWbLock=obj.value("wb_lock").toBool(false);
        if(whiteboardLocked!=serverWbLock){
            whiteboardLocked=serverWbLock;
            applyWhiteboardLockUi();
        }

        const bool oldSelfHost = memberStates.value(selfStream).host;
        const bool oldSelfCoHost = memberStates.value(selfStream).cohost;
        QHash<QString, MemberState> newStates;
        roomHostStream.clear();

        const QJsonArray arr = obj.value("members").toArray();
        for (const QJsonValue &v : arr) {
            if (!v.isObject()) continue;
            const QJsonObject m = v.toObject();
            MemberState st;
            st.user = m.value("user").toString();
            st.stream = m.value("stream").toString();
            st.audio = m.value("audio").toBool(true);
            st.video = m.value("video").toBool(true);
            st.pub = m.value("pub").toBool(false);
            st.share=m.value("share").toBool(false);
            const QString role = m.value("role").toString();
            st.host = (role == "host");
            st.cohost = (role == "cohost");
            if (st.stream.isEmpty()) continue;
            if (st.host) roomHostStream = st.stream;
            newStates.insert(st.stream, st);
        }

        if (roomHostStream.isEmpty() && !newStates.isEmpty()) {
            QStringList streams = newStates.keys();
            std::sort(streams.begin(), streams.end());
            roomHostStream = streams.first();
            newStates[roomHostStream].host = true;
        }

        memberStates = newStates;
        refreshRoomUserList();

        if (memberStates.contains(selfStream)) {
            const MemberState selfState = memberStates.value(selfStream);
            const bool oldAudio = localAudioOn;
            const bool oldVideo = localVideoOn;
            localAudioOn = selfState.audio;
            localVideoOn = selfState.video;

            if (oldAudio != localAudioOn) {
                appendRoomEvent(localAudioOn ? "麦克风已恢复" : "主持人已将你静音");
            }
            if (oldVideo != localVideoOn) {
                appendRoomEvent(localVideoOn ? "摄像头已开启" : "主持人已关闭你的摄像头");
            }
            const bool oldShare=localScreenShareOn;
            localScreenShareOn=selfState.share;
            if(oldShare!=localScreenShareOn){
                appendRoomEvent(localScreenShareOn?"屏幕共享已开启":"屏幕共享已停止");
                applyLocalVideoSource();
            }
            if (!oldSelfHost && selfState.host) {
                appendRoomEvent("你已成为主持人");
            } else if (oldSelfHost && !selfState.host) {
                appendRoomEvent("你的主持人身份已转移");
            }
            if (!oldSelfCoHost && selfState.cohost) {
                appendRoomEvent("你已成为联席主持人");
            } else if (oldSelfCoHost && !selfState.cohost && !selfState.host) {
                appendRoomEvent("你的联席主持人身份已取消");
            }
            if (oldAudio != localAudioOn || oldVideo != localVideoOn||oldShare!=localScreenShareOn) {
                refreshSelfControlActions();
            }
        }

        // 不在 members 刷新里主动 stopCurrentPull()，避免状态瞬时抖动触发误停拉流。
        // 拉流切换仅由用户双击、断开信令、结束会议这三条路径控制。
        return;
    }

    if (type == "ctrl") {
        const QString to = obj.value("to").toString();
        const QString action = obj.value("action").toString();
        if (to != selfStream) return;

        if (action == "mute_audio") {
            localAudioOn = false;
            appendRoomEvent("主持人已将你静音");
            refreshSelfControlActions();
            return;
        }
        if (action == "mute_video") {
            localVideoOn = false;
            appendRoomEvent("主持人已关闭你的摄像头");
            refreshSelfControlActions();
            return;
        }
        if (action == "unmute_audio" || action == "restore_audio" || action == "audio_on") {
            localAudioOn = true;
            appendRoomEvent("主持人已恢复你的麦克风");
            refreshSelfControlActions();
            return;
        }
        if (action == "unmute_video" || action == "restore_video" || action == "video_on") {
            localVideoOn = true;
            appendRoomEvent("主持人已开启你的摄像头");
            refreshSelfControlActions();
            return;
        }
        if (action == "set_host") {
            appendRoomEvent("你被设为主持人");
            return;
        }
        if (action == "set_cohost") {
            appendRoomEvent("你被设为联席主持人");
            return;
        }
        if (action == "unset_cohost") {
            appendRoomEvent("你的联席主持人身份被取消");
            return;
        }
        if (action == "kick") {
            appendRoomEvent("你已被主持人移出会议");
            on_stopMeetingButton_clicked();
            return;
        }
        if(action=="stop_share"){
            localScreenShareOn=false;
            applyLocalVideoSource();
            appendRoomEvent("主持人已停止你的屏幕共享");
            sendSignalUpdate();
            refreshSelfControlActions();
            return;
        }
    }
}

void MainWindow::onRoomUserDoubleClicked(QListWidgetItem *item)
{
    if (!item) return;
    const QString stream = item->data(Qt::UserRole).toString();
    if (stream.isEmpty()) return;
    const MemberState st = memberStates.value(stream);
    if (!st.pub) {
        appendRoomEvent("该成员未推流，无法拉取");
        return;
    }
    focusMode = true;
    focusedStream = stream;
    if (remoteStack && ui && ui->remoteVideolabel) {
        remoteStack->setCurrentWidget(ui->remoteVideolabel);
    }
    preferredRemoteStream = stream;
    startPullStream(stream);
    refreshRemoteTiles();
}

void MainWindow::onRoomListContextMenu(const QPoint &pos)
{
    if (!roomUserList) return;
    QListWidgetItem *item = roomUserList->itemAt(pos);
    if (!item) return;

    const QString targetStream = item->data(Qt::UserRole).toString();
    if (targetStream.isEmpty()) return;
    const MemberState selfState = memberStates.value(selfStream);
    const bool selfIsHost = selfState.host;
    const bool selfIsCoHost = selfState.cohost;

    QMenu menu(this);
    QAction *picked = nullptr;

    if (targetStream == selfStream) {
        QAction *selfAudioToggle = menu.addAction(localAudioOn ? "静音我自己" : "恢复我自己的麦克风");
        QAction *selfVideoToggle = menu.addAction(localVideoOn ? "关闭我自己的摄像头" : "开启我自己的摄像头");

        QAction *allMuteAudio = nullptr;
        QAction *allUnmuteAudio = nullptr;
        QAction *allMuteVideo = nullptr;
        QAction *allUnmuteVideo = nullptr;
        QAction *allStopShare=nullptr;

        if (selfIsHost) {
            menu.addSeparator();
            allMuteAudio = menu.addAction("全体静音（不含自己）");
            allUnmuteAudio = menu.addAction("全体恢复麦克风（不含自己）");
            menu.addSeparator();
            allMuteVideo = menu.addAction("全体关闭摄像头（不含自己）");
            allUnmuteVideo = menu.addAction("全体开启摄像头（不含自己）");
            allStopShare=menu.addAction("全体停止共享(不含自己");
        }

        picked = menu.exec(roomUserList->viewport()->mapToGlobal(pos));
        if (!picked) return;

        if (picked == selfAudioToggle) {
            localAudioOn = !localAudioOn;
            sendSignalUpdate();
            appendRoomEvent(localAudioOn ? "你已恢复麦克风" : "你已静音自己");
            refreshSelfControlActions();
            return;
        }
        if (picked == selfVideoToggle) {
            localVideoOn = !localVideoOn;
            sendSignalUpdate();
            appendRoomEvent(localVideoOn ? "你已开启摄像头" : "你已关闭自己的摄像头");
            refreshSelfControlActions();
            return;
        }

        if (!selfIsHost) return;

        auto sendToAll = [this](const QString &action, const QString &eventText) {
            int sent = 0;
            for (auto it = memberStates.cbegin(); it != memberStates.cend(); ++it) {
                const QString s = it.key();
                if (s.isEmpty() || s == selfStream) continue;
                sendSignalCmd(s, action);
                ++sent;
            }
            appendRoomEvent(QString("%1，已发送 %2 人").arg(eventText).arg(sent));
        };

        if (picked == allMuteAudio) {
            sendToAll("mute_audio", "主持人执行全体静音");
        } else if (picked == allUnmuteAudio) {
            sendToAll("unmute_audio", "主持人执行全体恢复麦克风");
        } else if (picked == allMuteVideo) {
            sendToAll("mute_video", "主持人执行全体关闭摄像头");
        } else if (picked == allUnmuteVideo) {
            sendToAll("unmute_video", "主持人执行全体开启摄像头");
        }else if(picked==allStopShare){
            sendToAll("stop_share","主持人执行全体停止共享");
        }
        return;
    }

    if (!(selfIsHost || selfIsCoHost)) {
        appendRoomEvent("仅主持人/联席主持人可管理其他成员");
        return;
    }

    const MemberState targetState = memberStates.value(targetStream);
    if (!selfIsHost && targetState.host) {
        appendRoomEvent("联席主持人不可管理主持人");
        return;
    }

    QAction *muteAudio = menu.addAction("静音该成员");
    QAction *unmuteAudio = menu.addAction("恢复该成员麦克风");
    menu.addSeparator();
    QAction *muteVideo = menu.addAction("关闭该成员摄像头");
    QAction *unmuteVideo = menu.addAction("开启该成员摄像头");
    menu.addSeparator();
    QAction *kick = menu.addAction("踢出该成员");

    QAction *setCoHost = nullptr;
    QAction *unsetCoHost = nullptr;
    QAction *transferHost = nullptr;
    QAction *stopShare=nullptr;
    if (selfIsHost) {
        menu.addSeparator();
        if (!targetState.host && !targetState.cohost) {
            setCoHost = menu.addAction("邀请为联席主持人");
        } else if (targetState.cohost) {
            unsetCoHost = menu.addAction("取消联席主持人");
        }
        if (!targetState.host) {
            transferHost = menu.addAction("转移主持人给该成员");
        }
        if(targetState.share){
            menu.addSeparator();
            stopShare=menu.addAction("停止该成员共享");
        }
    }

    picked = menu.exec(roomUserList->viewport()->mapToGlobal(pos));
    if (!picked) return;

    if (picked == muteAudio) {
        sendSignalCmd(targetStream, "mute_audio");
        appendRoomEvent(QString("已对 %1 发送静音").arg(targetStream));
    } else if (picked == unmuteAudio) {
        sendSignalCmd(targetStream, "unmute_audio");
        appendRoomEvent(QString("已对 %1 发送恢复麦克风").arg(targetStream));
    } else if (picked == muteVideo) {
        sendSignalCmd(targetStream, "mute_video");
        appendRoomEvent(QString("已对 %1 发送关闭摄像头").arg(targetStream));
    } else if (picked == unmuteVideo) {
        sendSignalCmd(targetStream, "unmute_video");
        appendRoomEvent(QString("已对 %1 发送开启摄像头").arg(targetStream));
    } else if (picked == kick) {
        sendSignalCmd(targetStream, "kick");
        appendRoomEvent(QString("已踢出 %1").arg(targetStream));
    } else if (picked == setCoHost) {
        sendSignalCmd(targetStream, "set_cohost");
        appendRoomEvent(QString("已邀请 %1 为联席主持人").arg(targetStream));
    } else if (picked == unsetCoHost) {
        sendSignalCmd(targetStream, "unset_cohost");
        appendRoomEvent(QString("已取消 %1 的联席主持人身份").arg(targetStream));
    } else if (picked == transferHost) {
        const auto ret = QMessageBox::question(
            this, "转移主持人",
            QString("确认将主持人转移给 %1 ？").arg(targetStream)
        );
        if (ret == QMessageBox::Yes) {
            sendSignalCmd(targetStream, "set_host");
            appendRoomEvent(QString("已将主持人转移给 %1").arg(targetStream));
        }
    }else if(picked==stopShare){
        sendSignalCmd(targetStream,"stop_share");
        appendRoomEvent(QString("已对%1发送停止共享").arg(targetStream));
    }
}

void MainWindow::onSendChatClicked()
{
    if(!chatInputEdit) return;

    if(!signalConnected||!signalSocket||signalSocket->state()!=QAbstractSocket::ConnectedState){
        appendRoomEvent("信令未连接,无法发送聊天消息");
        return;
    }

    if(!ensureRoomIdentity(false)) return;

    QString content=chatInputEdit->toPlainText().trimmed();
    if(content.isEmpty()) return;

    //先做硬限制,避免超长消息刷屏
    if(content.size()>500){
        content=content.left(500);
    }

    sendSignalChat(content);
    chatInputEdit->clear();
}

QStringList MainWindow::currentDisplayStreams() const
{
    QStringList streams;
    for (const auto &tile : remoteTiles) {
        if (tile.stream.isEmpty()) continue;
        const MemberState st = memberStates.value(tile.stream);
        if (st.pub) streams.push_back(tile.stream);
    }
    return streams;
}

void MainWindow::applyFocusAudioRouting()
{
    for (auto it = pullSessions.begin(); it != pullSessions.end(); ++it) {
        PullSession *sess = it.value();
        if (!sess || !sess->puller) continue;
        const bool enable = focusMode && !focusedStream.isEmpty() && sess->stream == focusedStream;
        QMetaObject::invokeMethod(sess->puller, "setAudioEnabled", Qt::QueuedConnection, Q_ARG(bool, enable));
    }
}

void MainWindow::ensurePullSession(const QString &stream)
{
    if (stream.isEmpty()) return;
    if (pullSessions.contains(stream)) return;

    PullSession *sess = new PullSession;
    sess->stream = stream;
    sess->puller = new rtmppuller(nullptr);
    sess->thread = new QThread(this);
    sess->thread->setObjectName(QString("pullThread_%1").arg(stream));
    sess->puller->moveToThread(sess->thread);
    pullSessions.insert(stream, sess);

    connect(sess->puller, &rtmppuller::finished, sess->thread, &QThread::quit, Qt::QueuedConnection);
    connect(sess->puller, &rtmppuller::videoFrameReady, this, [this, stream](const QImage &img) {
        if (PullSession *s = pullSessions.value(stream, nullptr)) {
            s->retryCount = 0;
            s->retryEpoch = 0;
        }
        latestRemoteFrames.insert(stream, img);
        applyTileFrame(stream, img);
        if (focusMode && focusedStream == stream && ui && ui->remoteVideolabel) {
            ui->remoteVideolabel->setPixmap(
                QPixmap::fromImage(img).scaled(ui->remoteVideolabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)
            );
        }
        if (camRecording && recorder && recorder->isOpen()) {
            const bool useRemoteRecord = (!isPublishing || !videoWorker);
            if (useRemoteRecord && focusMode && focusedStream == stream) {
                recorder->pushVideoFrame(img);
            }
        }
    });
    connect(sess->puller, &rtmppuller::audioPcmReady, this, [this, stream](const QByteArray &pcm, int sampleRate, int channels) {
        Q_UNUSED(sampleRate);
        Q_UNUSED(channels);
        if (!(camRecording && recorder && recorder->isOpen())) return;
        const bool useRemoteRecord = (!isPublishing || !audioWorker);
        if (!useRemoteRecord) return;
        if (!(focusMode && focusedStream == stream)) return;
        if (pcm.isEmpty()) return;
        const int nbSamples = pcm.size() / 2; // mono s16
        if (nbSamples <= 0) return;
        recorder->pushAudioPCM(reinterpret_cast<const uint8_t*>(pcm.constData()), nbSamples);
    });
    connect(sess->puller, &rtmppuller::errorOccurred, this, [this, stream](const QString &e) {
        PullSession *s = pullSessions.value(stream, nullptr);
        if (!s) return;
        appendRoomEvent(QString("拉流错误(%1): %2").arg(stream, e));

        const QString errLower = e.toLower();
        const bool nonNetwork =
            errLower.contains("operation not permitted") ||
            errLower.contains("no such stream") ||
            errLower.contains("immediate exit requested");
        if (!nonNetwork && (stream == focusedStream || stream == selfStream)) {
            handleNetworkIssue(QString("拉流抖动(%1)").arg(stream), 1);
        }

        if (meetingStopped || !signalConnected) return;
        const QStringList needed = currentDisplayStreams();
        if (!needed.contains(stream)) return;

        if (++s->retryCount > 8) {
            appendRoomEvent(QString("拉流重试超过上限: %1").arg(stream));
            return;
        }

        const int retryIndex = s->retryCount;
        const int exp = qMin(retryIndex - 1, 4);
        const int baseDelay = 300 * (1 << exp);
        const int jitter = QRandomGenerator::global()->bounded(120);
        const int delayMs = qMin(5000, baseDelay + jitter);
        const quint64 epoch = ++s->retryEpoch;
        ++totalPullRetryCount;
        updateMeetingStatsUi();
        appendRoomEvent(QString("准备重试拉流(%1/8): %2").arg(retryIndex).arg(stream));

        QTimer::singleShot(delayMs, this, [this, stream, epoch]() {
            if (meetingStopped || !signalConnected) return;
            PullSession *s2 = pullSessions.value(stream, nullptr);
            if (!s2) return;
            if (s2->retryEpoch != epoch) return;
            const QStringList needed2 = currentDisplayStreams();
            if (!needed2.contains(stream)) return;
            stopPullSession(stream, false);
            ensurePullSession(stream);
            applyFocusAudioRouting();
        });
    });

    sess->thread->start(QThread::HighPriority);
    const QString url = QString("rtmp://127.0.0.1/live/%1").arg(stream);
    QMetaObject::invokeMethod(sess->puller, "startPull", Qt::QueuedConnection, Q_ARG(QString, url));
    appendRoomEvent(QString("开始拉流: %1").arg(stream));
}

void MainWindow::stopPullSession(const QString &stream, bool waitForQuit)
{
    PullSession *sess = pullSessions.value(stream, nullptr);
    if (!sess) return;

    latestRemoteFrames.remove(stream);
    if (sess->puller) {
        disconnect(sess->puller, nullptr, this, nullptr);
        sess->puller->stop();
    }

    bool threadStopped = true;
    if (sess->thread) {
        sess->thread->quit();
        int waitMs = waitForQuit ? 2500 : 200;
        threadStopped = sess->thread->wait(waitMs);
        if (!threadStopped) {
            qWarning() << "[RtmpPuller]" << stream << "stop wait timeout, detach";
            sess->thread->requestInterruption();
            if (sess->puller) {
                QObject::connect(sess->thread, &QThread::finished, sess->puller, &QObject::deleteLater, Qt::UniqueConnection);
            }
            sess->thread->setParent(nullptr);
            QObject::connect(sess->thread, &QThread::finished, sess->thread, &QObject::deleteLater, Qt::UniqueConnection);
        } else {
            delete sess->thread;
        }
        sess->thread = nullptr;
    }

    // 仅在线程确认结束后同步销毁 puller；否则交由 finished->deleteLater 释放
    if (threadStopped && sess->puller) {
        delete sess->puller;
        sess->puller = nullptr;
    } else if (!threadStopped) {
        sess->puller = nullptr;
    }

    pullSessions.remove(stream);
    delete sess;
}

void MainWindow::stopAllPullSessions(bool waitForQuit)
{
    const QStringList keys = pullSessions.keys();
    for (const QString &s : keys) {
        stopPullSession(s, waitForQuit);
    }
}

void MainWindow::syncGridPullers()
{
    if (!signalConnected || meetingStopped) {
        stopAllPullSessions(false);
        return;
    }

    const QStringList neededList = currentDisplayStreams();
    const QSet<QString> needed = QSet<QString>(neededList.begin(), neededList.end());
    const QStringList current = pullSessions.keys();

    for (const QString &s : current) {
        if (!needed.contains(s)) {
            stopPullSession(s, false);
        }
    }
    for (const QString &s : needed) {
        ensurePullSession(s);
    }
    applyFocusAudioRouting();
}

void MainWindow::startPullStream(const QString &stream)
{
    if (stream.isEmpty()) return;
    currentRemoteStream = stream;
    focusedStream = stream;
    focusMode = true;
    if (remoteStack && ui && ui->remoteVideolabel) {
        remoteStack->setCurrentWidget(ui->remoteVideolabel);
    }
    ensurePullSession(stream);
    applyFocusAudioRouting();
}

void MainWindow::stopCurrentPull(bool waitForQuit)
{
    stopAllPullSessions(waitForQuit);
    currentRemoteStream.clear();
}
