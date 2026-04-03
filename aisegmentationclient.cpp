#include "aisegmentationclient.h"

#include <QBuffer>
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

AiSegmentationClient::AiSegmentationClient(QObject *parent)
    : QObject(parent)
    , networkManager(new QNetworkAccessManager(this))
{
}

void AiSegmentationClient::configure(bool enabled, const QString &serviceBaseUrlValue, int timeoutMsValue)
{
    clientEnabled = enabled;
    serviceBaseUrl = normalizedUrlBase(serviceBaseUrlValue);
    timeoutMs = timeoutMsValue > 0 ? timeoutMsValue : 1500;
}
//向 AI 分割服务发起图像分割网络请求
//校验→编码→发请求→超时保护→异步解析响应→信号通知结果
quint64 AiSegmentationClient::requestSegmentation(const QImage &image, bool returnSoftMask, double threshold)
{
    const quint64 requestId = ++latestIssuedRequestId;

    if (!clientEnabled || buildSegmentUrl().isEmpty()) {
        emit segmentationFailed(requestId, QStringLiteral("segmentation service disabled"));
        return requestId;
    }

    if (image.isNull()) {
        emit segmentationFailed(requestId, QStringLiteral("input image is null"));
        return requestId;
    }

    QByteArray encodedImage;
    {
        QBuffer buffer(&encodedImage);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) {
            emit segmentationFailed(requestId, QStringLiteral("failed to encode image as PNG"));
            return requestId;
        }
    }

    QJsonObject payload;
    payload[QStringLiteral("image_base64")] = QString::fromLatin1(encodedImage.toBase64());
    payload[QStringLiteral("request_id")] = QString::number(requestId);
    payload[QStringLiteral("return_soft_mask")] = returnSoftMask;
    if (threshold > 0.0) {
        payload[QStringLiteral("threshold")] = threshold;
    }

    QNetworkRequest request{QUrl(buildSegmentUrl())};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QNetworkReply *reply = networkManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    if (!reply) {
        emit segmentationFailed(requestId, QStringLiteral("failed to create network request"));
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
            emit segmentationDiscarded(requestId);
            reply->deleteLater();
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            emit segmentationFailed(
                requestId,
                timedOut
                    ? QStringLiteral("segmentation request timed out")
                    : reply->errorString());
            reply->deleteLater();
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            emit segmentationFailed(requestId, QStringLiteral("invalid segmentation response json"));
            reply->deleteLater();
            return;
        }

        const QJsonObject obj = doc.object();
        const QString maskBase64 = obj.value(QStringLiteral("mask_png_base64")).toString();
        if (maskBase64.isEmpty()) {
            emit segmentationFailed(requestId, QStringLiteral("segmentation response missing mask"));
            reply->deleteLater();
            return;
        }

        const QByteArray maskBytes = QByteArray::fromBase64(maskBase64.toLatin1());
        QImage mask;
        if (!mask.loadFromData(maskBytes, "PNG")) {
            emit segmentationFailed(requestId, QStringLiteral("failed to decode mask png"));
            reply->deleteLater();
            return;
        }

        const int latencyMs = obj.value(QStringLiteral("latency_ms")).toInt();
        const double foregroundRatio = obj.value(QStringLiteral("foreground_ratio")).toDouble();
        emit segmentationReady(requestId, mask, latencyMs, foregroundRatio);
        reply->deleteLater();
    });

    return requestId;
}

QString AiSegmentationClient::normalizedUrlBase(QString value) const
{
    value = value.trimmed();
    while (value.endsWith(QLatin1Char('/'))) {
        value.chop(1);
    }
    return value;
}

QString AiSegmentationClient::buildSegmentUrl() const
{
    const QString base = normalizedUrlBase(serviceBaseUrl);
    if (base.isEmpty()) {
        return QString();
    }
    if (base.endsWith(QStringLiteral("/segment"), Qt::CaseInsensitive)) {
        return base;
    }
    return QStringLiteral("%1/segment").arg(base);
}

QString AiSegmentationClient::serviceUrl() const
{
    return buildSegmentUrl();
}
