#ifndef RTMPPUSHER_H
#define RTMPPUSHER_H

#include <QObject>
#include<QMutex>
#include<QByteArray>
#include<QString>

extern "C"{
#include<libavcodec/avcodec.h>
#include<libavformat/avformat.h>
#include<libavutil/opt.h>
#include<libavdevice/avdevice.h>
#include<libswscale/swscale.h>
#include<libswresample/swresample.h>
#include<libavutil/channel_layout.h>
}
class RtmpPusher : public QObject
{
    Q_OBJECT
public:
    explicit RtmpPusher(QObject *parent = nullptr);
    ~RtmpPusher();
    //启动RTMP推流
    bool start(const QString& rtmpUrl,int fps,int sampleRate);
    //停止推流
    void stop();
    void setVideoParams(int w,int h,int fps){vW=w;vH=h;vFps=fps;}
    void setAudioParams(int sr,int ch){aRate=sr;aCh=ch;}
public slots:
    //接AvRecorder的信号
    void pushEncodeVideo(const QByteArray& pktData,quint32 pts_ms);
    void pushEncodeAudio(const QByteArray& pktData,quint32 pts_ms);
private:
    bool opened_=false;
    QString url_;
    //复用器上下文(flv+rtmp)
    AVFormatContext* fmtCtx_=nullptr;
    //输出流
    AVStream* vStream_=nullptr;
    AVStream* aStream_=nullptr;
    //将已编码的ES写进复用器
    //但为了PTS/DTS正常，需要知道各种的time_base
    AVRational vTimeBase_{1,1000};//以毫秒喂进来
    AVRational aTimeBase_{1,1000};
    //为了首帧时间对齐
    bool vFirst_=true;
    bool aFirst_=true;
    int64_t vStart_=0;
    int64_t aStart_=0;
    //工具
    bool writeHeader_(int fps,int sampleRate);
    void writeInterleaved_(AVPacket* pkt,AVStream* st,AVRational inTb);

    QByteArray vExtra_;
    QByteArray aExtra_;
    bool haveVConf=false;
    bool haveAConf=false;

    int vW=640,vH=480,vFps=30;
    int aRate=44100,aCh=1;
    //H264 NAL提取工具
    static void parseH264AnnexBForSpsPps(const uint8_t* data,int size,QByteArray& sps,QByteArray& pps,bool& isKeyFrame);
    static QByteArray makeAvcCFromSpsPps(const QByteArray& sps,const QByteArray& pps);

    //AAC ASC生成(LC,最常用)
    static inline uint8_t srIndexFromRate(int sr);
    static QByteArray makeAacAsc(int sampleRate,int channles);
    //延迟写Header:当haveVConf&&haveAConf都为真时才写
    bool ensureHeaderWritten();

    //禁用拷贝
    Q_DISABLE_COPY(RtmpPusher)
signals:
};

#endif // RTMPPUSHER_H
