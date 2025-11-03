#ifndef VIDEOCAPTURE_H
#define VIDEOCAPTURE_H

#include <QObject>
#include <QThread>
#include <QImage>
#include <QMutex>
#include <QTimer>
#include <atomic>
#include <opencv2/opencv.hpp>

class VideoCapture : public QObject
{
    Q_OBJECT
public:
    explicit VideoCapture(QObject *parent = nullptr);
    ~VideoCapture();
    bool open(int deviceIndex);
    void stop();
    void capturePhoto(const QString &path);
    QStringList availableCameras();
    bool reopen(int deviceIndex);
    QSize frameSize() const;

signals:
    void frameCaptured(const QImage &frame);  // 发射图像帧信号
public slots:
    void captureLoop();

private:
    std::atomic_bool running;
    cv::VideoCapture cap;
    QMutex mutex;
    QImage lastFrame;
    int currentDeviceIndex = 0;

};

#endif // VIDEOCAPTURE_H
