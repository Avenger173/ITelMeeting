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

    bool openVideo(int w,int h,int fps,int bitrate = 2200000);
    void close();

public slots:
    void pushVideoFrame(const QImage &img);//输入RGB888的QImage
signals:
    void videoPacketReady(const QByteArray &pktData,quint32 pts_ms);
private:
    const AVCodec* vCodec_=nullptr;
    AVCodecContext* vCtx_=nullptr;
    SwsContext* sws_=nullptr;
    AVFrame* yuv_=nullptr;
    AVPacket* pkt_=nullptr;

    int w_=0,h_=0,fps_=30;
    int bitrate_=2200000;
    int64_t vFrameIndex_=0;
    QElapsedTimer vClock_;
    bool vClockStarted_=false;
    qint64 lastPtsMs_=0;
    std::atomic_bool opened_{false};

    static void setX2640pts_(AVCodecContext* c,int fps);

    static QByteArray avccExtradataToAnnexB(const uint8_t* extra,int extraSize);
    void emitVideoConfigOnce();
    bool configEmitted=false;
};

#endif // AVNETENCODER_H
