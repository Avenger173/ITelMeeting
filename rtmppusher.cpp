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

bool RtmpPusher::start(const QString& rtmpUrl,int fps,int sampleRate)
{
    if(opened_) return true;
    url_=rtmpUrl;
    vFps=(fps>0 ? fps : vFps);
    aRate=(sampleRate>0 ? sampleRate : aRate);

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
    vStream_->time_base=AVRational{1,1000};
    vStream_->codecpar->codec_type=AVMEDIA_TYPE_VIDEO;
    vStream_->codecpar->codec_id=AV_CODEC_ID_H264;
    vStream_->codecpar->width=vW;
    vStream_->codecpar->height=vH;


    //音频是AAC
    aStream_->id=1;
    aStream_->time_base=AVRational{1,1000};
    aStream_->codecpar->codec_type=AVMEDIA_TYPE_AUDIO;
    aStream_->codecpar->codec_id=AV_CODEC_ID_AAC;
    aStream_->codecpar->sample_rate=aRate;
    aStream_->codecpar->format=AV_SAMPLE_FMT_FLTP;//典型AAC
    av_channel_layout_default(&aStream_->codecpar->ch_layout,aCh);//当前为单声道

    //打开IO
    if(!(fmtCtx_->oformat->flags&AVFMT_NOFILE)){
        if(avio_open(&fmtCtx_->pb,url_.toUtf8().constData(),AVIO_FLAG_WRITE)<0){
            qWarning()<<"[RtmpPusher] avio_open failed";
            return false;
        }
    }

    vFirst_=aFirst_=true;
    opened_=true;
    qInfo()<<"[RtmpPusher] started->"<<url_
            <<"video"<<vW<<"x"<<vH<<"@"<<vFps
            <<"audio"<<aRate<<"Hz ch="<<aCh;
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

    //1.尝试从AnnexB里解析出SPS/PPS,并确认是否为关键帧
    QByteArray sps,pps;
    bool isKey=false;
    parseH264AnnexBForSpsPps(reinterpret_cast<const uint8_t*>(pktData.constData()),pktData.size,sps,pps,isKey);
    if(!haveVConf){
        if(!sps.isEmpty()&&!pps.isEmpty()){
            vExtra_=makeAvcCFromSpsPps(sps,pps);
            haveVConf=true;
            //因为vW/vH从外部知道，不强依赖解析sps来推断
        }
    }
    //2.如果两边config都就绪,且还没写header->写
    if(!fmtCtx_->nb_streams||!ensureHeaderWritten()){
        //ensureHeaderWritten();内部负责写header,只写一次，写不出来就先等
        return;
    }
    //3.正常送包
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data=const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(pktData.constData()));
    pkt.size=pktData.size();
    pkt.pts=pkt.dts=pts_ms;//毫秒信号
    //key:给容器一个大致的duration(可选)
    pkt.duration=0;

    av_packet_rescale_ts(&pkt,AVRational{1,1000},vStream_->time_base);
    pkt.stream_index=vStream_->index;
    int r=av_interleaved_write_frame(fmtCtx_,&pkt);
    if(r<0){
        char err[128];
        av_strerror(r,err,sizeof(err));
        qWarning()<<"[RtmpPusher] write video failed:"<<err;
    }
}

void RtmpPusher::pushEncodeAudio(const QByteArray &pktData, quint32 pts_ms)
{
    if(!opened_||pktData.isEmpty()) return;

    if(!haveAConf){
        aExtra_=makeAacAsc(aRate,aCh);
        haveAConf=true;
    }
    if(!fmtCtx_->nb_streams||!ensureHeaderWritten()){
        return;
    }

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data=const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(pktData.constData()));
    pkt.size=pktData.size();
    pkt.pts=pkt.dts=pts_ms;//毫秒信号
    //key:给容器一个大致的duration(可选)
    pkt.duration=0;

    av_packet_rescale_ts(&pkt,AVRational{1,1000},aStream_->time_base);
    pkt.stream_index=aStream_->index;
    int r=av_interleaved_write_frame(fmtCtx_,&pkt);
    if(r<0){
        char err[128];
        av_strerror(r,err,sizeof(err));
        qWarning()<<"[RtmpPusher] write audio failed:"<<err;
    }
}

void RtmpPusher::writeInterleaved_(AVPacket *pkt, AVStream *st, AVRational inTb)
{
    // qDebug()<<"[RTMP] before scale pts/dts="<<pkt->pts<<pts->dts<<"inTb"<<inTb.num<<"/"<<inTb.den;
//把输入的毫秒time_base(1/1000)转为流的time_base(这里也是1/1000，就等价了）
    av_packet_rescale_ts(pkt,inTb,st->time_base);
    pkt->stream_index=st->index;

    // qDebug()<<"[RTMP] after scale pts/dts="<<pkt->pts<<pts->dts<<"stTb"<<st->time_base.num<<"/"<<st->time_base.den;
    const int r=av_interleaved_write_frame(fmtCtx_,pkt);
    if(r<0){
        char err[128];
        av_strerror(r,err,sizeof(err));
        qWarning()<<"[RtmpPusher] write frame failed:"<<err;
    }
}

void RtmpPusher::parseH264AnnexBForSpsPps(const uint8_t *data, int size, QByteArray &sps, QByteArray &pps, bool &isKeyFrame)
{
    isKeyFrame=false;
    int pos=0,sc,next;
    while((sc=findStartCode(data,size,pos))>=0){
        int nalStart =pos;
        int tmp=pos;
        next =findStartCode(data,size,tmp);
        int nalEnd=(next<0? size: next);
        int payload=nalEnd-nalStart;
        if(payload<=0) break;

        uint8_t nalType=data[nalStart]&0x1F;//H264
        const uint8_t* nalData=data+nalStart;
        int nalSize=payload;

        if(nalType==7){//SPS
            sps=QByteArray(reinterpret_cast<const char*>(nalData),nalSize);
        }else if(nalType==8){//PPS
            pps=QByteArray(reinterpret_cast<const char*>(nalData),nalSize);
        }else if(nalType==5){//IDR
            isKeyFrame=true;
        }
        pos=nalEnd;
    }
}

QByteArray RtmpPusher::makeAvcCFromSpsPps(const QByteArray &sps, const QByteArray &pps)
{
    QByteArray avcc;
    if(sps.size()<4||pps.isEmpty()) return avcc;

    const uint8_t* s=reinterpret_cast<const uint8_t*>(sps.constData());
    uint8_t profile_idc=s[1];
    uint8_t profile_comp=s[2];
    uint8_t level_idc =s[3];

    avcc.resize(5);
    avcc[0]=0x01;//version
    avcc[1]=profile_idc;
    avcc[2]=profile_comp;
    avcc[3]=level_idc;
    avcc[4]=0xFF;

    //SPS
    avcc.append(char(0xE1));
    uint16_t spslen=static_cast<uint16_t>(sps.size());
    avcc.append(char((spslen>>8)&0xFF));
    avcc.append(char((spslen   )&0xFF));
    avcc.append(sps);
    //PPS
    avcc.append(char(0x01));
    uint16_t ppslen=static_cast<uint16_t>(pps.size());
    avcc.append(char((ppslen>>8)&0xFF));
    avcc.append(char((ppslen)&0xFF));
    avcc.append(pps);

    return avcc;
}

uint8_t RtmpPusher::srIndexFromRate(int sr)
{
    //按ISO表
    static const int srs[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    for(int i=0;i<13;++i) if(srs[i]==sr) return (uint8_t);
    //找不到就用44100的index=4
    return 4;
}

QByteArray RtmpPusher::makeAacAsc(int sampleRate, int channles)
{
    uint8_t aot=2;
    uint8_t sfi=srIndexFromRate(sampleRate);
    uint8_t ch=(channles<0?1:(channles>7?2:(uint8_t)channles));

    QByteArray asc(2,0);
    asc[0]=(aot<<3)|((sfi>>1)&0x07);
    asc[1]=((sfi&0x01)<<7)|(ch<<3);
    return asc;
}

bool RtmpPusher::ensureHeaderWritten()
{
    //条件:两边extradata都齐了
    if(!haveVConf||!haveAConf) return false;
    //已经写过header就不重复
    if(fmtCtx_->pb&&(fmtCtx_->flags&AVFMT_FLAG_CUSTOM_IO)==0&&fmtCtx_->streams[0]->codecpar->extradata){
        return true;
    }
    //补vStream的extradata
    if(vStream_->codecpar->extradata){
        av_free(vStream_->codecpar->extradata);
        vStream_->codecpar->extradata=nullptr;
        vStream_->codecpar->extradata_size=0;
    }
    vStream_->codecpar->extradata=(uint8_t*)av_malloc(vExtra_.size()+AV_INPUT_BUFFER_PADDING_SIZE);
    vStream_->codecpar->extradata_size=vExtra_.size();
    memcpy(vStream_->codecpar->extradata,vExtra_.constData(),vExtra_.size());
    memset(vStream_->codecpar->extradata+vExtra_.size(),0,AV_INPUT_BUFFER_PADDING_SIZE);
    //补aStream的extradata
    if(aStream_->codecpar->extradata){
        av_free(aStream_->codecpar->extradata);
        aStream_->codecpar->extradata=nullptr;
        aStream_->codecpar->extradata_size=0;
    }
    aStream_->codecpar->extradata=(uint8_t*)av_malloc(aExtra_.size()+AV_INPUT_BUFFER_PADDING_SIZE);
    aStream_->codecpar->extradata_size=aExtra_.size();
    memcpy(aStream_->codecpar->extradata,aExtra_.constData(),aExtra_.size());
    memset(aStream_->codecpar->extradata+aExtra_.size(),0,AV_INPUT_BUFFER_PADDING_SIZE);

    //现在写header(只写一次)
    if(avformat_write_header(fmtCtx_,nullptr)<0){
        qWarning()<<"[RtmpPusher] avformat_write_header failed(ensureHeaderWritten)";
        return false;
    }
    qInfo()<<"[RtmpPusher] header written (FLV with H264/AAC)";
    return ture;
}
//找到AnnexB起始码(00 00 00 01或00 00 01)
static inline int findStartCode(const uint8_t* p,int end,int& off){
    for(int i=off;i+3<end;++i){
        if(p[i]==0x00&&((p[i+1]==0x00)&&((p[i+2]==0x01)||p[i+2]==0x00&&p[i+3]==0x01))){
            off=(p[i+2]==0x01)?(i+3):(i+4);
            return i;
        }
    }
    return -1;
}
