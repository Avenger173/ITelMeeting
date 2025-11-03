#ifndef AVRECEIVER_H
#define AVRECEIVER_H

#include <QObject>
#include<QUdpSocket>
#include<QThread>
#include<QImage>
#include<QAudioFormat>
#include<QAudioSink>
#include<QIODevice>
#include<QMutex>
#include<queue>
#include<QHash>
#include<bitset>
#include<functional>
#include<concurrent_priority_queue.h>
extern "C"{
#include<libavcodec/avcodec.h>
#include<libavformat/avformat.h>
#include<libswscale/swscale.h>
#include<libswresample/swresample.h>
#include<libavutil/channel_layout.h>
#include<libavutil/opt.h>
#include<libavdevice/avdevice.h>
}

struct FrameBuffer
{
    quint32 frameId=0;
    quint32 pts_ms=0;
    quint32 totalSize=0;
    quint16 chunkCount=0;
    int receivedCount=0;
    QByteArray buffer;
    QVector<char> receivedMap;
    qint64 lastUpdateMs=0;
};
class AVReceiver : public QObject
{
    Q_OBJECT
public:
    explicit AVReceiver(QObject *parent = nullptr);
    ~AVReceiver();

    bool start(const QString &host,int videoPort,int audioPort);//连接发送端
    void stop();//停止接收
signals:
    void newVideoFrame(const QImage &img);//解码后的视频帧
    void newAudioPCM(const QByteArray &pcm);//解码后的PCM
    void logMsg(const QString &msg);
private slots:
    void onVideoReadyRead();
    void onAudioReadyRead();
private:
    //网络
    QUdpSocket *videoSocket=nullptr;
    QUdpSocket *audioSocket=nullptr;
    //ffmpeg
    AVCodecContext *vCodecCtx=nullptr;
    AVCodecContext *aCodecCtx=nullptr;
    SwsContext *sws=nullptr;
    SwrContext *swr=nullptr;
    //播放
    QAudioSink *audioSink=nullptr;
    QIODevice *audioOut=nullptr;
    QAudioFormat audioFmt;
    //缓冲队列(AV同步)
    struct FrameItem{
        qint64 pts;
        QImage img;//视频
        QByteArray pcm;//音频
        bool isAudio;
    };
    std::priority_queue<
        FrameItem,
        std::vector<FrameItem>,
        std::function<bool(const FrameItem&,const FrameItem&)>>avQueue;

    QMutex queueMutex;
    qint64 startTimeMs=0;

    QHash<quint32,FrameBuffer> recvBuffers;
    QMutex recvBuffersMutex;

    static constexpr int HEADER_SIZE=16;
    static constexpr int CHUNK_SIZE=1200;
    //辅助函数
    void initCodecs();
    void decodePacket(const QByteArray &raw,bool isVideo);
    void handleVideoFrame(AVFrame *frame);
    void handleAudioFrame(AVFrame *frame);
    void playQueuedFrames();

    void cleanupStaleBuffers();
    static QString avErr(int errnum);
};

#endif // AVRECEIVER_H
