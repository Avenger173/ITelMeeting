#ifndef AUDIOCAPTURE_H
#define AUDIOCAPTURE_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QFile>
#include <atomic>
#include<QAudioSink>
extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
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
        void logMessage(const QString &msg);
        void audioFrameReady(QByteArray data);

public slots:
    void captureLoop();

private:
    std::atomic_bool running;
    QMutex mutex;
    QAudioSink *audioSink = nullptr;
    // QIODevice *audioIODevice = nullptr; // 移除


    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *codecCtx = nullptr;
    SwrContext *swrCtx = nullptr;
    int audioStreamIndex = -1;

    QFile outFile;
    bool enableSave = false;
    bool enablePlay = false;

    qint64 totalAudioBytes = 0; // 总共写入的音频字节数

};

#endif // AUDIOCAPTURE_H
