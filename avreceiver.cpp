#include "avreceiver.h"
#include<QDateTime>
#include<QTimer>
#include<QDebug>
#include<QMediaDevices>
#include<QNetworkDatagram>
#include<QDataStream>
AVReceiver::AVReceiver(QObject *parent)
    : QObject{parent}
{
}

AVReceiver::~AVReceiver()
{
    stop();
    freeDecoders();
}

static int findStartCodePos(const uint8_t* p,int size,int from){
    for(int i=from;i+3<size;++i){
        //00 00 01
        if(p[i]==0&&p[i+1]==0&&p[i+2]==1) return i;
        //00 00 00 01
        if(i+4<size&&p[i]==0&&p[i+1]==0&&p[i+2]==0&&p[i+3]==1) return i;
    }
    return -1;
}

static int startCodeLen(const uint8_t* p,int pos,int size){
    if(pos+3<size&&p[pos]==0&&p[pos+1]==0&&p[pos+2]==1) return 3;
    if(pos+4<size&&p[pos]==0&&p[pos+1]==0&&p[pos+2]==0&&p[pos+3]==1) return 4;
    return 0;
}

//从AnnexB数据里：扫描所有NAL,提取SPS(type7)/PPS(type8)
static void extractSpsPpsFromAnnexB(const QByteArray& es,QByteArray& outSps,QByteArray& outPps){
    const uint8_t* p=reinterpret_cast<const uint8_t*>(es.constData());
    const int n=es.size();
    int pos=0;

    while(true){
        int sc=findStartCodePos(p,n,pos);
        if(sc<0) break;
        int scl=startCodeLen(p,sc,n);
        int nalStart=sc+scl;
        if(nalStart>=n) break;

        int next=findStartCodePos(p,n,nalStart);
        int nalEnd=(next<0)?n:next;

        int nalType=p[nalStart]&0x1F;

        if(nalType==7){
            //SPS
            outSps=es.mid(sc,nalEnd-sc);
        }else if(nalType==8){
            //PPS
            outPps=es.mid(sc,nalEnd-sc);
        }

        pos=nalEnd;
    }
}

bool AVReceiver::start(const QString &host, int videoPort,int audioPort)
{
    m_stopping=false;
    if(videoSocket||audioSocket){
        emit logMsg("[AVReceiver] 已经启动");
        return false;
    }
    m_destAddr=QHostAddress(host);
    m_videoPort=videoPort;
    m_audioPort=audioPort;

    //video socket
    videoSocket=new QUdpSocket(this);
    if(!videoSocket->bind(m_destAddr,videoPort)){
        qWarning()<<"[AVReceiver] 视频端口绑定失败";
        delete videoSocket;
        videoSocket=nullptr;
        return false;
    }
    connect(videoSocket,&QUdpSocket::readyRead,this,&AVReceiver::onVideoReadyRead);
    //Audio socket
    audioSocket=new QUdpSocket(this);
    if(!audioSocket->bind(m_destAddr,audioPort)){
        qWarning()<<"[AVReceiver] 音频端口绑定失败";
        delete audioSocket;
        audioSocket=nullptr;
        return false;
    }
    connect(audioSocket,&QUdpSocket::readyRead,this,&AVReceiver::onAudioReadyRead);

    qDebug()<<"[AVReceiver]已启动，监听"<<host<<":"<<videoPort<<","<<audioPort;
    return true;
}

void AVReceiver::stop()
{
    m_stopping=true;

    if(videoSocket){
        disconnect(videoSocket,nullptr,this,nullptr);
        videoSocket->close();
        delete videoSocket;
        videoSocket=nullptr;
    }
    if(audioSocket){
        disconnect(audioSocket,nullptr,this,nullptr);
        audioSocket->close();
        delete audioSocket;
        audioSocket=nullptr;
    }

    if(sws){
        sws_freeContext(sws);
        sws=nullptr;
    }
    if(pkt){
        av_packet_free(&pkt);
        pkt=nullptr;
    }
    if(vFrame){
        av_frame_free(&vFrame);
        vFrame=nullptr;
    }

    sps_.clear();
    pps_.clear();
}

void AVReceiver::onVideoReadyRead()
{
    if(m_stopping.load()){
        //丢掉积压数据，避免退出时还继续解码/发信号
        while(videoSocket&&videoSocket->hasPendingDatagrams()){
            videoSocket->receiveDatagram();
        }
        return;
    }
    while(videoSocket&&videoSocket->hasPendingDatagrams()){
        QByteArray data;
        data.resize(videoSocket->pendingDatagramSize());
        videoSocket->readDatagram(data.data(),data.size());
        processVideoPacket(data);
    }
}

void AVReceiver::onAudioReadyRead()
{
    if(m_stopping.load()){
        while(audioSocket&&audioSocket->hasPendingDatagrams()){
            audioSocket->receiveDatagram();
        }
        return;
    }
    while(audioSocket&&audioSocket->hasPendingDatagrams()){
        QByteArray data;
        data.resize(audioSocket->pendingDatagramSize());
        audioSocket->readDatagram(data.data(),data.size());
        processAudioPacket(data);
    }
}

void AVReceiver::processVideoPacket(const QByteArray &data)
{
    //处理视频数据（解码，显示）
    //我们自定义头：4+2+2+4+4=16字节
    if(data.size()<=16)
        return;
    if(!ensureVideoDecoder())
        return;

    //解析头部
    QDataStream s(data);
    s.setByteOrder(QDataStream::BigEndian);

    quint32 frameId;
    quint16 chunkId;
    quint16 chunkCount;
    quint32 ptsMs;
    quint32 totalSize;
    s>>frameId>>chunkId>>chunkCount>>ptsMs>>totalSize;

    QByteArray es=data.mid(16);//裁掉头部，剩下就是H264 ES
    if(es.isEmpty()) return;

    auto hex8=es.left(8).toHex();
    if(hex8.startsWith("0000000167")||hex8.startsWith("0000000168")||hex8.startsWith("00000167")||hex8.startsWith("00000168")){
        qDebug()<<"[AVReceiver] got config NAL begin="<<hex8<<"size="<<es.size();
    }

    static int printed=0;
    if(printed<20){//只打印前20包，够定位
        qDebug()<<"[AVReceiver] video datagram size="<<data.size()
                 <<"es size="<<es.size()
                 <<"first16="<<es.left(16).toHex();
        printed++;
    }

    const uint8_t* raw=reinterpret_cast<const uint8_t*>(es.constData());
    int rawSize=es.size();

    //1)从这一帧里尝试解析SPS/PPS和是否为关键帧
    QByteArray decodePayload;//最终喂给解码器的数据（尽量转为AnnextB）
    QByteArray sps,pps;
    bool isKey=false;

    const bool looksLikeAnnexB=(rawSize>=4)&&((raw[0]==0x00&&raw[1]==0x00&&raw[2]==0x01)||
                                            (raw[0]==0x00&&raw[1]==0x00&&raw[2]==0x00&&raw[3]==0x01));
    if(looksLikeAnnexB){
        decodePayload=es;
        extractSpsPpsFromAnnexB(es,sps,pps);

        if(!sps.isEmpty()||!pps.isEmpty()){
            qDebug()<<"[AVReceiver] nal scan:sps="<<sps.size()<<"pps="<<pps.size()
                     <<"head"<<es.left(8).toHex();
        }
    }else{
        //尝试把AVCC转为AnnexB,并从中抓SPS/PPS
        if(!avccToAnnexBAndExtract(raw,rawSize,decodePayload,sps,pps,isKey)){
            qWarning()<<"[AVReceiver] H264 payload format unknown,drop,first4="
                       <<QByteArray(es.constData(),qMin(4,es.size())).toHex();
            return;
        }
    }
    //2)如果还没视频配置，且这帧里带了SPS/PPS,就记下来
    if(!haveVConf){
            //允许分开拿到SPS/PPS
        if(!sps.isEmpty())  sps_=sps;
        if(!pps.isEmpty())  pps_=pps;

        if(!sps_.isEmpty()&&!pps_.isEmpty()){
            haveVConf=true;
            qDebug()<<"[AVReceiver] 捕获到SPS/PPS,开始初始化解码器配置"
                     <<"sps= "<<sps_.size()<<"pps="<<pps_.size();
            //执行解码器初始化逻辑
            AVPacket confPkt;
            av_init_packet(&confPkt);
            confPkt.data=reinterpret_cast<uint8_t*>(sps_.data());
            confPkt.size=sps_.size();
            avcodec_send_packet(vDecCtx,&confPkt);
            confPkt.data=reinterpret_cast<uint8_t*>(pps_.data());
            confPkt.size=pps_.size();
            avcodec_send_packet(vDecCtx,&confPkt);
        }else{
            //还没拿到SPS/PPS之前的帧一律丢弃
            return;
        }
    }

    //3)正常送当前帧
    av_packet_unref(pkt);
    pkt->data=reinterpret_cast<uint8_t*>(decodePayload.data());
    pkt->size=decodePayload.size();

    int ret=avcodec_send_packet(vDecCtx,pkt);
    if(ret<0){
        char err[128];
        av_strerror(ret,err,sizeof(err));
        qWarning()<<"[AVReceiver] send_packet失败"<<ret<<"msg:"<<err;
        return;
    }
    //4)把可用的帧都取出来，转成QImage,发给UI
    while(ret>=0){
        ret=avcodec_receive_frame(vDecCtx,vFrame);
        if(ret==AVERROR(EAGAIN)||ret==AVERROR_EOF)
            break;
        if(ret<0){
            char err[128];
            av_strerror(ret,err,sizeof(err));
            qWarning()<<"[AVReceiver] receive_frame失败"<<ret<<"msg:"<<err;
            break;
        }

        int w=vFrame->width;
        int h=vFrame->height;
        if(w<=0||h<=0)
            continue;

        if(!sws){
            sws=sws_getContext(
                w,h,static_cast<AVPixelFormat>(vDecCtx->pix_fmt)
                ,w,h,AV_PIX_FMT_RGB24
                ,SWS_BILINEAR,nullptr,nullptr,nullptr);
            if(!sws){
                qWarning()<<"[AVReceiver] 创建swsContext失败";
                return;
            }
        }

        QImage img(w,h,QImage::Format_RGB888);
        uint8_t *dstData[4];
        int dstLinesize[4];
        av_image_fill_arrays(dstData,dstLinesize,img.bits(),AV_PIX_FMT_RGB24,w,h,1);

        sws_scale(sws,vFrame->data,vFrame->linesize,0,h,dstData,dstLinesize);

        emit newVideoFrame(img.copy());
    }

    qDebug()<<"[AVReceiver] 接收到视频数据:"<<data.size();
    //解码并显示视频（后续实现）
}

void AVReceiver::processAudioPacket(const QByteArray &data)
{
    //处理音频数据（解码，播放）
    Q_UNUSED(data);
    qDebug()<<"[AVReceiver] 接收到音频数据:"<<data.size();
    //解码并播放音频（后续实现）
}

bool AVReceiver::ensureVideoDecoder()
{
    if(vDecCtx)
        return true;

    const AVCodec *dec=avcodec_find_decoder(AV_CODEC_ID_H264);
    if(!dec){
        qWarning()<<"[AVReceiver] 找不到H264解码器";
        return false;
    }

    vDecCtx=avcodec_alloc_context3(dec);
    if(!vDecCtx){
        qWarning()<<"[AVReceiver] 分配视频解码器上下文失败";
        return false;
    }

    //H264可以不提前设置宽高/像素格式，由解码器从码流里探测
    if(avcodec_open2(vDecCtx,dec,nullptr)<0){
        qWarning()<<"[AVReceiver] 打开视频解码器失败";
        avcodec_free_context(&vDecCtx);
        return false;
    }

    vFrame=av_frame_alloc();
    pkt=av_packet_alloc();
    if(!vFrame||!pkt){
        qWarning()<<"[AVReceiver] 分配视频帧/包失败";
        freeDecoders();
        return false;
    }

    return true;
}

void AVReceiver::freeDecoders()
{
    if(sws){
        sws_freeContext(sws);
        sws=nullptr;
    }
    if(vFrame){
        av_frame_free(&vFrame);
        vFrame=nullptr;
    }
    if(pkt){
        av_packet_free(&pkt);
        pkt=nullptr;
    }
    if(vDecCtx){
        avcodec_free_context(&vDecCtx);
        vDecCtx=nullptr;
    }
}

int AVReceiver::findStartCode(const uint8_t *p, int end, int &off)
{
    for(int i=off;i+3<end;++i){
        //匹配00 00 01或00 00 00 01
        if(p[i]==0x00&&p[i+1]==0x00&&
            (p[i+2]==0x01||(p[i+2]==0x00&&p[i+3]==0x01))){
            if(p[i+2]==0x01){
                //00 00 01->起始码后的位置
                off=i+3;
            }else{
                //00 00 00 01->起始码后的位置
                off=i+4;
            }
            return i;//返回起始码开始的位置
        }
    }
    return -1;
}

void AVReceiver::parseH264AnnexBForSpsPps(const uint8_t *data, int size, QByteArray &sps, QByteArray &pps, bool &isKeyFrame)
{
    sps.clear();
    pps.clear();
    isKeyFrame=false;

    int off=0;
    while(true){
        int start=findStartCode(data,size,off);
        if(start<0) break;
        int nextOff=off;
        int nextStart=findStartCode(data,size,nextOff);
        int nalStart=off;
        int nalEnd=(nextStart<0 ? size : nextStart);

        if(nalStart>=nalEnd) break;

        uint8_t nalHeader=data[nalStart];
        uint8_t nalType=nalHeader&0x1F;

        const uint8_t *nalPtr=data+nalStart;
        int nalSize=nalEnd-nalStart;

        switch(nalType){
        case 7://SPS
            sps=QByteArray(reinterpret_cast<const char*>(nalPtr),nalSize);
            break;
        case 8://PPS
            pps=QByteArray(reinterpret_cast<const char*>(nalPtr),nalSize);
            break;
        case 5://IDR
            isKeyFrame=true;
            break;
        default:
            break;
        }

        off=nextOff;
        if(nextStart<0) break;
    }
}

bool AVReceiver::avccToAnnexBAndExtract(const uint8_t *data, int size, QByteArray &annexb, QByteArray &sps, QByteArray &pps, bool &isKeyFrame)
{
    annexb.clear();
    sps.clear();
    pps.clear();
    isKeyFrame=false;
    if(!data||size<5) return false;

    //默认按4字节长度前缀解析
    int pos=0;
    while(pos+4<=size){
        const uint32_t nalulen=(uint32_t(data[pos])<<24)|
                                (uint32_t(data[pos+1])<<16)|
                                (uint32_t(data[pos+2])<<8)|
                                uint32_t(data[pos+3]);
        pos+=4;
        if(nalulen==0) continue;
        if(pos+int(nalulen)>size) return false;

        const uint8_t nalType=data[pos]&0x1F;
        if(nalType==7){
            sps=QByteArray(reinterpret_cast<const char*>(data+pos),int(nalulen));
        }else if(nalType==8){
            pps=QByteArray(reinterpret_cast<const char*>(data+pos),int(nalulen));
        }else if(nalType==5){
            isKeyFrame=true;
        }
        //写AnnexB:00 00 00 01+NALU
        annexb.append("\x00\x00\x00\x01",4);
        annexb.append(reinterpret_cast<const char*>(data+pos),int(nalulen));

        pos+=int(nalulen);
    }
    return !annexb.isEmpty();
}
