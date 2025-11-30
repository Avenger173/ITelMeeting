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

bool AVReceiver::start(const QString &host, int videoPort,int audioPort)
{
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
    if(videoSocket){
        videoSocket->close();
        videoSocket->deleteLater();
        videoSocket=nullptr;
    }
    if(audioSocket){
        audioSocket->close();
        audioSocket->deleteLater();
        audioSocket=nullptr;
    }
}

void AVReceiver::onVideoReadyRead()
{
    while(videoSocket&&videoSocket->hasPendingDatagrams()){
        QByteArray data;
        data.resize(videoSocket->pendingDatagramSize());
        videoSocket->readDatagram(data.data(),data.size());
        processVideoPacket(data);
    }
}

void AVReceiver::onAudioReadyRead()
{
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
    if(es.isEmpty())
        return;

    const uint8_t* raw=reinterpret_cast<const uint8_t*>(es.constData());
    int rawSize=es.size();

    //1)从这一帧里尝试解析SPS/PPS和是否为关键帧
    QByteArray sps,pps;
    bool isKey=false;
    parseH264AnnexBForSpsPps(raw,rawSize,sps,pps,isKey);
    //2)如果还没视频配置，且这帧里带了SPS/PPS,就记下来
    if(!haveVConf){
        if(!sps.isEmpty()&&!pps.isEmpty()){
            sps_=sps;
            pps_=pps;
            haveVConf=true;

            qDebug()<<"[AVReceiver] 捕获到SPS/PPS,开始初始化解码器配置";
            //可以选择先喂一遍SPS/PPS
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
    pkt->data=const_cast<uint8_t*>(raw);
    pkt->size=rawSize;

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
        //00 00 01或00 00 00 01
        if(p[i]==0x00&&p[i+1]==0x00&&
            ((p[i+2]==0x00)||(p[i+2]==0x00&&p[i+3]==0x01))){
            if(p[i+2]==0x00){
                off=i+3;
            }else{
                off=i+4;
            }
            return i;
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
