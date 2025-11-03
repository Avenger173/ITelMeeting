#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include<QTimer>
#include<opencv2/opencv.hpp>
#include<QFile>
#include<QAudioFormat>
#include<QIODevice>
#include<QAudioDevice>
#include<QMediaDevices>
#include<QAudioSink>
#include<QMutex>
#include<atomic>
#include<QCheckBox>
#include<QCloseEvent>
#include<QElapsedTimer>
#include"videocapture.h"
#include"audiocapture.h"
#include"avrecorder.h"
#include"avsender.h"
#include"avreceiver.h"
extern "C"{
#include<libavcodec/avcodec.h>
#include<libavformat/avformat.h>
#include<libavutil/opt.h>
#include<libavdevice/avdevice.h>
#include<libswscale/swscale.h>
#include<libswresample/swresample.h>
}
QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();


private slots:

    void on_startMeetingButton_clicked();
    void on_switchCameraButton_clicked();
    void on_captureImageButton_clicked();
    void on_startAudioButton_clicked();
    void on_stopAudioButton_clicked();
    void on_startRecordButton_clicked();
    void on_stopRecordButton_clicked();

    void onDebugStartEmptyRecord();//只open写头
    void onDebugStopEmptyRecord();//只close写尾
    void onDebugGen3sTestVideo();//生成3秒，30fps,640x480的测试视频(彩条+时间戳)
    void onDebugStartCamRecord();//开始：仅视频
    void onDebugStopCamRecord();//停止：写尾并关闭
    void onDebugStartEmptyAV();//openAV(带音频）+立即close();
    void onDebugStartAudioRecord();//仅音频
    void onDebugStopAudioRecord();

    void onDebugStartAVRecord();//开始音视频同步录制
    void onDebugStopAVRecord();//停止音视频录制

    void on_startReceiveButton_clicked();

    void on_stopMeetingButton_clicked();

private:
    Ui::MainWindow *ui;
    QTimer* timer = nullptr;
    QThread *videoThread = nullptr;
    VideoCapture *videoWorker = nullptr;
    QThread *audioThread = nullptr;
    AudioCapture *audioWorker = nullptr;
    QAudioSink* audioSink = nullptr;
    QIODevice* audioOutput = nullptr;
    SwrContext* playSwrCtx = nullptr; // 播放端重采样上下文
    QAudioFormat playFormat;           // 播放端格式
    QMetaObject::Connection recordConn;
    bool isRecording=false;

    AvRecorder *recorder=nullptr;
    bool camRecording=false;//是否正在"摄像头->本地视频"录制
    int recFps=30;//目标录制FPS
    qint64 lastPushMs=0;//上一次写入的时间戳(ms)

    //发送
    AVSender *sender=nullptr;
    //接收
    AVReceiver *receiver=nullptr;
    QAudioSink *recvAudioSink=nullptr;
    QIODevice *recvAudioOutput=nullptr;
    SwrContext *recvPlaySwrCtx=nullptr;
    QAudioFormat recvPlayFormat;

};
#endif // MAINWINDOW_H
