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
#include<QHostAddress>
extern "C"{
#include<libavcodec/avcodec.h>
#include<libavformat/avformat.h>
#include<libswscale/swscale.h>
#include<libswresample/swresample.h>
#include<libavutil/channel_layout.h>
#include<libavutil/opt.h>
#include<libavdevice/avdevice.h>
#include<libavutil/imgutils.h>
}


class AVReceiver : public QObject
{
    Q_OBJECT
public:
    explicit AVReceiver(QObject *parent = nullptr);
    ~AVReceiver();

    bool start(const QString &host,int videoPort,int audioPort);//连接发送端
    void stop();//停止接收
signals:
    //解码好的远端视频帧
    void newVideoFrame(const QImage &img);
    //简单日志信号，方便接到Mainwindow打印
    void logMsg(const QString &msg);
private slots:
    void onVideoReadyRead();
    void onAudioReadyRead();
private:
    //网络
    QUdpSocket *videoSocket=nullptr;
    QUdpSocket *audioSocket=nullptr;
    QHostAddress m_destAddr;
    int m_videoPort=0,m_audioPort=0;

    //视频解码相关
    AVCodecContext *vDecCtx=nullptr;
    AVFrame *vFrame=nullptr;
    AVPacket *pkt=nullptr;
    SwsContext *sws=nullptr;
    //保存解析到的SPS/PPS，用于给解码器初始化
    QByteArray sps_;
    QByteArray pps_;
    bool haveVConf=false;

    //处理收到的一个完整UDP包（单包=一帧）
    void processVideoPacket(const QByteArray &data);
    void processAudioPacket(const QByteArray &data);

    bool ensureVideoDecoder();
    void freeDecoders();

    //H.264 AnnexB工具函数（从RtmpPusher那边“平移”一份过来）
    static int findStartCode(const uint8_t* p,int end,int &off);
    static void parseH264AnnexBForSpsPps(const uint8_t *data,int size,QByteArray &sps,QByteArray &pps,bool &isKeyFrame);

};

#endif // AVRECEIVER_H
