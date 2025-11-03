#include "avsender.h"
#include<QDataStream>
#include<QThread>
#include<QDebug>

AVSender::AVSender(QObject *parent)
    : QObject{parent}
{}

AVSender::~AVSender()
{
    stop();
}

bool AVSender::start(const QString &host, quint16 videoPort, quint16 audioPort)
{
    if(m_socket){
        emit warnMessage("[AVSender] 已经启动,忽略重复调用");
        return false;
    }
    m_socket=new QUdpSocket(this);
    m_destAddr=QHostAddress(host);
    m_videoPort=videoPort;
    m_audioPort=audioPort;

    //绑定一个本地端口用于发送
    if(!m_socket->bind(QHostAddress::AnyIPv4,0,QUdpSocket::ShareAddress|QUdpSocket::ReuseAddressHint)){
        emit warnMessage("[AVSender] bind failed,cannot send UDP packets");
        return false;
    }
    emit infoMessage(QStringLiteral("AVSender startd->%1(v:%2,a:%3)")
                         .arg(host).arg(videoPort).arg(audioPort));
    return true;
}

void AVSender::stop()
{
    if(m_socket){
        m_socket->close();
        m_socket->deleteLater();
        m_socket=nullptr;
    }
    emit infoMessage("[AVSender] stopped");
}

void AVSender::sendEncodedVideo(const QByteArray &encodePkt, quint32 pts_ms)
{
    //防呆:大包≈原始帧，禁止发送
    if(encodePkt.size()>200*1024){
        emit warnMessage(QStringLiteral("[AVSender] video payload looks like RAM(size=%1)-expected H.264.Drop.").arg(encodePkt.size()));
        return;
    }
    //防呆:不是Annex起始码(00 00 00 01),很可能不是直解的H.264
    if(!(encodePkt.size()>=4&&
          (uchar)encodePkt[0]==0x00&&(uchar)encodePkt[1]==0x00&&
          (uchar)encodePkt[2]==0x00&&(uchar)encodePkt[3]==0x01)){
        emit warnMessage("[AVSender] video payload not AnnexB (no 0x00000001).Likely AVCC or RAW.Drop.");
        return;
    }

    quint32 frameId=0;
    {
        QMutexLocker locker(&m_mutex);
        frameId=++m_nextVideoFrameId;
    }
    packetizeAndSend(encodePkt,frameId,pts_ms,m_videoPort);
}

void AVSender::sendEncodedAudio(const QByteArray &encodePkt, quint32 pts_ms)
{
    //防呆:大包≈原始帧，禁止发送
    if(encodePkt.size()>32*1024){
        emit warnMessage(QStringLiteral("[AVSender] audio payload too large (size=%1). Expected AAC frame. Drop.").arg(encodePkt.size()));
        return;
    }

    quint32 frameId=0;
    {
        QMutexLocker locker(&m_mutex);
        frameId=++m_nextAudioFrameId;
    }
    packetizeAndSend(encodePkt,frameId,pts_ms,m_audioPort);

}

void AVSender::sendAVPacket(const AVPacket *pkt, AVRational time_base, bool isVideo)
{
    if(!pkt||pkt->size<=0||!pkt->data) return;

    //计算pts_ms
    quint32 pts_ms=0;
    if(pkt->pts!=AV_NOPTS_VALUE){
        //将pkt->pts转换为毫秒
        pts_ms=static_cast<quint32>(av_rescale_q(pkt->pts,time_base,AVRational{1,1000}));
    }
    QByteArray ba(reinterpret_cast<const char*>(pkt->data),pkt->size);
    if(isVideo) sendEncodedVideo(ba,pts_ms);
    else sendEncodedAudio(ba,pts_ms);
}

void AVSender::sendPacketInternal(const QByteArray &datagram, quint16 port)
{
    if(!m_socket)
    {
        qWarning()<<"[AVSender] socket is null";
        return;
    }
    qint64 written=m_socket->writeDatagram(datagram,m_destAddr,port);
    qDebug()<<"[AVSender] writeDatagram->size="<<datagram.size()<<"sent="<<written<<"to"<<m_destAddr.toString()<<":"<<port;
    if(written==-1){
        emit warnMessage(QStringLiteral("[AVSender] writeDatagram failed: %1").arg(m_socket->errorString()));
    }
}

void AVSender::packetizeAndSend(const QByteArray &encodePkt, quint32 frameId, quint32 pts_ms, quint16 port)
{
    if(encodePkt.isEmpty()) return;
    const int totalSize=encodePkt.size();
    const int chunkPayload=CHUNK_SIZE;
    const int chunkCount=(totalSize+chunkPayload-1)/chunkPayload;

    int offset=0;
    for(int i=0;i<chunkCount;++i){
        const int remain=totalSize-offset;
        const int thisSize=qMin(remain,chunkPayload);

        QByteArray datagram(HEADER_SIZE+thisSize,0);
        QDataStream ds(&datagram,QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::BigEndian);

        quint32 fid=frameId;
        quint16 chunkId=static_cast<quint16>(i);
        quint16 ccount=static_cast<quint16>(chunkCount);
        quint32 pts=pts_ms;
        quint32 total=static_cast<quint32>(totalSize);

        ds<<fid;
        ds<<chunkId;
        ds<<ccount;
        ds<<pts;
        ds<<total;

        memcpy(datagram.data()+HEADER_SIZE,encodePkt.constData()+offset,thisSize);

        if(QThread::currentThread()!=this->thread()){
            QMetaObject::invokeMethod(this,"sendPacketInternal",Qt::QueuedConnection,Q_ARG(QByteArray,datagram),Q_ARG(quint16,port));
        }else{
            sendPacketInternal(datagram,port);
        }
        offset+=thisSize;
    }
}
