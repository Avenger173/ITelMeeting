#ifndef AUDIOCAPTURE_H
#define AUDIOCAPTURE_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QFile>
#include <atomic>
#include<QAudioSink>    //音频输出
#include<QElapsedTimer> //高精度计时
extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>  //参数选项处理
#include <libswresample/swresample.h>   //音频重采样
#include<libavcodec/avcodec.h>
}

class AudioCapture : public QObject
{
    Q_OBJECT
public:
    explicit AudioCapture(QObject *parent = nullptr);
    ~AudioCapture();
    bool startCapture(const QString &deviceName, bool saveAudio, bool playAudio, QAudioFormat outputFormat);
    void stop(); // 只设置 running=false
    void cleanup(); // 采集线程退出后手动调用
signals:
        void logMessage(const QString &msg);    //日志信号
        void audioFrameReady(QByteArray data);  //传递解码 + 重采样后的音频数据块（16 位单声道 44100Hz），供播放 / 处理逻辑使用

public slots:
    void captureLoop();

private:
    std::atomic_bool running;
    QMutex mutex;
    QAudioSink *audioSink = nullptr;    //Qt音频输出对象（播放音频）


    AVFormatContext *fmtCtx = nullptr;  // 格式上下文（管理音频流）
    AVCodecContext *codecCtx = nullptr; // 编解码上下文（音频解码）
    SwrContext *swrCtx = nullptr;   // 重采样上下文（音频格式转换，如采样率/声道数转换）
    int audioStreamIndex = -1;  // 音频流索引（标记哪个流是音频）
    // 音频保存相关
    QFile outFile;
    bool enableSave = false;
    bool enablePlay = false;

    qint64 totalAudioBytes = 0; // 总共写入的音频字节数
    static int interruptCallback(void *opaque); // FFmpeg中断回调（用于停止采集）

};

#endif // AUDIOCAPTURE_H
