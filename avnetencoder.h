#ifndef AVNETENCODER_H
#define AVNETENCODER_H

#include <QObject>
#include<QImage>
#include<QByteArray>
#include<QElapsedTimer>
#include<atomic>

extern "C"{
#include<libavcodec/avcodec.h>
#include<libswscale/swscale.h>
#include<libavutil/imgutils.h>
#include<libavutil/opt.h>
}
class AvNetEncoder : public QObject
{
    Q_OBJECT
public:
    explicit AvNetEncoder(QObject *parent = nullptr);
    ~AvNetEncoder();

    bool openVideo(int w,int h,int fps,int bitrate = 2200000);  //初始化编码器
    void close();

public slots:
    void pushVideoFrame(const QImage &img);//接收RGB888的QImage
signals:
    //编码完成后触发的信号，对外输出编码后的视频数据包和时间戳
    void videoPacketReady(const QByteArray &pktData,quint32 pts_ms);
private:
    const AVCodec* vCodec_=nullptr;
    AVCodecContext* vCtx_=nullptr;
    SwsContext* sws_=nullptr;
    AVFrame* yuv_=nullptr;
    AVPacket* pkt_=nullptr;

    int w_=0,h_=0,fps_=30;
    int bitrate_=2200000;

    int64_t vFrameIndex_=0; //视频帧索引，用于计算时间戳
    QElapsedTimer vClock_;  //时间戳计算器
    bool vClockStarted_=false;
    qint64 lastPtsMs_=0;

    std::atomic_bool opened_{false};

    static void setX2640pts_(AVCodecContext* c,int fps);    //配置 x264 编码器的时间戳相关参数（x264 是主流的 H.264 编码器）
    //将 AVCC 格式的编码器配置数据转为 AnnexB 格式（H.264 码流的两种格式，AnnexB 更适合网络传输）。
    static QByteArray avccExtradataToAnnexB(const uint8_t* extra,int extraSize);
    void emitVideoConfigOnce(); //确保编码器配置信息（SPS/PPS）只输出一次，避免重复发送导致解码端异常
    bool configEmitted=false;
};

#endif // AVNETENCODER_H
