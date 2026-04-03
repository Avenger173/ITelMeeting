#ifndef AISEGMENTATIONCLIENT_H
#define AISEGMENTATIONCLIENT_H

#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
//AI 图像分割的网络客户端，专门和后端 AI 服务通信
class AiSegmentationClient : public QObject
{
    Q_OBJECT

public:
    explicit AiSegmentationClient(QObject *parent = nullptr);
    //配置客户端（启用开关、AI 服务地址、请求超时时间）
    void configure(bool enabled, const QString &serviceBaseUrl, int timeoutMs);
    //传入待分割图片，发起 AI 分割请求，返回唯一请求 ID
    quint64 requestSegmentation(const QImage &image,
                                bool returnSoftMask = true,
                                double threshold = -1.0);

    bool isEnabled() const { return clientEnabled; }
    QString serviceUrl() const;
    int requestTimeoutMs() const { return timeoutMs; }

signals:
    //分割成功，返回请求 ID、分割掩码图、耗时、前景比例。
    void segmentationReady(quint64 requestId, const QImage &mask, int latencyMs, double foregroundRatio);
    //请求被丢弃
    void segmentationDiscarded(quint64 requestId);
    //请求被丢弃
    void segmentationFailed(quint64 requestId, const QString &reason);

private:
    QString normalizedUrlBase(QString value) const;
    QString buildSegmentUrl() const;

private:
    QNetworkAccessManager *networkManager = nullptr;
    bool clientEnabled = false;
    QString serviceBaseUrl = QStringLiteral("http://127.0.0.1:18080");
    int timeoutMs = 1500;
    quint64 latestIssuedRequestId = 0;
};

#endif // AISEGMENTATIONCLIENT_H
