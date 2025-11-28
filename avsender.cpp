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
        qWarning()<<"[AVSender] 已经启动";
        return false;
    }
    m_socket=new QUdpSocket(this);
    m_destAddr=QHostAddress(host);
    m_videoPort=videoPort;
    m_audioPort=audioPort;

    //绑定一个本地端口用于发送
    if(!m_socket->bind(QHostAddress::AnyIPv4,0,QUdpSocket::ShareAddress|QUdpSocket::ReuseAddressHint)){
        qWarning()<<"[AVSender] UDP绑定失败";
        return false;
    }
    qInfo()<<"[AVSender] 已启动推流,目标:"<<host<<":"<<videoPort<<","<<audioPort;
    return true;
}

void AVSender::stop()
{
    if(m_socket){
        m_socket->close();
        delete m_socket;
        m_socket=nullptr;
    }
}

void AVSender::sendEncodedVideo(const QByteArray &encodePkt, quint32 pts_ms)
{
    //编码数据包需要附加头部信息
    QByteArray datagram;
    QDataStream stream(&datagram,QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);

    quint32 frameId=++m_nextVideoFrameId;
    quint16 chunkId=0,chunkCount=1;
    quint32 totalSize=encodePkt.size();

    //包装数据
    stream<<frameId<<chunkId<<chunkCount<<pts_ms<<totalSize;
    datagram.append(encodePkt);

    sendPacketInternal(datagram,m_videoPort);
}

void AVSender::sendEncodedAudio(const QByteArray &encodePkt, quint32 pts_ms)
{
    //编码数据包需要附加头部信息
    QByteArray datagram;
    QDataStream stream(&datagram,QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);

    quint32 frameId=++m_nextAudioFrameId;
    quint16 chunkId=0,chunkCount=1;
    quint32 totalSize=encodePkt.size();

    //包装数据
    stream<<frameId<<chunkId<<chunkCount<<pts_ms<<totalSize;
    datagram.append(encodePkt);

    sendPacketInternal(datagram,m_audioPort);

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
        qWarning()<<"[AVSender]数据发送失败:"<<m_socket->errorString();
    }
}

