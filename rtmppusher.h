#ifndef RTMPPUSHER_H
#define RTMPPUSHER_H

#include <QObject>
#include<QMutex>
#include<QByteArray>
#include<QString>
#include<QMutex>
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
    void setVideoParams(int w,int h,int fps){vW=w;vH=h;vFps=fps;}   //设置视频参数：宽、高、帧率
    void setAudioParams(int sr,int ch){aRate=sr;aCh=ch;}    //设置音频参数：采样率、声道数
public slots:
    //接AvRecorder的信号
    //接收外部编码好的音视频数据（如 H.264 视频、AAC 音频）并推送
    void pushEncodeVideo(const QByteArray& pktData,quint32 pts_ms);
    void pushEncodeAudio(const QByteArray& pktData,quint32 pts_ms);
private:
    bool opened_=false;
    bool headerWritten_=false;
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

    bool basePtsInited=false;
    quint32 basePtsMs=0;
    quint32 lastVideoMs=0;
    quint32 lastAudioMs=0;
    //工具
    //写入 FLV/RTMP 头信息（包含音视频编码参数）
    bool writeHeader_(int fps,int sampleRate);
    //将编码后的 AVPacket 写入复用器，并处理时间戳转换（适配 RTMP 时间基）
    void writeInterleaved_(AVPacket* pkt,AVStream* st,AVRational inTb);

    QByteArray vExtra_;
    QByteArray aExtra_;
    //是否已获取音视频的配置信息，需等配置信息齐全才写 FLV 头
    bool haveVConf=false;
    bool haveAConf=false;

    int vW=640,vH=480,vFps=30;
    int aRate=44100,aCh=1;
    //H264 NAL提取工具
    //解析 H.264 的 AnnexB 格式数据，提取 SPS/PPS（配置信息）和判断是否是关键帧
    static void parseH264AnnexBForSpsPps(const uint8_t* data,int size,QByteArray& sps,QByteArray& pps,bool& isKeyFrame);
    //将 SPS/PPS 转换为 RTMP 要求的 AVC-C 格式（RTMP 推流 H.264 必须用此格式）
    static QByteArray makeAvcCFromSpsPps(const QByteArray& sps,const QByteArray& pps);

    //AAC ASC生成(LC,最常用)
    static inline uint8_t srIndexFromRate(int sr);
    //生成 AAC 的音频特定配置（ASC），FLV 封装 AAC 需先写 ASC 配置信息
    static QByteArray makeAacAsc(int sampleRate,int channles);
    //延迟写Header:当haveVConf&&haveAConf都为真时才写,只有音视频配置信息都齐全时才写头，保证推流兼容性
    bool ensureHeaderWritten();

    //禁用拷贝
    Q_DISABLE_COPY(RtmpPusher)
    QMutex writeMtx_;
signals:
    void writeError(const QString &err, bool videoPacket);
};

#endif // RTMPPUSHER_H
