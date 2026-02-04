#ifndef AVAUDIOENCODER_H
#define AVAUDIOENCODER_H

#include <QObject>
#include<QByteArray>
#include<vector>
#include<QElapsedTimer>
#include<QThread>
extern "C"{
#include<libavcodec/avcodec.h>
#include<libswresample/swresample.h>
#include<libavutil/opt.h>
#include<libavutil/channel_layout.h>
}
class AvAudioEncoder : public QObject
{
    Q_OBJECT
public:
    explicit AvAudioEncoder(QObject *parent = nullptr);
    ~AvAudioEncoder();

    bool open(int sampleRate,int channels);
    void close();
public slots:
    //输入S16PCM(AudioCaptrue 输出就是这个)
    void pushPcm(const QByteArray &pcm);
signals:
    void audioPacketReady(const QByteArray &pktData,quint32 pts_ms);
private:
    const AVCodec* codec_=nullptr;
    AVCodecContext* ctx_=nullptr;
    SwrContext* swr_=nullptr;
    AVFrame* frame_=nullptr;
    AVPacket* pkt_=nullptr;

    int sampleRate_=44100;
    int channels_=1;
    int64_t nextPts_=0;
    int64_t ptsBase=0;
    bool ptsInited=false;
    bool hasLastPts=false;
    quint32 lastPtsMs=0;
    bool opened_=false;

    std::vector<int16_t> pcmBuf_;
};

#endif // AVAUDIOENCODER_H
