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
#include <algorithm>
#include <libswresample/swresample.h>

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

    if (playSwrCtx) {
        swr_free(&playSwrCtx);
        playSwrCtx = nullptr;
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

        if (!videoWorker->open(0)) {
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

    if (!audioWorker) {
        on_startAudioButton_clicked();
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

    on_stopAudioButton_clicked();
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

void MainWindow::on_switchCameraButton_clicked()
{
    if (!videoWorker) return;

    int index = ui->cameraDevicecomboBox->currentIndex();
    if (!videoWorker->reopen(index)) {
        QMessageBox::warning(this, "错误", "切换摄像头失败");
    }
}

void MainWindow::on_captureImageButton_clicked()
{
    if (videoWorker) {
        QString filename = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss") + ".jpg";
        videoWorker->capturePhoto(filename);
        QMessageBox::information(this, "提示", "已保存照片：" + filename);
    }
}



void MainWindow::on_startAudioButton_clicked()
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

        QString device = "audio=" + ui->audioDevicecomboBox->currentText();
        qDebug() << "FFmpeg 采集设备名:" << device;
        bool save = ui->enableAudioSavecheckBox->isChecked();
        bool play = ui->enableAudioPlaycheckBox->isChecked();
        audioPlayEnabled = play;

        // 采集端参数固定: 44100 Hz / 单声道 / Int16
        QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        QAudioFormat fmt;
        fmt.setSampleRate(44100);
        fmt.setChannelCount(1);
        fmt.setSampleFormat(QAudioFormat::Int16);
        if (!dev.isFormatSupported(fmt)) {
            qDebug() << "播放设备不支持 44100 单声道 Int16，使用推荐格式";
            fmt = dev.preferredFormat();
        }

        if (!audioWorker->startCapture(device, save, play, fmt)) {
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
                if(self->audioPlayEnabled){
                    if (!self->audioSink) {
                        QAudioDevice dev = QMediaDevices::defaultAudioOutput();
                        self->playFormat = dev.preferredFormat();
                        self->audioSink = new QAudioSink(dev, self->playFormat, self.data());
                        self->audioOutput = self->audioSink->start();

                        // 初始化 swrCtx (FFmpeg 7.x API)
                        if (self->playSwrCtx) {
                            swr_free(&self->playSwrCtx);
                        }
                        AVSampleFormat outFmt = AV_SAMPLE_FMT_S16;
                        if (self->playFormat.sampleFormat() == QAudioFormat::Int16) outFmt = AV_SAMPLE_FMT_S16;
                        else if (self->playFormat.sampleFormat() == QAudioFormat::Float) outFmt = AV_SAMPLE_FMT_FLT;
                        else if (self->playFormat.sampleFormat() == QAudioFormat::UInt8) outFmt = AV_SAMPLE_FMT_U8;

                        AVChannelLayout outLayout, inLayout;
                        av_channel_layout_default(&outLayout, self->playFormat.channelCount());
                        av_channel_layout_default(&inLayout, 1); // 采集端单声道

                        self->playSwrCtx = swr_alloc();
                        swr_alloc_set_opts2(
                            &self->playSwrCtx,
                            &outLayout,
                            outFmt,
                            self->playFormat.sampleRate(),
                            &inLayout,
                            AV_SAMPLE_FMT_S16,
                            44100,
                            0, nullptr
                            );
                        swr_init(self->playSwrCtx);

                        // 释放临时 layout
                        av_channel_layout_uninit(&outLayout);
                        av_channel_layout_uninit(&inLayout);
                    }
                    // 只对播放做格式转换，采集和保存流程不受影响
                    if (self->audioOutput && self->playSwrCtx) {
                        // 输入参数
                        const uint8_t* inData[1] = { reinterpret_cast<const uint8_t*>(data.constData()) };
                        int inSamples = data.size() / 2; // Int16 单声道

                        // 计算输出缓冲区大小
                        int outSamples = av_rescale_rnd(
                            swr_get_delay(self->playSwrCtx, 44100) + inSamples,
                            self->playFormat.sampleRate(),
                            44100,
                            AV_ROUND_UP
                            );
                        int outChannels = self->playFormat.channelCount();
                        int outBytesPerSample = self->playFormat.sampleFormat() == QAudioFormat::Int16 ? 2 :
                                                    self->playFormat.sampleFormat() == QAudioFormat::Float ? 4 :
                                                    self->playFormat.sampleFormat() == QAudioFormat::UInt8 ? 1 : 2;
                        int outBufSize = outSamples * outChannels * outBytesPerSample;
                        QByteArray outBuf(outBufSize, 0);
                        uint8_t* outData[2] = { reinterpret_cast<uint8_t*>(outBuf.data()), nullptr };

                        // 转换
                        int converted = swr_convert(
                            self->playSwrCtx,
                            outData, outSamples,
                            inData, inSamples
                            );
                        if (converted > 0) {
                            int bytesWritten = converted * outChannels * outBytesPerSample;
                            self->audioOutput->write(outBuf.constData(), bytesWritten);
                        }
                    }
                }

            },
            Qt::QueuedConnection
            );
}

void MainWindow::on_stopAudioButton_clicked()
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

    if (audioSink) {
        audioSink->stop();
        delete audioSink;
        audioSink = nullptr;
        audioOutput = nullptr;
    }
    if (playSwrCtx) {
        swr_free(&playSwrCtx);
        playSwrCtx = nullptr;
    }

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

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
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

    remoteContainer = new QWidget(ui->centralwidget);
    remoteContainer->setObjectName("remoteContainer");
    remoteContainer->setGeometry(ui->remoteVideolabel->geometry());

    remoteStack = new QStackedLayout(remoteContainer);
    remoteStack->setContentsMargins(0, 0, 0, 0);
    remoteStack->setSpacing(0);

    remoteGridPage = new QWidget(remoteContainer);
    auto *grid = new QGridLayout(remoteGridPage);
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(6);

    remoteTiles.clear();
    remoteTiles.reserve(4);

    for (int i = 0; i < 4; ++i) {
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

        tile.nameLabel = new QLabel("", tile.frame);
        tile.nameLabel->setStyleSheet("QLabel{color:#f0f0f0;font-weight:600;}");
        tile.stateLabel = new QLabel("", tile.frame);
        tile.stateLabel->setStyleSheet("QLabel{color:#a0a0a0;}");

        vbox->addWidget(tile.videoLabel, 1);
        vbox->addWidget(tile.nameLabel);
        vbox->addWidget(tile.stateLabel);
        grid->addWidget(tile.frame, i / 2, i % 2);

        remoteTiles.push_back(tile);
    }

    ui->remoteVideolabel->setParent(remoteContainer);
    ui->remoteVideolabel->setMinimumSize(120, 80);
    ui->remoteVideolabel->setAlignment(Qt::AlignCenter);
    ui->remoteVideolabel->setStyleSheet("QLabel{background:black;color:#d0d0d0;}");

    remoteStack->addWidget(remoteGridPage);
    remoteStack->addWidget(ui->remoteVideolabel);
    remoteStack->setCurrentWidget(remoteGridPage);
    remoteContainer->show();
}

void MainWindow::refreshRemoteTiles()
{
    if (remoteTiles.isEmpty()) return;

    QStringList streams = memberStates.keys();
    std::sort(streams.begin(), streams.end(), [this](const QString &a, const QString &b) {
        const bool ah = memberStates.value(a).host;
        const bool bh = memberStates.value(b).host;
        if (ah != bh) return ah > bh;
        return a < b;
    });

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
            tile.frame->setStyleSheet("QFrame{background:#101010;border:1px solid #4a4a4a;border-radius:6px;}");
            continue;
        }

        const QString stream = streams[i];
        const MemberState st = memberStates.value(stream);
        tile.stream = stream;
        streamToTile.insert(stream, i);

        QString name = stream;
        if (st.host) name += " [主持人]";
        tile.nameLabel->setText(name);

        QStringList flags;
        if (!st.pub) flags << "未推流";
        if (!st.audio) flags << "麦关";
        if (!st.video) flags << "摄关";
        tile.stateLabel->setText(flags.join(" | "));

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
    }
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

    QString selectedStream;
    if (auto *cur = roomUserList->currentItem()) {
        selectedStream = cur->data(Qt::UserRole).toString();
    }

    QStringList streams = memberStates.keys();
    std::sort(streams.begin(), streams.end(), [this](const QString &a, const QString &b) {
        const bool ah = memberStates.value(a).host;
        const bool bh = memberStates.value(b).host;
        if (ah != bh) return ah > bh;
        return a < b;
    });

    roomUserList->blockSignals(true);
    roomUserList->clear();
    for (const QString &stream : streams) {
        const MemberState st = memberStates.value(stream);
        QString text = stream;
        if (st.host) text += " [主持人]";
        if (!st.pub) text += " [未推流]";
        if (!st.audio) text += " [麦关]";
        if (!st.video) text += " [摄关]";

        auto *item = new QListWidgetItem(text, roomUserList);
        item->setData(Qt::UserRole, stream);
        if (st.host) {
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
}

void MainWindow::onSignalConnected()
{
    signalConnected = true;
    if (signalStateLabel) signalStateLabel->setText("信令: 已连接");
    if (ui && ui->startReceiveButton) ui->startReceiveButton->setText("断开信令");
    sendSignalJoin();
}

void MainWindow::onSignalDisconnected()
{
    signalConnected = false;
    if (signalStateLabel) signalStateLabel->setText("信令: 未连接");
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
            st.host = (m.value("role").toString() == "host");
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
            localAudioOn = selfState.audio;
            localVideoOn = selfState.video;
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
            return;
        }
        if (action == "mute_video") {
            localVideoOn = false;
            appendRoomEvent("主持人已关闭你的摄像头");
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
    if (targetStream.isEmpty() || targetStream == selfStream) return;
    if (!memberStates.value(selfStream).host) {
        appendRoomEvent("仅主持人可管理成员");
        return;
    }

    QMenu menu(this);
    QAction *muteAudio = menu.addAction("静音该成员");
    QAction *muteVideo = menu.addAction("关闭该成员摄像头");
    QAction *kick = menu.addAction("踢出该成员");
    QAction *picked = menu.exec(roomUserList->viewport()->mapToGlobal(pos));
    if (!picked) return;

    if (picked == muteAudio) {
        sendSignalCmd(targetStream, "mute_audio");
        appendRoomEvent(QString("已对 %1 发送静音").arg(targetStream));
    } else if (picked == muteVideo) {
        sendSignalCmd(targetStream, "mute_video");
        appendRoomEvent(QString("已对 %1 发送关闭摄像头").arg(targetStream));
    } else if (picked == kick) {
        sendSignalCmd(targetStream, "kick");
        appendRoomEvent(QString("已踢出 %1").arg(targetStream));
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
