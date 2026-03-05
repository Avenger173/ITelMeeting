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

    bool open(int sampleRate,int channels); // 初始化编码器（指定采样率、声道数）
    void close();   // 释放编码器、重采样器等资源
public slots:
    //输入S16PCM(AudioCaptrue 输出就是这个)
    void pushPcm(const QByteArray &pcm);
signals:
    // 编码完成后对外发送压缩音频包（数据 + 时间戳）
    void audioPacketReady(const QByteArray &pktData,quint32 pts_ms);
private:
    const AVCodec* codec_=nullptr;  // 编码器（比如 AAC/MP3 编码器）
    AVCodecContext* ctx_=nullptr;   // 编码器上下文（存储编码参数、状态）
    SwrContext* swr_=nullptr;   // 重采样上下文（格式转换）
    AVFrame* frame_=nullptr;    // 存储待编码的音频帧（PCM 数据）
    AVPacket* pkt_=nullptr; // 存储编码后的音频包（压缩数据）
    // 音频参数
    int sampleRate_=44100;
    int channels_=1;
    // 时间戳（PTS）相关（音视频同步关键）
    int64_t nextPts_=0; // 下一个帧的 PTS
    int64_t ptsBase=0;  // PTS 基准值
    bool ptsInited=false;   // PTS 初始化标记
    bool hasLastPts=false;  // 上一个 PTS 是否有效
    quint32 lastPtsMs=0;     // 上一个毫秒级 PTS

    bool opened_=false; // 编码器是否已打开（状态标记）

    std::vector<int16_t> pcmBuf_;   // PCM 数据缓存（处理分包/拼接）
};

#endif // AVAUDIOENCODER_H
