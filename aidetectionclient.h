#ifndef AIDETECTIONCLIENT_H
#define AIDETECTIONCLIENT_H

#include <QImage>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QRect>
#include <QString>
#include <QSize>

struct AiDetectionBox
{
    int classId = -1;
    QString label;
    double score = 0.0;
    QRect box;
};

using AiDetectionList = QList<AiDetectionBox>;

Q_DECLARE_METATYPE(AiDetectionBox)
Q_DECLARE_METATYPE(AiDetectionList)

class AiDetectionClient : public QObject
{
    Q_OBJECT

public:
    explicit AiDetectionClient(QObject *parent = nullptr);

    void configure(bool enabled, const QString &serviceBaseUrl, int timeoutMs);
    quint64 requestDetection(const QImage &image,
                             double confThreshold = -1.0,
                             double iouThreshold = -1.0,
                             int maxDetections = 20);

    bool isEnabled() const { return clientEnabled; }
    QString serviceUrl() const;
    int requestTimeoutMs() const { return timeoutMs; }

signals:
    void detectionReady(quint64 requestId, const AiDetectionList &detections, int latencyMs, const QSize &imageSize);
    void detectionDiscarded(quint64 requestId);
    void detectionFailed(quint64 requestId, const QString &reason);

private:
    QString normalizedUrlBase(QString value) const;
    QString buildDetectUrl() const;

private:
    QNetworkAccessManager *networkManager = nullptr;
    bool clientEnabled = false;
    QString serviceBaseUrl = QStringLiteral("http://127.0.0.1:18080");
    int timeoutMs = 1500;
    quint64 latestIssuedRequestId = 0;
};

#endif // AIDETECTIONCLIENT_H
