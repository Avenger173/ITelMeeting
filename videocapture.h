#ifndef VIDEOCAPTURE_H
#define VIDEOCAPTURE_H

#include <QObject>
#include <QThread>
#include <QImage>
#include <QMutex>
#include <QTimer>
#include <atomic>
#include <QDateTime>
#include <opencv2/opencv.hpp>

class VideoCapture : public QObject
{
    Q_OBJECT
public:
    enum class CaptureMode { Camera = 0, Screen = 1 };

    explicit VideoCapture(QObject *parent = nullptr);
    ~VideoCapture();
    bool open(int deviceIndex);
    void stop();
    void capturePhoto(const QString &path);
    QStringList availableCameras();
    bool reopen(int deviceIndex);
    QSize frameSize() const;

signals:
    // 发送 RGB888 图像帧
    void frameCaptured(const QImage &frame);

public slots:
    void captureLoop();
    void setCaptureMode(int mode); // 0: camera, 1: screen
    void setShareTarget(int screenIndex, quint64 windowId); // 共享目标: 屏幕/窗口
    void setBeautyEnabled(bool on);
    void setBeautyLevel(int level); // 0~100
    void setBeautyStyle(int style); // 0关 1自然 2清晰 3柔和 4磨皮 5瘦脸 6祛皱

private:
    bool ensureFaceCascadeLoaded();
    std::vector<cv::Rect> detectFaces(const cv::Mat &frame);
    void applyBeautyFilter(cv::Mat &frame, int style, int level);

    std::atomic_bool running;
    cv::VideoCapture cap;
    QMutex mutex;
    QImage lastFrame;
    int currentDeviceIndex = 0;

    CaptureMode captureMode = CaptureMode::Camera;
    int shareScreenIndex = 0;
    quint64 shareWindowId = 0;

    bool beautyEnabled = false;
    int beautyLevel = 0;
    int beautyStyle = 0;

    cv::CascadeClassifier faceCascade;
    bool faceCascadeTried = false;
    bool faceCascadeLoaded = false;
    int faceDetectTick = 0;
    std::vector<cv::Rect> cachedFaces;
};

#endif // VIDEOCAPTURE_H
