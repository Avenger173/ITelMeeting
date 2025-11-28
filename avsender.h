#ifndef AVSENDER_H
#define AVSENDER_H

#include <QObject>
#include<QUdpSocket>
#include<QHostAddress>
#include<QMutex>
#include<QByteArray>

extern "C"{
#include<libavcodec/avcodec.h>
#include<libavutil/rational.h>
#include<libavutil/avutil.h>
}
class AVSender : public QObject
{
    Q_OBJECT
public:
    explicit AVSender(QObject *parent = nullptr);
    ~AVSender() override;
    //启动发送端
    bool start(const QString &host,quint16 videoPort=12345,quint16 audioPort=12346);
    void stop();

    Q_INVOKABLE void sendEncodedVideo(const QByteArray &encodePkt,quint32 pts_ms=0);
    Q_INVOKABLE void sendEncodedAudio(const QByteArray &encodePkt,quint32 pts_ms=0);


private:
    void sendPacketInternal(const QByteArray &datagram,quint16 port);
    QUdpSocket*m_socket=nullptr;
    QHostAddress m_destAddr;
    quint16 m_videoPort=12345;
    quint16 m_audioPort=12346;
    QMutex m_mutex;//保护帧id

    quint32 m_nextVideoFrameId=0;
    quint32 m_nextAudioFrameId=0;

};

#endif // AVSENDER_H
