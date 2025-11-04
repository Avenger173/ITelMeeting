#include "rtmppusher.h"
#include<QDebug>
RtmpPusher::RtmpPusher(QObject *parent)
    : QObject{parent}
{
    avformat_network_init();
}

RtmpPusher::~RtmpPusher()
{
    stop();
    avformat_network_deinit();
}

bool RtmpPusher::start(const QString &rtmpUrl, int fps, int sampleRate)
{
    if(opened_) return true;
    url_=rtmpUrl;

    if(avformat_alloc_output_context2(&fmtCtx_,nullptr,"flv",url_.toUtf8().constData())<0||!fmtCtx_){
        qWarning()<<"[RtmpPusher] avformat_alloc_output_context2 failed";
        return false;
    }
    //新建音视频流(容器流,编码在外部完成)
    vStream_=avformat_new_stream(fmtCtx_,nullptr);
    aStream_=avformat_new_stream(fmtCtx_,nullptr);
    if(!vStream_||!aStream_){
        qWarning()<<"[RtmpPusher] avformat_new_stream failed";
        return false;
    }
    //告诉容器:视频是H264，时间基于毫秒(后续做rescale)
    vStream_->id=0;
    vStream_->time_base=AVRational{1,1000};//按毫秒喂
    vStream_->codecpar->codec_type=AVMEDIA_TYPE_VIDEO;
    vStream_->codecpar->codec_id=AV_CODEC_ID_H264;

    //音频是AAC
    aStream_->id=1;
    aStream_->time_base=AVRational{1,1000};
    aStream_->codecpar->codec_type=AVMEDIA_TYPE_AUDIO;
    aStream_->codecpar->codec_id=AV_CODEC_ID_AAC;
    aStream_->codecpar->sample_rate=sampleRate;
    aStream_->codecpar->format=AV_SAMPLE_FMT_FLTP;//典型AAC
    av_channel_layout_default(&aStream_->codecpar->ch_layout,1);//当前为单声道
    aStream_->time_base=AVRational{1,sampleRate};

    if(!(fmtCtx_->oformat->flags&AVFMT_NOFILE)){
        if(avio_open(&fmtCtx_->pb,url_.toUtf8().constData(),AVIO_FLAG_WRITE)<0){
            qWarning()<<"[RtmpPusher] avio_open failed";
            return false;
        }
    }

    if(avformat_write_header(fmtCtx_,nullptr)<0){
        qWarning()<<"[RtmpPusher] avformat_write_header failed";
        return false;
    }

    vFirst_=aFirst_=true;
    opened_=true;
    qInfo()<<"[RtmpPusher] started->"<<url_;
    return true;
}

void RtmpPusher::stop()
{
    if(!opened_) return;
    if(fmtCtx_){
        av_write_trailer(fmtCtx_);
        if(!(fmtCtx_->oformat->flags&AVFMT_NOFILE)&&fmtCtx_->pb){
            avio_closep(&fmtCtx_->pb);
        }
        avformat_free_context(fmtCtx_);
        fmtCtx_=nullptr;
    }
    if(aStream_&&aStream_->codecpar){
        av_channel_layout_uninit(&aStream_->codecpar->ch_layout);
    }
    opened_=false;
    qInfo()<<"RtmpPusher] stopped";
}

void RtmpPusher::pushEncodeVideo(const QByteArray &pktData, quint32 pts_ms)
{
    if(!opened_||pktData.isEmpty()) return;

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data=const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(pktData.constData()));
    pkt.size=pktData.size();
    pkt.pts=pkt.dts=pts_ms;//毫秒信号
    //key:给容器一个大致的duration(可选)
    pkt.duration=0;
    writeInterleaved_(&pkt,vStream_,vTimeBase_);
}

void RtmpPusher::pushEncodeAudio(const QByteArray &pktData, quint32 pts_ms)
{
    if(!opened_||pktData.isEmpty()) return;

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data=const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(pktData.constData()));
    pkt.size=pktData.size();
    pkt.pts=pkt.dts=pts_ms;//毫秒信号
    //key:给容器一个大致的duration(可选)
    pkt.duration=0;
    writeInterleaved_(&pkt,aStream_,aTimeBase_);
}

void RtmpPusher::writeInterleaved_(AVPacket *pkt, AVStream *st, AVRational inTb)
{
//把输入的毫秒time_base(1/1000)转为流的time_base(这里也是1/1000，就等价了）
    av_packet_rescale_ts(pkt,inTb,st->time_base);
    pkt->stream_index=st->index;
    int r=av_interleaved_write_frame(fmtCtx_,pkt);
    if(r<0){
        char err[128];
        av_strerror(r,err,sizeof(err));
        qWarning()<<"[RtmpPusher] write frame failed:"<<err;
    }
}

