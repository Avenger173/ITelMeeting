#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include<QImage>
#include<QPixmap>
#include<QDateTime>
#include<QMessageBox>
#include<QDebug>
#include<QThread>
#include<thread>
#include<QProcess>
#include<QDataStream>
#include<QAudioFormat>
#include<QPainter>
#include<QApplication>
#include<QPointer>
#include<libswresample/swresample.h>
#include<QDesktopServices>
#include<QUrl>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    ,timer(new QTimer(this))
{
    ui->setupUi(this);
    // 临时测试菜单
    QMenu *debugMenu=menuBar()->addMenu(QStringLiteral("调试(临时)"));
    QAction *actStartEmpty=new QAction(QStringLiteral("开始空录制(仅写头)"),this);
    QAction *actStopEmpty=new QAction(QStringLiteral("结束空录制(写尾)"),this);
    debugMenu->addAction(actStartEmpty);
    debugMenu->addAction(actStopEmpty);
    connect(actStartEmpty,&QAction::triggered,this,&MainWindow::onDebugStartEmptyRecord);
    connect(actStopEmpty,&QAction::triggered,this,&MainWindow::onDebugStopEmptyRecord);
    QAction *actGenTest=new QAction(QStringLiteral("生成3秒测试视频"),this);
    debugMenu->addAction(actGenTest);
    connect(actGenTest,&QAction::triggered,this,&MainWindow::onDebugGen3sTestVideo);
    QAction *actStartCamRec=new QAction(QStringLiteral("开始摄像头录制(仅视频)"),this);
    QAction *actStopCamRec=new QAction(QStringLiteral("停止摄像头录制"),this);
    debugMenu->addAction(actStartCamRec);
    debugMenu->addAction(actStopCamRec);
    connect(actStartCamRec,&QAction::triggered,this,&MainWindow::onDebugStartCamRecord);
    connect(actStopCamRec,&QAction::triggered,this,&MainWindow::onDebugStopCamRecord);
    QAction *actStartEmptyAV=new QAction(QStringLiteral("开始空AV录制(仅写头)"),this);
    debugMenu->addAction(actStartEmptyAV);
    connect(actStartEmptyAV,&QAction::triggered,this,&MainWindow::onDebugStartEmptyAV);
    QAction *actStartAudioRec=new QAction(QStringLiteral("开始音频录制(仅音频)"),this);
    QAction *actStopAudioRec=new QAction(QStringLiteral("停止音频录制"),this);
    debugMenu->addAction(actStartAudioRec);
    debugMenu->addAction(actStopAudioRec);
    connect(actStartAudioRec,&QAction::triggered,this,&MainWindow::onDebugStartAudioRecord);
    connect(actStopAudioRec,&QAction::triggered,this,&MainWindow::onDebugStopAudioRecord);

    QAction *actStartAVRec=new QAction(QStringLiteral("开始AV录制(音视频)"),this);
    QAction *actStopAVRec=new QAction(QStringLiteral("停止AV录制"),this);
    debugMenu->addAction(actStartAVRec);
    debugMenu->addAction(actStopAVRec);
    connect(actStartAVRec,&QAction::triggered,this,&MainWindow::onDebugStartAVRecord);
    connect(actStopAVRec,&QAction::triggered,this,&MainWindow::onDebugStopAVRecord);

    setWindowTitle("SmartMeet视频会议系统");
    for (int i = 0; i < 5; ++i) {
        cv::VideoCapture temp(i);
        if (temp.isOpened()) {
            ui->cameraDevicecomboBox->addItem("摄像头" + QString::number(i), i);
            temp.release();
        }
    }
    // 构造函数中加入音频输入设备（使用 Qt 媒体设备描述）
    for (const auto &device : QMediaDevices::audioInputs()) {
        ui->audioDevicecomboBox->addItem(device.description());
    }
    // 确保 recorder 存在后再连接信号
        recorder=new AvRecorder(this);
    connect(recorder,&AvRecorder::videoPacketReady,this,[](const QByteArray &pkt,quint32 pts){
        qDebug()<<"[Recorder] 视频包"<<pkt.size()<<"pts"<<pts;
    });
    connect(recorder,&AvRecorder::audioPacketReady,this,[](const QByteArray &pkt,quint32 pts){
        qDebug()<<"[Recorder] 音频包"<<pkt.size()<<"pts"<<pts;
    });
}

MainWindow::~MainWindow()
{
    qDebug()<<"[MainWindow] 析构";
    on_stopMeetingButton_clicked();


    if (playSwrCtx) {
        swr_free(&playSwrCtx);
        playSwrCtx = nullptr;
    }
    if (videoWorker&&videoThread) {
        videoWorker->stop();
        videoThread->quit();
        videoThread->wait();
        delete videoWorker;
        delete videoThread;
        videoWorker = nullptr;
        videoThread=nullptr;
    }



    auto tmp=ui;
    ui=nullptr;
    delete tmp;
}

void MainWindow::on_startMeetingButton_clicked()
{
    meetingStopped=false;
    audioStopped=false;
    const bool useExternalWebRtcTest=false;
    if(videoWorker||videoThread){
        qInfo()<<"[Mainwindow] 会议已启动（重复启动忽略）";
        return;
    }
    if (!videoWorker) {
        videoWorker = new VideoCapture;
        videoThread = new QThread(this);
        videoWorker->moveToThread(videoThread);

        connect(videoThread, &QThread::started, videoWorker, &VideoCapture::captureLoop);

        QPointer<QLabel> localLabel=ui->localVideolabel;
        QPointer<MainWindow> self(this);
        disconnect(videoWorker,&VideoCapture::frameCaptured,this,nullptr);
        connect(videoWorker, &VideoCapture::frameCaptured, this, [self,localLabel](const QImage &img){
            if(!self) return;
            if(self->meetingStopped) return;
            if(!localLabel) return;
            // 预览到本地label
            localLabel->setPixmap(QPixmap::fromImage(img).scaled(
                localLabel->size(), Qt::KeepAspectRatio, Qt::FastTransformation));

            // if(camRecording&&recorder&&recorder->isOpen()){
            //     // 简单节流
            //     const qint64 nowMs=QDateTime::currentMSecsSinceEpoch();
            //     const qint64 interval=1000/qMax(1,recFps);
            //     if(nowMs-lastPushMs>=interval){
            //         recorder->pushVideoFrame(img);
            //         lastPushMs=nowMs;
            //     }
            // }
        },Qt::QueuedConnection);

        if (!videoWorker->open(0)) {
            QMessageBox::warning(this, "错误", "无法打开摄像头");
            // 清理:避免留下半初始化对象导致 stopMeeting 崩溃
            videoWorker->deleteLater();
            videoWorker=nullptr;
            videoThread->deleteLater();
            videoThread=nullptr;
            return;
        }

        videoThread->start(QThread::HighPriority);
    }
    // RTMP联调阶段先关闭UDP发送，避免额外线程/日志压力
    constexpr bool kEnableUdpSender=false;
    if(kEnableUdpSender){
        if(!sender) sender=new AVSender(this);
        if(sender->start("127.0.0.1",12345,12346)){
            qDebug()<<"[Mainwindow] 发送端已启动";
        }else{
            qWarning()<<"[Mainwindow] AVSender 已在运行";
        }
    }

    // 启动RTMP推流到ZLMediaKit
    // 创建编码/推流线程
    if(!encThread){
        encThread=new QThread(this);
        encThread->start(QThread::HighPriority);
    }
    // RTMP推流器放到pushThread
    if(!pushThread){
        pushThread=new QThread(this);
        pushThread->start(QThread::HighPriority);
    }
    if(!pusher) pusher=new RtmpPusher(nullptr);
    pusher->moveToThread(pushThread);
    connect(pushThread,&QThread::finished,pusher,&QObject::deleteLater);

    // 先启动音频
    //Audio RTMP
    if(!audioWorker){
        on_startAudioButton_clicked();// 复用已有音频采集
    }

    if(audioWorker){
        if(!audioEncThread){
            audioEncThread=new QThread(this);
            audioEncThread->start(QThread::HighPriority);
        }
        audioEnc=new AvAudioEncoder(nullptr);
        audioEnc->moveToThread(audioEncThread);
        connect(audioEncThread,&QThread::finished,audioEnc,&QObject::deleteLater);

        bool aok=false;
        QMetaObject::invokeMethod(audioEnc,[&](){
            aok=audioEnc->open(44100,1);
        },Qt::BlockingQueuedConnection);

        if(!aok){
            qWarning()<<"[Mainwindow] 音频编码器打开失败";
        }

        disconnect(audioWorker,&AudioCapture::audioFrameReady,audioEnc,nullptr);
        connect(audioWorker,&AudioCapture::audioFrameReady,audioEnc,&AvAudioEncoder::pushPcm,Qt::QueuedConnection);

        disconnect(audioEnc,&AvAudioEncoder::audioPacketReady,pusher,nullptr);
        connect(audioEnc,&AvAudioEncoder::audioPacketReady,pusher,&RtmpPusher::pushEncodeAudio,Qt::QueuedConnection);
    }

    bool rtmpOk=false;
    QMetaObject::invokeMethod(pusher,[&](){
        pusher->setVideoParams(640,480,30);
        pusher->setAudioParams(44100,1);
        rtmpOk = pusher->start("rtmp://127.0.0.1/live/test",30,44100);
    },Qt::BlockingQueuedConnection);

    if(!rtmpOk){
        qWarning()<<"[Mainwindow] RTMP推流启动失败";
    }else{
        qDebug()<<"[Mainwindow] RTMP推流已启动";
    }

    // 编码器放到视频线程
    if(!netEnc) netEnc=new AvNetEncoder(nullptr);
    netEnc->moveToThread(encThread);
    connect(encThread,&QThread::finished,netEnc,&QObject::deleteLater);

    // 先连接输出信号，避免配置包丢失
    // connect(netEnc,&AvNetEncoder::videoPacketReady,sender,&AVSender::sendEncodedVideo,Qt::QueuedConnection);
    connect(netEnc,&AvNetEncoder::videoPacketReady,pusher,&RtmpPusher::pushEncodeVideo,Qt::QueuedConnection);

    bool encOk=false;
    QMetaObject::invokeMethod(netEnc,[&](){
        encOk = netEnc->openVideo(640,480,30);
    },Qt::BlockingQueuedConnection);

    if(!encOk){
        qWarning()<<"[Mainwindow] netEnc openVideo 失败";
        return;
    }

    // netEnc 在 encThread，必须 QueuedConnection
    connect(videoWorker,&VideoCapture::frameCaptured,netEnc,&AvNetEncoder::pushVideoFrame,Qt::QueuedConnection);


    // // 连接录像模块 -> 推流模块
    // connect(recorder,&AvRecorder::videoPacketReady,sender,&AVSender::sendEncodedVideo,Qt::QueuedConnection);
    // connect(recorder,&AvRecorder::audioPacketReady,sender,&AVSender::sendEncodedAudio,Qt::QueuedConnection);

    // // 连接编码后数据 -> 推流器
    // connect(recorder,&AvRecorder::videoPacketReady,pusher,&RtmpPusher::pushEncodeVideo,Qt::QueuedConnection);
    // connect(recorder,&AvRecorder::audioPacketReady,pusher,&RtmpPusher::pushEncodeAudio,Qt::QueuedConnection);
}

void MainWindow::on_stopMeetingButton_clicked()
{
    if(meetingStopped){
        qDebug()<<"[Mainwindow] 会议已停止（重复调用忽略）";
    }
    meetingStopped=true;

    // 断开所有外部对象
    if(receiver) disconnect(receiver,nullptr,this,nullptr);
    // 1) 先停止音频采集（若正在采集）
    on_stopAudioButton_clicked();
    // 停止接收端(UDP收流+播放)
    if(receiver){
        disconnect(receiver,nullptr,this,nullptr);;// 先断开，防止继续触发UI更新
        receiver->stop();
        delete receiver;
        receiver=nullptr;
    }
    // 3) 停止发送端
    if(sender){
        sender->stop();
        delete sender;
        sender=nullptr;
    }
    // 停止RTMP推流
    if(pusher){
        QMetaObject::invokeMethod(pusher,[&](){
            pusher->stop();
        },Qt::BlockingQueuedConnection);
        pusher->deleteLater();
        pusher=nullptr;
    }
    if(pushThread){
        pushThread->quit();
        pushThread->wait();
        delete pushThread;
        pushThread=nullptr;
    }
    if(audioEnc){
        QMetaObject::invokeMethod(audioEnc,[&](){
            audioEnc->close();
        },Qt::BlockingQueuedConnection);
        audioEnc=nullptr;
    }
    if(audioEncThread){
        audioEncThread->quit();
        audioEncThread->wait();
        delete audioEncThread;
        audioEncThread=nullptr;
    }
    // 编码器
    if(netEnc){
        QMetaObject::invokeMethod(netEnc,[&](){
            netEnc->close();
        },Qt::BlockingQueuedConnection);
        netEnc=nullptr;
    }

    if(encThread){
        encThread->quit();
        encThread->wait();
        delete encThread;
        encThread=nullptr;
    }
    // 视频线程对象
    if(videoWorker){
        // 请求采集线程结束循环
        videoWorker->stop();
    }
    if(videoThread){
            videoThread->quit();
            videoThread->wait();
    }
    if(videoWorker){
        delete videoWorker;
        videoWorker=nullptr;
    }
    if(videoThread){
        delete videoThread;
        videoThread=nullptr;
    }

    if(puller&&pullThead){
        QMetaObject::invokeMethod(puller,"stop",Qt::BlockingQueuedConnection);
        pullThead->quit();
        pullThead->wait();
        pullThead->deleteLater();
        pullThead=nullptr;
        puller=nullptr;
    }


    qDebug()<<"[Mainwindow] 会议已结束";
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
        audioWorker->moveToThread(audioThread);

        connect(audioThread, &QThread::started, audioWorker, &AudioCapture::captureLoop);
        connect(audioWorker, &AudioCapture::logMessage, this, [](const QString &msg){
            qDebug() << "AudioLog:" << msg;
        });

        QString device = "audio=" + ui->audioDevicecomboBox->currentText();
        qDebug() << "FFmpeg 采集设备名:" << device;
        bool save = ui->enableAudioSavecheckBox->isChecked();
        bool play = false;
        audioPlayEnabled=false;

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

    QPointer<MainWindow> self(this);
    connect(audioWorker, &AudioCapture::audioFrameReady, this,
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
        if(audioThread){
            audioThread->quit();
            audioThread->wait();
        }
        disconnect(audioWorker, nullptr, this, nullptr); // 断开所有信号
        delete audioWorker;
        audioWorker = nullptr;
    }
    if(audioThread){
        delete audioThread;
        audioThread=nullptr;
    }
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

}

void MainWindow::on_stopRecordButton_clicked()
{

}

void MainWindow::onDebugStartEmptyRecord()
{
    if(recorder&&recorder->isOpen()){
        QMessageBox::information(this,"提示","录制器已打开");
        return;
    }
    if(!recorder) recorder=new AvRecorder(this);

    const QString filename="test_empty.mp4";
    const int w=640,h=480,fps=30;

    if(!recorder->open(filename,w,h,fps)){
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
            for (int y = 0; y < H; y += 60) p.drawLine(0, y, W, y);\

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
    // 临时: 外部浏览器验证WebRTC(不嵌入界面)
    const bool useExternalWebRTcTest=false;
    if(puller) return;// 防止重复启动

    puller=new rtmppuller(nullptr);
    pullThead=new QThread(this);
    puller->moveToThread(pullThead);

    connect(pullThead,&QThread::finished,puller,&QObject::deleteLater);
    connect(puller,&rtmppuller::videoFrameReady,this,[this](const QImage& img){
        ui->remoteVideolabel->setPixmap(
            QPixmap::fromImage(img).scaled(ui->remoteVideolabel->size(),Qt::KeepAspectRatio,Qt::FastTransformation));
    });

    // 音频播放已在 rtmppuller 线程内部处理
    connect(puller,&rtmppuller::errorOccurred,this,[](const QString& e){
        qWarning()<<e;
    });

    pullThead->start(QThread::HighPriority);

    const QString url="rtmp://127.0.0.1/live/test";
    QMetaObject::invokeMethod(puller,"startPull",Qt::QueuedConnection,Q_ARG(QString,url));

    qInfo()<<"[Mainwindow] 接收端已启动(ZLMediakit Pull)";
}




