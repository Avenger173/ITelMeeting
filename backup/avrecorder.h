  #ifndef AVRECORDER_H
#define AVRECORDER_H

#include <QObject>
#include <QMutex>
#include<QThread>
#include<QWaitCondition>
#include<QQueue>
#include<QImage>
#include<atomic>
#include<QString>
#include <opencv2/opencv.hpp>

extern "C" {
#include <libavformat/avformat.h>//格式上下文/复用器
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>//编解码器
#include <libavutil/opt.h>//AVDictionary等选项
#include <libavutil/time.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>//像素格式转换（BGR/RGB->YUV）
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
}


class AvRecorder : public QObject
{
    Q_OBJECT
public:
    explicit AvRecorder(QObject *parent = nullptr);
    ~AvRecorder();

    bool open(const QString &filename,int width,int height,int fps);
    bool openAV(const QString &filename,int width,int height,int fps,int sampleRate);
    void pushAudioPCM(const uint8_t* data,int nb_samples);//输入PCM的写入接口
    void pushVideoFrame(const QImage &img);
    void close();
    bool isOpen() const{return m_opened;}
private:
    //文件/容器层
    AVFormatContext *fmtCtx=nullptr;
    //视频编码器/流
    AVStream *vStream=nullptr;
    AVCodecContext *vCodecCtx=nullptr;
    SwsContext *sws=nullptr;//RGB->YUV
    //基本参数
    int w=0,h=0,m_fps=0;
    AVRational tb={1,1};//视频时间基
    std::atomic_bool m_opened{false};
    AVStream *aStream=nullptr;
    AVCodecContext *aCodecCtx=nullptr;
    SwrContext *aSwr=nullptr;
    AVChannelLayout aChLayout={};
    AVRational atb={1,44100};
    int64_t aNextPts=0;
    const AVCodec* pickAACEncoder() const;//选择AAC编码器
    AVFrame *vFrame=nullptr;//复用的视频缓冲(YUV420p)
    int64_t vNextPts=0;//逐帧累加的pts
    AVFrame *aFrame=nullptr;//重用的音频AVFrame
    std::vector<int16_t> audioBuffer;

private:
    const AVCodec *pickH264Encoder() const;//选择合适H.264编码器(libx264)
    static QString avErr(int errnum);
};

#endif // AVRECORDER_H
