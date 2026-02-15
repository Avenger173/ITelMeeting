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
#include <QToolButton>
#include <algorithm>

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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , timer(new QTimer(this))
{
    ui->setupUi(this);
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
}

MainWindow::~MainWindow()
{
    qDebug()<<"[MainWindow] 析构";

    on_stopMeetingButton_clicked();
    if(signalSocket){
        delete signalSocket;
        signalSocket=nullptr;
    }

    auto tmp=ui;
    ui=nullptr;
    delete tmp;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    on_stopMeetingButton_clicked();
    QMainWindow::closeEvent(event);
}

void MainWindow::on_startMeetingButton_clicked()
{
    meetingStopped = false;
    audioStopped = false;
    localAudioOn = true;
    localVideoOn = true;
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
                localLabel->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
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

    bool aok = false;
    QMetaObject::invokeMethod(audioEnc, [&]() {
        aok = audioEnc->open(44100, 1);
    }, Qt::BlockingQueuedConnection);
    if (!aok) {
        qWarning() << "[Mainwindow] 音频编码器打开失败";
    }

    bool encOk = false;
    QMetaObject::invokeMethod(netEnc, [&]() {
        encOk = netEnc->openVideo(640, 480, 30);
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

    const QString pushUrl = QString("rtmp://127.0.0.1/live/%1").arg(selfStream);
    bool rtmpOk = false;
    QMetaObject::invokeMethod(pusher, [&]() {
        pusher->setVideoParams(640, 480, 30);
        pusher->setAudioParams(44100, 1);
        rtmpOk = pusher->start(pushUrl, 30, 44100);
    },Qt::BlockingQueuedConnection);

    if (!rtmpOk) {
        qWarning() << "[Mainwindow] RTMP推流启动失败";
        isPublishing = false;
    } else {
        qDebug() << "[Mainwindow] RTMP推流已启动";
        isPublishing = true;
    }

    appendRoomEvent(QString("开始会议，推流: %1").arg(pushUrl));
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
    isPublishing = false;
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
    if (remoteStack && remoteGridPage) {
        remoteStack->setCurrentWidget(remoteGridPage);
    }
    if (ui && ui->remoteVideolabel) {
        ui->remoteVideolabel->clear();
    }

    qDebug() << "[Mainwindow] 会议已结束";
    stopMeetingInProgress = false;
}

void MainWindow::on_captureImageButton_clicked()
{
    if (videoWorker) {
        QString filename = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss") + ".jpg";
        videoWorker->capturePhoto(filename);
        QMessageBox::information(this, "提示", "已保存照片：" + filename);
    }
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


void MainWindow::on_startReceiveButton_clicked()
{
    if (signalConnected) {
        appendRoomEvent("手动断开信令");
        if (signalSocket) {
            sendSignalLeave();
            signalSocket->close();
        }
        return;
    }
    if (!ensureRoomIdentity(true)) {
        return;
    }
    if (!signalSocket) {
        signalSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        connect(signalSocket, &QWebSocket::connected, this, &MainWindow::onSignalConnected);
        connect(signalSocket, &QWebSocket::disconnected, this, &MainWindow::onSignalDisconnected);
        connect(signalSocket, &QWebSocket::textMessageReceived, this, &MainWindow::onSignalTextMessage);
        connect(signalSocket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError){
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
    qInfo() << "[Mainwindow] 接收端已启动(信令)";
}

void MainWindow::setupSignalUi()
{
    if (roomDock && roomUserList && roomEventLog && signalStateLabel && roomCountLabel) return;

    // 优先使用 UI(XML) 中已有控件。
    roomDock = findChild<QDockWidget*>("roomDock");
    signalStateLabel = findChild<QLabel*>("signalStateLabel");
    roomCountLabel = findChild<QLabel*>("roomCountLabel");
    roomUserList = findChild<QListWidget*>("roomUserList");
    roomEventLog = findChild<QPlainTextEdit*>("roomEventLog");

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

    connect(roomUserList, &QListWidget::itemDoubleClicked, this, &MainWindow::onRoomUserDoubleClicked, Qt::UniqueConnection);
    connect(roomUserList, &QListWidget::customContextMenuRequested, this, &MainWindow::onRoomListContextMenu, Qt::UniqueConnection);
}

void MainWindow::setupBottomMenus()
{
    auto *beautyBtn = findChild<QToolButton*>("beautyMenuButton");
    if (beautyBtn && !beautyBtn->menu()) {
        auto *beautyMenu = new QMenu(beautyBtn);
        auto *natural = beautyMenu->addAction("自然");
        natural->setEnabled(false);
        auto *clear = beautyMenu->addAction("清晰");
        clear->setEnabled(false);
        auto *soft = beautyMenu->addAction("柔和");
        soft->setEnabled(false);
        beautyMenu->addSeparator();
        auto *tone = beautyMenu->addAction("肤色调节（待实现）");
        tone->setEnabled(false);
        beautyBtn->setMenu(beautyMenu);
    }

    auto *moreBtn = findChild<QToolButton*>("moreMenuButton");
    if (moreBtn && !moreBtn->menu()) {
        auto *moreMenu = new QMenu(moreBtn);
        auto *share = moreMenu->addAction("屏幕共享（待实现）");
        share->setEnabled(false);
        auto *whiteboard = moreMenu->addAction("协作白板（待实现）");
        whiteboard->setEnabled(false);
        auto *chat = moreMenu->addAction("聊天面板（待实现）");
        chat->setEnabled(false);
        auto *participants = moreMenu->addAction("成员管理（待实现）");
        participants->setEnabled(false);
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
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
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
        QPixmap::fromImage(img).scaled(tile.videoLabel->size(), Qt::KeepAspectRatio, Qt::FastTransformation)
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

    focusStatusLabel->setText(flags.join("  |  "));
    focusStatusLabel->adjustSize();
    focusStatusLabel->move(10, 10);
    focusStatusLabel->show();
    focusStatusLabel->raise();
}

bool MainWindow::ensureRoomIdentity(bool askRoomIfEmpty)
{
    if (askRoomIfEmpty && roomId.isEmpty()) {
        bool ok = false;
        QString suggest = roomId;
        if (suggest.isEmpty()) {
            suggest = QString::number(QRandomGenerator::global()->bounded(100000, 999999));
        }
        const QString input = QInputDialog::getText(
            this, "输入房间号", "房间号：", QLineEdit::Normal, suggest, &ok
        ).trimmed();
        if (!ok || input.isEmpty()) return false;
        roomId = input;
    } else if (roomId.isEmpty()) {
        roomId = QString::number(QRandomGenerator::global()->bounded(100000, 999999));
    }
    if (userId.isEmpty()) {
        userId = QString("u%1").arg(QRandomGenerator::global()->bounded(1000, 9999));
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
    if (roomEventLog) {
        roomEventLog->appendPlainText(msg);
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

        auto *item = new QListWidgetItem(text, roomUserList);
        item->setData(Qt::UserRole, stream);
        item->setIcon(makeStateIcon(st));
        const QString roleText = st.host ? "主持人" : (st.cohost ? "联席主持人" : "成员");
        item->setToolTip(QString("角色: %1\n推流: %2\n麦克风: %3\n摄像头: %4")
                             .arg(roleText)
                             .arg(st.pub ? "开启" : "关闭")
                             .arg(st.audio ? "开启" : "关闭")
                             .arg(st.video ? "开启" : "关闭"));
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
    refreshRemoteTiles();
    syncGridPullers();
}

void MainWindow::sendSignalJoin()
{
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

void MainWindow::onSignalConnected()
{
    signalConnected = true;
    if (signalStateLabel) signalStateLabel->setText("信令: 已连接");
    if (auto *signalBadgeLabel = findChild<QLabel*>("signalBadgeLabel")) {
        signalBadgeLabel->setText("信令在线");
    }
    if (ui && ui->startReceiveButton) ui->startReceiveButton->setText("断开信令");
    sendSignalJoin();
    refreshSelfControlActions();
}

void MainWindow::onSignalDisconnected()
{
    signalConnected = false;
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
}

void MainWindow::onSignalTextMessage(const QString &msg)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
    const QJsonObject obj = doc.object();
    const QString type = obj.value("type").toString();

    if (type == "members") {
        const QString room = obj.value("room").toString();
        if (!roomId.isEmpty() && room != roomId) return;

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
            if (oldAudio != localAudioOn || oldVideo != localVideoOn) {
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

        if (selfIsHost) {
            menu.addSeparator();
            allMuteAudio = menu.addAction("全体静音（不含自己）");
            allUnmuteAudio = menu.addAction("全体恢复麦克风（不含自己）");
            menu.addSeparator();
            allMuteVideo = menu.addAction("全体关闭摄像头（不含自己）");
            allUnmuteVideo = menu.addAction("全体开启摄像头（不含自己）");
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
    }
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
    connect(sess->thread, &QThread::finished, sess->puller, &QObject::deleteLater, Qt::QueuedConnection);
    connect(sess->puller, &rtmppuller::videoFrameReady, this, [this, stream](const QImage &img) {
        applyTileFrame(stream, img);
        if (focusMode && focusedStream == stream && ui && ui->remoteVideolabel) {
            ui->remoteVideolabel->setPixmap(
                QPixmap::fromImage(img).scaled(ui->remoteVideolabel->size(), Qt::KeepAspectRatio, Qt::FastTransformation)
            );
        }
    });
    connect(sess->puller, &rtmppuller::errorOccurred, this, [this, stream](const QString &e) {
        PullSession *s = pullSessions.value(stream, nullptr);
        if (!s) return;
        appendRoomEvent(QString("拉流错误(%1): %2").arg(stream, e));
        if (meetingStopped || !signalConnected) return;
        const QStringList needed = currentDisplayStreams();
        if (!needed.contains(stream)) return;
        if (++s->retryCount > 5) {
            appendRoomEvent(QString("拉流重试超过上限: %1").arg(stream));
            return;
        }
        const int retryIndex = s->retryCount;
        QTimer::singleShot(300 * retryIndex, this, [this, stream]() {
            if (meetingStopped || !signalConnected) return;
            if (!pullSessions.contains(stream)) return;
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

    if (sess->puller) {
        disconnect(sess->puller, nullptr, this, nullptr);
        sess->puller->stop();
    }
    if (sess->thread) {
        sess->thread->quit();
        if (waitForQuit) {
            if (!sess->thread->wait(2500)) {
                qWarning() << "[RtmpPuller]" << stream << "stop wait timeout, detach";
                sess->thread->requestInterruption();
                sess->thread->setParent(nullptr);
                QObject::connect(sess->thread, &QThread::finished, sess->thread, &QObject::deleteLater, Qt::UniqueConnection);
            } else {
                delete sess->thread;
            }
        } else {
            if (!sess->thread->wait(200)) {
                sess->thread->requestInterruption();
                sess->thread->setParent(nullptr);
                QObject::connect(sess->thread, &QThread::finished, sess->thread, &QObject::deleteLater, Qt::UniqueConnection);
            } else {
                delete sess->thread;
            }
        }
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
