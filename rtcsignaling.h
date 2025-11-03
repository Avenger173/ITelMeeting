#ifndef RTCSIGNALING_H
#define RTCSIGNALING_H

#include <QObject>
#include<QtWebSockets/QWebSocket>
#include<QJsonObject>
#include<QJsonDocument>

class RtcSignaling : public QObject
{
    Q_OBJECT
public:
    explicit RtcSignaling(QObject *parent = nullptr);
    bool connectTo(const QUrl&url);
    void close();

    //业务：发送三类信息
    void sendOffer(const QJsonObject& sdp);
    void sendAnswer(const QJsonObject& sdp);
    void sendIce(const QJsonObject& ice);
signals:
    //这仨信号交给RtcEngine 去处理
    void remoteOffer(const QJsonObject& sdp);
    void remoteAnswer(const QJsonObject& sdp);
    void remoteIce(const QJsonObject& ice);
    void log(const QString& line);
    void connected();
    void disconnected();
private slots:
    void onConnected();
    void onTextMessageReceived(const QString& msg);
private:
    QWebSocket ws_;
};

#endif // RTCSIGNALING_H
