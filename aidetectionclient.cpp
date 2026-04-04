#include "aidetectionclient.h"

#include <QBuffer>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

AiDetectionClient::AiDetectionClient(QObject *parent)
    : QObject(parent)
    , networkManager(new QNetworkAccessManager(this))
{
    qRegisterMetaType<AiDetectionBox>("AiDetectionBox");
    qRegisterMetaType<AiDetectionList>("AiDetectionList");
}

void AiDetectionClient::configure(bool enabled, const QString &serviceBaseUrlValue, int timeoutMsValue)
{
    clientEnabled = enabled;
    serviceBaseUrl = normalizedUrlBase(serviceBaseUrlValue);
    timeoutMs = timeoutMsValue > 0 ? timeoutMsValue : 1500;
}

quint64 AiDetectionClient::requestDetection(const QImage &image,
                                            double confThreshold,
                                            double iouThreshold,
                                            int maxDetections)
{
    const quint64 requestId = ++latestIssuedRequestId;

    if (!clientEnabled || buildDetectUrl().isEmpty()) {
        emit detectionFailed(requestId, QStringLiteral("detection service disabled"));
        return requestId;
    }

    if (image.isNull()) {
        emit detectionFailed(requestId, QStringLiteral("input image is null"));
        return requestId;
    }

    QByteArray encodedImage;
    {
        QBuffer buffer(&encodedImage);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "JPG", 85)) {
            emit detectionFailed(requestId, QStringLiteral("failed to encode image as JPG"));
            return requestId;
        }
    }

    QJsonObject payload;
    payload[QStringLiteral("image_base64")] = QString::fromLatin1(encodedImage.toBase64());
    payload[QStringLiteral("request_id")] = QString::number(requestId);
    payload[QStringLiteral("max_detections")] = qBound(1, maxDetections, 100);
    if (confThreshold > 0.0) {
        payload[QStringLiteral("conf_threshold")] = confThreshold;
    }
    if (iouThreshold > 0.0) {
        payload[QStringLiteral("iou_threshold")] = iouThreshold;
    }

    QNetworkRequest request{QUrl(buildDetectUrl())};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QNetworkReply *reply = networkManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    if (!reply) {
        emit detectionFailed(requestId, QStringLiteral("failed to create network request"));
        return requestId;
    }

    reply->setProperty("requestId", QVariant::fromValue(requestId));

    auto *timeoutTimer = new QTimer(reply);
    timeoutTimer->setSingleShot(true);
    QObject::connect(timeoutTimer, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("timedOut", true);
        reply->abort();
    });
    timeoutTimer->start(timeoutMs);

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const quint64 requestId = reply->property("requestId").toULongLong();
        const bool timedOut = reply->property("timedOut").toBool();
        const QByteArray body = reply->readAll();

        if (requestId < latestIssuedRequestId) {
            emit detectionDiscarded(requestId);
            reply->deleteLater();
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            emit detectionFailed(
                requestId,
                timedOut
                    ? QStringLiteral("detection request timed out")
                    : reply->errorString());
            reply->deleteLater();
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            emit detectionFailed(requestId, QStringLiteral("invalid detection response json"));
            reply->deleteLater();
            return;
        }

        const QJsonObject obj = doc.object();
        const QJsonArray detectionsJson = obj.value(QStringLiteral("detections")).toArray();
        AiDetectionList detections;
        detections.reserve(detectionsJson.size());

        for (const QJsonValue &itemValue : detectionsJson) {
            const QJsonObject item = itemValue.toObject();
            AiDetectionBox box;
            box.classId = item.value(QStringLiteral("class_id")).toInt(-1);
            box.label = item.value(QStringLiteral("label")).toString();
            box.score = item.value(QStringLiteral("score")).toDouble();
            const int x1 = item.value(QStringLiteral("x1")).toInt();
            const int y1 = item.value(QStringLiteral("y1")).toInt();
            const int x2 = item.value(QStringLiteral("x2")).toInt();
            const int y2 = item.value(QStringLiteral("y2")).toInt();
            box.box = QRect(QPoint(x1, y1), QPoint(x2, y2)).normalized();
            detections.push_back(box);
        }

        const int latencyMs = obj.value(QStringLiteral("latency_ms")).toInt();
        const QSize imageSize(obj.value(QStringLiteral("width")).toInt(), obj.value(QStringLiteral("height")).toInt());
        emit detectionReady(requestId, detections, latencyMs, imageSize);
        reply->deleteLater();
    });

    return requestId;
}

QString AiDetectionClient::normalizedUrlBase(QString value) const
{
    value = value.trimmed();
    while (value.endsWith(QLatin1Char('/'))) {
        value.chop(1);
    }
    return value;
}

QString AiDetectionClient::buildDetectUrl() const
{
    const QString base = normalizedUrlBase(serviceBaseUrl);
    if (base.isEmpty()) {
        return QString();
    }
    if (base.endsWith(QStringLiteral("/detect"), Qt::CaseInsensitive)) {
        return base;
    }
    return QStringLiteral("%1/detect").arg(base);
}

QString AiDetectionClient::serviceUrl() const
{
    return buildDetectUrl();
}
