#include "rtcsignaling.h"

RtcSignaling::RtcSignaling(QObject *parent)
    : QObject{parent}
{}

bool RtcSignaling::connectTo(const QUrl &url)
{
    QObject::connect(&ws_,&QWebSocket::connect,this,&RtcSignaling::onConnected);
    QObject::connect(&ws_,&QWebSocket::disconnect,this,&RtcSignaling::disconnected);
    QObject::connect(&ws_,&QWebSocket::textMessageReceived,this,&RtcSignaling::onTextMessageReceived);
    ws_.open(url);
    return true;
}

void RtcSignaling::close()
{
    ws_.close();
}

void RtcSignaling::sendOffer(const QJsonObject &sdp)
{
    QJsonObject o=sdp;
    o["type"]="offer";
    send(ws_,o);
}

void RtcSignaling::sendAnswer(const QJsonObject &sdp)
{
    QJsonObject o=sdp;
    o["type"]="answer";
    send(ws_,o);
}

void RtcSignaling::sendIce(const QJsonObject &ice)
{
    QJsonObject o=sdp;
    o["type"]="ice";
    send(ws_,o);
}

void RtcSignaling::onConnected()
{
    emit connected();
    emit log("[Signaling] connected");
}

void RtcSignaling::onTextMessageReceived(const QString &msg)
{
    const auto obj=QJsonDocument::fromJson(msg.toUtf8()).object();
    const auto type=obj.value("type").toString();
    if(type=="offer") emit remoteAnswer(obj);
    else if(type=="answer") emit remoteAnswer(obj);
    else if(type=="ice") emit remoteIce(obj);
    else emit log("[Signaling] unknown:"+type);
}

static inline void send(QWebSocket& ws,const QJsonObject& obj){
    ws.sendTextMessage(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

