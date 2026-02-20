#include "avnetencoder.h"
#include<QDebug>

static bool isAnnexB(const uint8_t* p,int n);
AvNetEncoder::AvNetEncoder(QObject *parent)
    : QObject{parent}
{}

AvNetEncoder::~AvNetEncoder()
{
    close();
}

bool AvNetEncoder::openVideo(int w, int h, int fps, int bitrate)
{
    if(opened_) return true;
    w_=w;
    h_=h;
    fps_=fps>0?fps:30;
    bitrate_ = bitrate > 0 ? bitrate : 2200000;

    vCodec_=avcodec_find_encoder_by_name("libx264");
    if(!vCodec_) vCodec_=avcodec_find_encoder(AV_CODEC_ID_H264);
    if(!vCodec_){
        qWarning()<<"[AvNetEncoder] H264 encoder not found";
        return false;
    }

    vCtx_=avcodec_alloc_context3(vCodec_);
    vCtx_->codec_type=AVMEDIA_TYPE_VIDEO;
    vCtx_->width=w_;
    vCtx_->height=h_;
    vCtx_->pix_fmt=AV_PIX_FMT_YUV420P;
    vCtx_->time_base=AVRational{1,1000};
    vCtx_->framerate=AVRational{fps_,1};
    vCtx_->bit_rate=bitrate_;
    const int gop=qMax(1,fps_);
    vCtx_->gop_size=gop;
    vCtx_->max_b_frames=0;//实时禁言B帧
    vCtx_->keyint_min=gop;

    if(vCodec_->id==AV_CODEC_ID_H264&&vCtx_->priv_data){
        setX2640pts_(vCtx_,fps_);
    }

    //让H264的SPS/PPS进extradata
    vCtx_->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary* opts=nullptr;

    //x264的参数必须通过opts传入，最稳
    av_dict_set(&opts,"preset","ultrafast",0);
    av_dict_set(&opts,"tune","zerolatency",0);

    //repeat headers+annexb+固定GOP
    //repeat-headers=1:每个IDR前都带SPS/PPS(接收端中途加入也能解
    //annexb=1:输出00 00 00 01起始码
    //keyint/min-keyint:控制IDR间隔
    //scenecut=0:不让场景切换乱改GOP
    av_dict_set(&opts,"x264-params",QString("repeat-headers=1:annexb=1:keyint=%1:min-keyint=%1:scenecut=0").arg(gop).toUtf8().constData(),0);
    av_dict_set(&opts,"g",QString::number(gop).toUtf8().constData(),0);

    int openRet=avcodec_open2(vCtx_,vCodec_,&opts);
    av_dict_free(&opts);
    if(openRet<0){
        qWarning()<<"[AvNetEncoder] avcodec_open2 failed";
        close();
        return false;
    }
    configEmitted=false;


    qDebug()<<"[AvNetEncoder] encoder="<<(vCodec_?vCodec_->name:"null")
             <<"id="<<(vCodec_?vCodec_->id:-1);

    //open2之后再看extradata
    if(vCtx_->extradata&&vCtx_->extradata_size>0){
        qDebug()<<"[AvNetEncoder] extradata ready.size="<<vCtx_->extradata_size
                 <<"first16="
                 <<QByteArray((const char*)vCtx_->extradata,qMin(16,vCtx_->extradata_size)).toHex();
    }else{
        qWarning()<<"[AvNetEncoder] still no extradata after open2";
    }

    yuv_=av_frame_alloc();
    yuv_->format=vCtx_->pix_fmt;
    yuv_->width=w_;
    yuv_->height=h_;
    if(av_frame_get_buffer(yuv_,32)<0){
        qWarning()<<"[AvNetEncoder] av_frame_get_buffer failed";
        close();
        return false;
    }

    pkt_=av_packet_alloc();

    sws_=sws_getContext(w_,h_,AV_PIX_FMT_RGB24,
                        w_,h_,AV_PIX_FMT_YUV420P,
                        SWS_BILINEAR,nullptr,nullptr,nullptr);

    if(!sws_){
        qWarning()<<"[AvNetEncoder] sws_getContext failed";
        close();
        return false;
    }

    opened_=true;
    vFrameIndex_=0;
    vClockStarted_=false;
    lastPtsMs_=0;
    qDebug()<<"[AvNetEncoder] openVideo OK"<<w_<<h_<<"@"<<fps_;
    return true;
}

void AvNetEncoder::close()
{
    opened_=false;
    if(sws_){sws_freeContext(sws_);sws_=nullptr;}
    if(pkt_){av_packet_free(&pkt_);}
    if(yuv_){av_frame_free(&yuv_);}
    if(vCtx_){avcodec_free_context(&vCtx_);}
    vCodec_=nullptr;
}

void AvNetEncoder::pushVideoFrame(const QImage &img)
{
    if(!opened_||!vCtx_) return;
    if(img.isNull()) return;

    //只接受RGB888,其他格式先转
    QImage rgb=img.format()==QImage::Format_RGB888?img:img.convertToFormat(QImage::Format_RGB888);

    if(rgb.width()!=w_||rgb.height()!=h_){
        rgb=rgb.scaled(w_,h_,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    }

    av_frame_make_writable(yuv_);

    const uint8_t* srcData[4]={rgb.constBits(),nullptr,nullptr,nullptr};
    int srcLinesize[4]={static_cast<int>(rgb.bytesPerLine()),0,0,0};

    sws_scale(sws_,srcData,srcLinesize,0,h_,yuv_->data,yuv_->linesize);
    const int gop=qMax(1,fps_);
    //强制关键帧：第一帧+每2秒一帧
    const bool forceKey=(vFrameIndex_==0||(vFrameIndex_%gop==0));
    if(forceKey){
        yuv_->pict_type=AV_PICTURE_TYPE_I;//强制I帧
    }else{
        yuv_->pict_type=AV_PICTURE_TYPE_NONE;
    }
    yuv_->key_frame=forceKey?1:0;
    if(!vClockStarted_){
        vClock_.start();
        vClockStarted_=true;
    }
    qint64 ptsMs=vClock_.elapsed();
    if(ptsMs<=lastPtsMs_) ptsMs=lastPtsMs_+1;
    lastPtsMs_=ptsMs;
    yuv_->pts=av_rescale_q(ptsMs,AVRational{1,1000},vCtx_->time_base);
    vFrameIndex_++;

    int ret=avcodec_send_frame(vCtx_,yuv_);
    if(ret<0) return;

    while(true){
        ret=avcodec_receive_packet(vCtx_,pkt_);
        if(ret==AVERROR(EAGAIN)||ret==AVERROR_EOF) break;
        if(ret<0) break;

        //pts_ms:用帧序号换算即可
        int64_t pktPts=(pkt_->pts!=AV_NOPTS_VALUE)?pkt_->pts:yuv_->pts;
        const quint32 pts_ms=(quint32)av_rescale_q(pktPts,vCtx_->time_base,AVRational{1,1000});

        QByteArray ba(reinterpret_cast<const char*>(pkt_->data),pkt_->size);
        //一旦extradata出现，立刻发一次SPS/PPS配置包(AnnexB)
        if(!configEmitted&&vCtx_->extradata&&vCtx_->extradata_size>0){
            QByteArray cfg;
            if(isAnnexB(vCtx_->extradata,vCtx_->extradata_size)){
                cfg=QByteArray(reinterpret_cast<const char*>(vCtx_->extradata),vCtx_->extradata_size);
            }else{
                cfg=avccExtradataToAnnexB(vCtx_->extradata,vCtx_->extradata_size);
            }
            if(!cfg.isEmpty()){
                configEmitted=true;
                qDebug()<<"[AvNetEncoder] extradata ready after packet,emit config size="
                         <<cfg.size()<<"first16="<<cfg.left(16).toHex();
                emit videoPacketReady(cfg,0);
            }
        }
        emit videoPacketReady(ba,pts_ms);
        av_packet_unref(pkt_);
    }
}

void AvNetEncoder::setX2640pts_(AVCodecContext *c, int fps)
{
    //低延迟+关键：每个关键帧前重复SPS/PPS,并输出AnnexB
    av_opt_set(c->priv_data,"preset","ultrafast",0);
    av_opt_set(c->priv_data,"tune","zerolatency",0);
    av_opt_set(c->priv_data,"repeat-headers","1",0);
    av_opt_set(c->priv_data,"annexb","1",0);
    //GOP:2秒一个IDR，方便接收端中途加入
    const int gop=qMax(1,fps);
    av_opt_set_int(c->priv_data,"keyint",gop,0);
    av_opt_set_int(c->priv_data,"min-keyint",gop,0);
    av_opt_set(c->priv_data,"scenecut","0",0);
}

QByteArray AvNetEncoder::avccExtradataToAnnexB(const uint8_t *extra, int extraSize)
{
    QByteArray out;
    if(!extra||extraSize<7) return out;

    int pos=0;
    if(extra[pos]!=1) return out;
    pos+=5;

    int numSps=extra[pos]&0x1F;
    pos++;

    for(int i=0;i<numSps;++i){
        if(pos+2>extraSize) return QByteArray();
        int spslen=(extra[pos]<<8) | extra[pos+1];
        pos+=2;
        if(pos+spslen>extraSize) return QByteArray();
        out.append("\x00\x00\x00\x01",4);
        out.append(reinterpret_cast<const char*>(extra+pos),spslen);
        pos+=spslen;
    }

    if(pos+1>extraSize)  return QByteArray();
    int numPps=extra[pos];
    pos++;

    for(int i=0;i<numPps;++i){
        if(pos+2>extraSize) return QByteArray();
        int ppsLen=(extra[pos]<<8) | extra[pos+1];
        pos+=2;
        if(pos+ppsLen>extraSize) return QByteArray();
        out.append("\x00\x00\x00\x01",4);
        out.append(reinterpret_cast<const char*>(extra+pos),ppsLen);
        pos+=ppsLen;
    }

    return out;
}

static bool isAnnexB(const uint8_t* p,int n){
    if(!p||n<4)  return false;
    //00 00 01或00 00 00 01
    return (p[0]==0&&p[1]==0&&p[2]==1)||(p[0]==0&&p[1]==0&&p[2]==0&&p[3]==1);
}

void AvNetEncoder::emitVideoConfigOnce()
{
    if(!vCtx_||configEmitted) return;
    if(!vCtx_->extradata||vCtx_->extradata_size<=0){
        qWarning()<<"[AvNetEncoder] no extradata";
        return;
    }

    const uint8_t* extra=vCtx_->extradata;
    const int extraSize=vCtx_->extradata_size;

    QByteArray cfg;

    if(isAnnexB(extra,extraSize)){
        //extradata已经是AnnexB
        cfg=QByteArray(reinterpret_cast<const char*>(extra),extraSize);
        qDebug()<<"[AvNetEncoder] extradata is AnnexB,send directly size="<<cfg.size()
                 <<"first16="<<cfg.left(16).toHex();
    }else{
        //AVCC才需要转换
        cfg=avccExtradataToAnnexB(extra,extraSize);
        qDebug()<<"[AvNetEncoder] extradata is AVCC,converted size="<<cfg.size()
                 <<"first16="<<cfg.left(16).toHex();
    }

    if(cfg.isEmpty()){
        qWarning()<<"[AvNetEncoder] config empty,not sent";
        return;
    }

    configEmitted=true;
    emit videoPacketReady(cfg,0);
}

