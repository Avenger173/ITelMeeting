#ifndef VIDEOCAPTURE_H
#define VIDEOCAPTURE_H

#include <QObject>
#include <QThread>
#include <QImage>
#include <QColor>
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
    QStringList availableCameras(); //枚举系统中所有可用的摄像头设备，返回设备列表
    bool reopen(int deviceIndex);   //关闭当前打开的摄像头，重新打开指定索引的摄像头
    QSize frameSize() const;

signals:
    // 发送 RGB888 图像帧
    void frameCaptured(const QImage &frame);
    void segmentationFrameReady(const QImage &frame);

public slots:
    void captureLoop();
    void setCaptureMode(int mode); // 0: camera, 1: screen
    void setShareTarget(int screenIndex, quint64 windowId); // 共享目标: 屏幕/窗口
    void setTargetFps(int fps); // 输出节流帧率
    void setShareMaxSize(int maxW, int maxH); // 共享上限分辨率
    void setBeautyEnabled(bool on);
    void setBeautyLevel(int level); // 0~100
    void setBeautyStyle(int style); // 0关 1自然 2清晰 3柔和 4磨皮 5瘦脸 6祛皱
    void setVirtualBackgroundEnabled(bool on);
    void setVirtualBackgroundMode(const QString &mode); // off/blur/color
    void setVirtualBackgroundColor(const QColor &color); // 纯色背景颜色
    void setVirtualBackgroundImage(const QImage &image); // 图片背景
    void setVirtualBackgroundBlurStrength(int level); // 0~100
    void setVirtualBackgroundRequestInterval(int interval); // 每 N 帧请求一次分割
    void updateVirtualBackgroundMask(const QImage &mask);
    void clearVirtualBackgroundMask();

private:
    bool ensureFaceCascadeLoaded(); //确保人脸检测的级联分类器（CascadeClassifier）已加载
    std::vector<cv::Rect> detectFaces(const cv::Mat &frame);//对输入帧进行人脸检测，返回所有人脸的位置，std::vector<cv::Rect>，OpenCV 的矩形向量，存储检测到的所有人脸区域
    void applyBeautyFilter(cv::Mat &frame, int style, int level);//对输入帧应用指定风格和等级的美颜滤镜

    std::atomic_bool running;
    cv::VideoCapture cap;
    QMutex mutex;
    QImage lastFrame;   //截屏图片
    int currentDeviceIndex = 0;

    CaptureMode captureMode = CaptureMode::Camera;
    int shareScreenIndex = 0;
    quint64 shareWindowId = 0;
    int targetFps = 30;
    int shareMaxWidth = 1920;
    int shareMaxHeight = 1080;

    bool beautyEnabled = false;
    int beautyLevel = 0;
    int beautyStyle = 0;
    bool virtualBackgroundEnabled = false;
    QString virtualBackgroundMode = QStringLiteral("off");
    QColor virtualBackgroundColor = QColor(QStringLiteral("#ddebff"));
    QImage virtualBackgroundImage;
    int virtualBackgroundBlurStrength = 45;
    int virtualBackgroundRequestInterval = 4;
    int segmentationRequestTick = 0;
    QImage latestVirtualBackgroundMask;

    cv::CascadeClassifier faceCascade;  //OpenCV 的级联分类器，用于人脸检测（加载 Haar/LBP 分类器模型）。
    bool faceCascadeTried = false;  //标记是否已经尝试加载人脸分类器
    bool faceCascadeLoaded = false; //标记人脸分类器是否成功加载
    int faceDetectTick = 0; //人脸检测的计时 / 计数变量（例如每隔 N 帧检测一次人脸，避免每帧检测消耗性能）。
    std::vector<cv::Rect> cachedFaces;  //缓存的人脸检测结果（避免每帧都检测，提升性能）。
};

#endif // VIDEOCAPTURE_H
