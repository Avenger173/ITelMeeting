#ifndef RTMPPULLER_H
#define RTMPPULLER_H

#include <QObject>
#include <QImage>
#include <QAtomicInteger>
#include <QByteArray>
#include <QAudioSink>
#include <QAudioFormat>
#include <QIODevice>
#include <QtGlobal>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}

class rtmppuller : public QObject
{
    Q_OBJECT
public:
    explicit rtmppuller(QObject *parent = nullptr);
    ~rtmppuller();

public slots:
    void startPull(const QString &url);
    void stop();
    void setAudioEnabled(bool enabled);

signals:
    void videoFrameReady(const QImage &img);
    void audioPcmReady(const QByteArray &pcm, int sampleRate, int channels);
    void errorOccurred(const QString &err);
    void finished();

private:
    static int interruptCb(void *opaque);

    QAtomicInteger<bool> stopFlag{false};
    QAtomicInteger<bool> audioEnabled_{true};

    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *vDecCtx = nullptr;
    SwsContext *sws = nullptr;
    AVFrame *frame = nullptr;
    AVFrame *rgbFrame = nullptr;
    AVPacket *pkt = nullptr;
    uint8_t *rgbBuf = nullptr;
    int vStream = -1;

    void cleanup();

    AVCodecContext *aDecCtx = nullptr;
    SwrContext *aswr = nullptr;
    SwrContext *recSwr_ = nullptr;
    AVFrame *aFrame = nullptr;
    int aStream = -1;

    QAudioSink *audioSink_ = nullptr;
    QIODevice *audioOut_ = nullptr;
    QAudioFormat playFmt_;
    QByteArray audioPending_;

    void flushAudioPending_();
    qint64 currentAudioBufferedMs_() const;
    void paceVideoAgainstAudio_();
};

#endif // RTMPPULLER_H
