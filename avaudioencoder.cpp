#include "avaudioencoder.h"
#include<QDebug>

AvAudioEncoder::AvAudioEncoder(QObject *parent)
    : QObject{parent}
{}

AvAudioEncoder::~AvAudioEncoder()
{
    close();
}

bool AvAudioEncoder::open(int sampleRate, int channels)
{
    if(opened_) return true;

    sampleRate_=sampleRate>0?sampleRate:44100;
    channels_=channels>0?channels:1;

    codec_=avcodec_find_encoder(AV_CODEC_ID_AAC);
    if(!codec_){
        qWarning()<<"[AvAudioEncoder] AAC encoder not found";
        return false;
    }

    ctx_=avcodec_alloc_context3(codec_);
    if(!ctx_) return false;

    av_channel_layout_default(&ctx_->ch_layout,channels);
    ctx_->sample_rate=sampleRate_;
    ctx_->time_base=AVRational{1,sampleRate_};
    ctx_->bit_rate=64000;//mono 64k 足够

    //选择encoder支持的sample_fmt,优先FLTP
    AVSampleFormat pick=AV_SAMPLE_FMT_FLTP;
    if(codec_->sample_fmts){
        pick=codec_->sample_fmts[0];
        const AVSampleFormat* p=codec_->sample_fmts;
        while(*p!=AV_SAMPLE_FMT_NONE){
            if(*p==AV_SAMPLE_FMT_FLTP){
                pick=AV_SAMPLE_FMT_FLTP;
                break;
            }
            ++p;
        }
    }
    ctx_->sample_fmt=pick;

    if(avcodec_open2(ctx_,codec_,nullptr)<0){
        qWarning()<<"[AvAudioEncoder] avcodec_open2 failed";
        close();
        return false;
    }

    //S16->encoder sample_fmt
    AVChannelLayout inLayout;
    av_channel_layout_default(&inLayout,channels_);
    swr_=swr_alloc();
    if(!swr_){
        close();
        return false;
    }
    if(swr_alloc_set_opts2(&swr_,&ctx_->ch_layout,ctx_->sample_fmt,ctx_->sample_rate,&inLayout,AV_SAMPLE_FMT_S16,sampleRate_,0,nullptr)<0){
        av_channel_layout_uninit(&inLayout);
        close();
        return false;
    }
    av_channel_layout_uninit(&inLayout);

    if(swr_init(swr_)<0){
        close();
        return false;
    }

    pkt_=av_packet_alloc();
    if(!pkt_){
        close();
        return false;
    }
    pcmBuf_.clear();
    nextPts_=0;
    ptsBase=0;
    ptsInited=false;
    hasLastPts=false;
    lastPtsMs=0;
    opened_=true;
    return true;
}

void AvAudioEncoder::close()
{
    opened_=false;
    pcmBuf_.clear();

    if(pkt_){
        av_packet_free(&pkt_);
        pkt_=nullptr;
    }
    if(frame_){
        av_frame_free(&frame_);
        frame_=nullptr;
    }
    if(swr_){
        swr_free(&swr_);
        swr_=nullptr;
    }
    if(ctx_){
        avcodec_free_context(&ctx_);
        ctx_=nullptr;
    }
    codec_=nullptr;
}

void AvAudioEncoder::pushPcm(const QByteArray &pcm)
{
    if(!opened_||pcm.isEmpty()) return;

    const int16_t* samples=reinterpret_cast<const int16_t*>(pcm.constData());
    int sampleCount=pcm.size()/2;//S16
    pcmBuf_.insert(pcmBuf_.end(),samples,samples+sampleCount);

    const int frameSize=(ctx_->frame_size>0)?ctx_->frame_size:1024;
    const int need=frameSize* channels_;

    while((int)pcmBuf_.size()>=need){
        if(!frame_){
            frame_=av_frame_alloc();
            frame_->nb_samples=frameSize;
            frame_->format=ctx_->sample_fmt;
            frame_->ch_layout=ctx_->ch_layout;
            frame_->sample_rate=ctx_->sample_rate;
            if(av_frame_get_buffer(frame_,0)<0) return;
        }
        if(av_frame_make_writable(frame_)<0) return;

        const uint8_t *inData[1]={reinterpret_cast<const uint8_t*>(pcmBuf_.data())};
        int ret=swr_convert(swr_,frame_->data,frameSize,inData,frameSize);
        if(ret<0) return;

        frame_->nb_samples=ret;
        frame_->pts=nextPts_;
        nextPts_+=ret;

        //消费输入
        pcmBuf_.erase(pcmBuf_.begin(),pcmBuf_.begin()+need);

        ret=avcodec_send_frame(ctx_,frame_);
        if(ret<0) continue;

        while(ret>=0){
            ret=avcodec_receive_packet(ctx_,pkt_);
            if(ret==AVERROR(EAGAIN)||ret==AVERROR_EOF) break;
            if(ret<0) break;

            int64_t pts=(pkt_->pts!=AV_NOPTS_VALUE)?pkt_->pts:frame_->pts;
            if(!ptsInited){
                ptsBase=(pts<0?-pts:0);
                ptsInited=true;
            }
            pts+=ptsBase;
            if(pts<0) pts=0;

            quint32 pts_ms=(quint32)av_rescale_q(pts,ctx_->time_base,AVRational{1,1000});
            if(hasLastPts&&pts_ms<=lastPtsMs)   pts_ms=lastPtsMs+1;
            lastPtsMs=pts_ms;
            hasLastPts=true;

            QByteArray ba(reinterpret_cast<const char*>(pkt_->data),pkt_->size);
            emit audioPacketReady(ba,pts_ms);

            av_packet_unref(pkt_);
        }
    }
}

