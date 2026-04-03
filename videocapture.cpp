#include "videocapture.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QList>
#include <QPixmap>
#include <QScreen>

#include <algorithm>
#include <cmath>

namespace {

// 工具函数：将矩形裁剪到图像边界内，避免越界访问，接收一个待裁剪的矩形 r 和图像的尺寸 s（宽高），返回一个被限制在图像边界内的新矩形
static cv::Rect clipRect(const cv::Rect &r, const cv::Size &s)
{   //r & 边界矩形：计算待裁剪矩形 r 和图像有效区域的交集，结果就是被裁剪后、完全在图像边界内的矩形
    //cv::Rect(0, 0, s.width, s.height)：构造一个从图像左上角（0,0）开始，宽高等于图像尺寸的 “边界矩形”，代表图像的有效区域
    return r & cv::Rect(0, 0, s.width, s.height);
}

static bool bgrMatToRgbImage(const cv::Mat &frame, QImage *out)
{
    if (!out || frame.empty() || frame.type() != CV_8UC3) return false;

    QImage img(frame.cols, frame.rows, QImage::Format_RGB888);
    if (img.isNull()) return false;

    for (int y = 0; y < frame.rows; ++y) {
        const uchar *src = frame.ptr<uchar>(y);
        uchar *dst = img.scanLine(y);
        for (int x = 0; x < frame.cols; ++x) {
            dst[3 * x + 0] = src[3 * x + 2];
            dst[3 * x + 1] = src[3 * x + 1];
            dst[3 * x + 2] = src[3 * x + 0];
        }
    }

    *out = img;
    return true;
}

static bool buildVirtualBackgroundSoftMask(const cv::Size &frameSize, const QImage &maskImage, cv::Mat *mask3Out)
{
    if (!mask3Out || frameSize.width <= 0 || frameSize.height <= 0 || maskImage.isNull()) return false;

    QImage grayMask = maskImage.convertToFormat(QImage::Format_Grayscale8);
    if (grayMask.isNull()) return false;

    cv::Mat mask(grayMask.height(),
                 grayMask.width(),
                 CV_8UC1,
                 const_cast<uchar *>(grayMask.constBits()),
                 grayMask.bytesPerLine());

    cv::Mat resizedMask;
    cv::Mat maskView = mask;
    if (maskView.size() != frameSize) {
        cv::resize(maskView, resizedMask, frameSize, 0, 0, cv::INTER_LINEAR);
        maskView = resizedMask;
    }

    cv::Mat maskFloat;
    maskView.convertTo(maskFloat, CV_32FC1, 1.0 / 255.0);
    cv::GaussianBlur(maskFloat, maskFloat, cv::Size(0, 0), 2.2);
    cv::pow(maskFloat, 0.75, maskFloat);

    cv::Mat mask3;
    std::vector<cv::Mat> channels(3, maskFloat);
    cv::merge(channels, mask3);
    *mask3Out = mask3;
    return true;
}

static void applyVirtualBackgroundBlur(cv::Mat &frame, const QImage &maskImage, int strength)
{
    if (frame.empty() || maskImage.isNull() || frame.type() != CV_8UC3) return;

    const double t = std::clamp(strength / 100.0, 0.0, 1.0);
    if (t <= 0.0) return;

    cv::Mat mask3;
    if (!buildVirtualBackgroundSoftMask(frame.size(), maskImage, &mask3)) return;

    cv::Mat blurred;
    // 性能优化：先对低分辨率帧做模糊，再放大回原尺寸。
    // 对虚拟背景来说，背景本身不需要保留高频细节，这样可以显著降低 CPU 占用。
    const double blurScale = std::clamp(0.58 - 0.28 * t, 0.28, 0.58);
    const int blurW = std::max(1, static_cast<int>(std::lround(frame.cols * blurScale)));
    const int blurH = std::max(1, static_cast<int>(std::lround(frame.rows * blurScale)));
    if (blurW < frame.cols || blurH < frame.rows) {
        cv::Mat small;
        cv::resize(frame, small, cv::Size(blurW, blurH), 0, 0, cv::INTER_LINEAR);
        const double sigma = 1.8 + 5.2 * t;
        cv::GaussianBlur(small, small, cv::Size(0, 0), sigma, sigma);
        cv::resize(small, blurred, frame.size(), 0, 0, cv::INTER_LINEAR);
    } else {
        const double sigma = 6.0 + 16.0 * t;
        cv::GaussianBlur(frame, blurred, cv::Size(0, 0), sigma, sigma);
    }

    cv::Mat srcF, blurF;
    frame.convertTo(srcF, CV_32FC3);
    blurred.convertTo(blurF, CV_32FC3);
    const cv::Mat inv = cv::Scalar::all(1.0) - mask3;
    cv::Mat mixed = srcF.mul(mask3) + blurF.mul(inv);
    mixed.convertTo(frame, CV_8UC3);
}

static void applyVirtualBackgroundColor(cv::Mat &frame, const QImage &maskImage, const QColor &color)
{
    if (frame.empty() || maskImage.isNull() || frame.type() != CV_8UC3 || !color.isValid()) return;

    cv::Mat mask3;
    if (!buildVirtualBackgroundSoftMask(frame.size(), maskImage, &mask3)) return;

    cv::Mat srcF;
    frame.convertTo(srcF, CV_32FC3);

    cv::Mat solid(frame.rows, frame.cols, CV_32FC3,
                  cv::Scalar(static_cast<float>(color.blue()),
                             static_cast<float>(color.green()),
                             static_cast<float>(color.red())));

    const cv::Mat inv = cv::Scalar::all(1.0) - mask3;
    cv::Mat mixed = srcF.mul(mask3) + solid.mul(inv);
    mixed.convertTo(frame, CV_8UC3);
}

static bool buildBackgroundImageMat(const QImage &backgroundImage, const cv::Size &frameSize, cv::Mat *out)
{
    if (!out || backgroundImage.isNull() || frameSize.width <= 0 || frameSize.height <= 0) return false;

    QImage rgbImage = backgroundImage.convertToFormat(QImage::Format_RGB888);
    if (rgbImage.isNull()) return false;

    QImage fitted = rgbImage.scaled(frameSize.width,
                                    frameSize.height,
                                    Qt::KeepAspectRatioByExpanding,
                                    Qt::SmoothTransformation);
    if (fitted.isNull()) return false;

    const int x = std::max(0, (fitted.width() - frameSize.width) / 2);
    const int y = std::max(0, (fitted.height() - frameSize.height) / 2);
    fitted = fitted.copy(x, y, frameSize.width, frameSize.height);
    if (fitted.isNull()) return false;

    cv::Mat rgb(frameSize.height,
                frameSize.width,
                CV_8UC3,
                const_cast<uchar *>(fitted.constBits()),
                fitted.bytesPerLine());
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    *out = bgr;
    return true;
}

static void applyVirtualBackgroundImage(cv::Mat &frame, const QImage &maskImage, const QImage &backgroundImage)
{
    if (frame.empty() || maskImage.isNull() || frame.type() != CV_8UC3 || backgroundImage.isNull()) return;

    cv::Mat mask3;
    if (!buildVirtualBackgroundSoftMask(frame.size(), maskImage, &mask3)) return;

    cv::Mat background;
    if (!buildBackgroundImageMat(backgroundImage, frame.size(), &background)) return;

    cv::Mat srcF, bgF;
    frame.convertTo(srcF, CV_32FC3);
    background.convertTo(bgF, CV_32FC3);

    const cv::Mat inv = cv::Scalar::all(1.0) - mask3;
    cv::Mat mixed = srcF.mul(mask3) + bgF.mul(inv);
    mixed.convertTo(frame, CV_8UC3);
}

// 工具函数：按比例扩展人脸区域，并裁剪到合法范围
static cv::Rect expandFaceRect(const cv::Rect &r, const cv::Size &size, double sx, double sy)
{   // 1. 计算原始人脸框的中心点坐标 (cx, cy)
    const int cx = r.x + r.width / 2;   // 中心点x坐标 = 框左上角x + 框宽度的一半
    const int cy = r.y + r.height / 2;
    // 2. 按比例计算扩展后的宽高，且保证最小为2像素（避免宽高为0的无效框）
    const int w = std::max(2, static_cast<int>(r.width * sx));  // 新宽度 = 原宽度*水平扩展比例，最小2
    const int h = std::max(2, static_cast<int>(r.height * sy));
    // 3. 以原始中心点为中心，生成扩展后的新矩形框
    const cv::Rect rr(cx - w / 2, cy - h / 2, w, h);    // 新框左上角 = 中心点 - 新宽/高的一半
    // 4. 裁剪新框到图像合法范围
    return clipRect(rr, size);
}

// 工具函数：对人脸 ROI 做横向压缩，实现瘦脸几何形变
static void applySlimFaceWarp(cv::Mat &img, const std::vector<cv::Rect> &faces, double strength01)
{
    if (img.empty() || faces.empty()) return;

    const double s = std::clamp(strength01, 0.0, 1.0);
    if (s <= 0.0) return;

    auto smoothStep = [](double edge0, double edge1, double x) {
        if (edge1 <= edge0) return 0.0;
        double t = (x - edge0) / (edge1 - edge0);
        t = std::clamp(t, 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    };
    for (const auto &f : faces) {
        // 仅覆盖脸部主体，尽量不碰头发区域
        cv::Rect roi = expandFaceRect(f, img.size(), 1.18, 1.06);
        if (roi.width < 32 || roi.height < 32) continue;

        cv::Mat src = img(roi).clone();
        const int w = src.cols;
        const int h = src.rows;

        cv::Mat mapX(h, w, CV_32FC1);
        cv::Mat mapY(h, w, CV_32FC1);

        // 基于脸框构造“仅下半脸生效”的水平 inward 位移场
        const double fx = static_cast<double>(f.x - roi.x);
        const double fy = static_cast<double>(f.y - roi.y);
        const double fw = static_cast<double>(f.width);
        const double fh = static_cast<double>(f.height);
        const double cx = fx + fw * 0.5;
        const double maxShift = (0.02 + 0.06 * s) * fw; // 更稳，避免眼区/鼻区畸变

        for (int y = 0; y < h; ++y) {
            float *mx = mapX.ptr<float>(y);
            float *my = mapY.ptr<float>(y);
            for (int x = 0; x < w; ++x) {
                const double rx = (static_cast<double>(x) - cx) / std::max(1.0, fw * 0.5); // [-1,1] around face center
                const double absRx = std::abs(rx);
                const double fyn = (static_cast<double>(y) - fy) / std::max(1.0, fh);

                // 垂直权重：从鼻翼以下开始，峰值在脸颊/下颌，眼睛区域基本为0
                const double topGate = smoothStep(0.50, 0.68, fyn);
                const double bottomGate = 1.0 - smoothStep(0.95, 1.06, fyn);
                const double wy = topGate * bottomGate;

                // 水平权重：中心(鼻梁/眼间)与最外边缘都保护，只作用脸颊带
                const double cheekBand = smoothStep(0.38, 0.86, absRx);
                const double edgeFade = 1.0 - smoothStep(0.98, 1.06, absRx);
                const double wx = cheekBand * edgeFade;

                const double amp = maxShift * wy * wx;
                const double sign = (rx >= 0.0) ? 1.0 : -1.0;
                const double sx = static_cast<double>(x) + sign * amp; // inverse-map: inward shift
                mx[x] = static_cast<float>(std::clamp(sx, 0.0, static_cast<double>(w - 1)));
                my[x] = static_cast<float>(y);
            }
        }

        cv::Mat warped;
        cv::remap(src, warped, mapX, mapY, cv::INTER_LINEAR, cv::BORDER_REFLECT_101);
        // 直接回写，避免“与原图二次混合”导致的旧轮廓残影
        warped.copyTo(img(roi));
    }
}

} // namespace

VideoCapture::VideoCapture(QObject *parent)
    : QObject{parent}, running(false)
{}

VideoCapture::~VideoCapture()
{
    stop();
}

//  打开摄像头设备并进入摄像头采集模式
bool VideoCapture::open(int deviceIndex)
{
    currentDeviceIndex = deviceIndex;
    captureMode = CaptureMode::Camera;

    QMutexLocker locker(&mutex);
    if (cap.isOpened()) cap.release();
    if (!cap.open(deviceIndex)) return false;
    running = true;
    return true;
}

//  停止采集循环并释放摄像头句柄
void VideoCapture::stop()
{
    QMutexLocker locker(&mutex);
    running = false;
    if (cap.isOpened()) cap.release();
}

//  保存最近一帧图像到指定路径
void VideoCapture::capturePhoto(const QString &path)
{
    QMutexLocker locker(&mutex);
    if (!lastFrame.isNull()) {
        lastFrame.save(path);
    }
}

// 在设备切换时重开摄像头
bool VideoCapture::reopen(int deviceIndex)
{
    QMutexLocker locker(&mutex);
    if (deviceIndex == currentDeviceIndex) return true;
    cap.release();
    if (!cap.open(deviceIndex)) return false;
    currentDeviceIndex = deviceIndex;
    return true;
}

// 获取当前摄像头分辨率，失败时返回默认值
QSize VideoCapture::frameSize() const
{
    int w = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    int h = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    if (w <= 0 || h <= 0) return QSize(640, 480);
    return QSize(w, h);
}

// 采集主循环：处理摄像头/屏幕采集、节流与美颜
void VideoCapture::captureLoop()
{
    // 输出节流（默认 30fps，可动态调整）
    qint64 lastSend = 0;    //初始化 lastSend 记录上一次发送帧的时间戳，用于帧率控制

    while (running) {
        QImage img;
        CaptureMode mode = CaptureMode::Camera;

        int fpsHint = 30;
        int shareMaxW = 1920;
        int shareMaxH = 1080;
        bool beautyOn = false;
        int beautyLv = 0;
        int beautySty = 0;
        bool virtualBgOn = false;
        QString virtualBgMode = QStringLiteral("off");
        QColor virtualBgColor = QColor(QStringLiteral("#ddebff"));
        QImage virtualBgImage;
        int virtualBgBlurStrength = 45;
        int virtualBgRequestInterval = 4;
        QImage virtualBgMask;

        {
            QMutexLocker locker(&mutex);
            mode = captureMode;
            fpsHint = std::clamp(targetFps, 8, 30);//std::clamp 限制目标帧率在 8-30fps 之间，避免帧率设置不合理
            shareMaxW = std::max(640, shareMaxWidth);
            shareMaxH = std::max(360, shareMaxHeight);
            beautyOn = beautyEnabled;
            beautyLv = beautyLevel;
            beautySty = beautyStyle;
            virtualBgOn = virtualBackgroundEnabled;
            virtualBgMode = virtualBackgroundMode;
            virtualBgColor = virtualBackgroundColor;
            virtualBgImage = virtualBackgroundImage;
            virtualBgBlurStrength = virtualBackgroundBlurStrength;
            virtualBgRequestInterval = std::max(1, virtualBackgroundRequestInterval);
            virtualBgMask = latestVirtualBackgroundMask;
        }

         //动态帧率节流（核心优化点）
        int effectiveFps = fpsHint;
        // 美颜会消耗CPU，重度美颜降低帧率避免卡顿
        if (mode == CaptureMode::Camera && beautyOn && beautyLv > 0 && beautySty > 0) {
            // 重度美颜样式进一步限帧，避免 CPU 峰值导致整体卡顿。
            if (beautySty >= 5) effectiveFps = std::min(effectiveFps, 18);
            else if (beautySty == 4) effectiveFps = std::min(effectiveFps, 24);
        }
        const int minIntervalMs = std::max(1, 1000 / std::max(1, effectiveFps));// 计算最小帧间隔（毫秒）
        // 未到发送间隔则休眠1ms，避免空循环占用CPU
        const qint64 nowPre = QDateTime::currentMSecsSinceEpoch();
        if (nowPre - lastSend < minIntervalMs) {
            QThread::msleep(1);
            continue;
        }

        if (mode == CaptureMode::Camera) {  //摄像头采集分支
            cv::Mat frame;
            {
                QMutexLocker locker(&mutex);
                if (!cap.isOpened()) {
                    QThread::msleep(10);
                    continue;
                }
                cap >> frame;
            }

            if (frame.empty()) {
                QThread::msleep(5);
                continue;
            }

            if (beautyOn && beautyLv > 0 && beautySty > 0) {
                applyBeautyFilter(frame, beautySty, beautyLv);
            }

            if (virtualBgOn) {
                ++segmentationRequestTick;
                if (segmentationRequestTick >= virtualBgRequestInterval) {
                    segmentationRequestTick = 0;
                    QImage segmentationInput;
                    if (bgrMatToRgbImage(frame, &segmentationInput)) {
                        emit segmentationFrameReady(segmentationInput);
                    }
                }
                if (!virtualBgMask.isNull()) {
                    if (virtualBgMode == QStringLiteral("color")) {
                        applyVirtualBackgroundColor(frame, virtualBgMask, virtualBgColor);
                    } else if (virtualBgMode == QStringLiteral("image")) {
                        applyVirtualBackgroundImage(frame, virtualBgMask, virtualBgImage);
                    } else {
                        applyVirtualBackgroundBlur(frame, virtualBgMask, virtualBgBlurStrength);
                    }
                }
            } else {
                segmentationRequestTick = 0;
            }

            if (!bgrMatToRgbImage(frame, &img)) {
                QThread::msleep(5);
                continue;
            }
        } else {    //屏幕/窗口采集分支
            int targetScreen = 0;
            quint64 targetWindow = 0;
            {
                QMutexLocker locker(&mutex);
                targetScreen = shareScreenIndex;
                targetWindow = shareWindowId;
            }

            QPixmap shot;

            // 1) 优先抓窗口
            if (targetWindow != 0) {
                const WId wid = static_cast<WId>(targetWindow);
                const auto screens = QGuiApplication::screens();
                for (QScreen *s : screens) {
                    if (!s) continue;
                    shot = s->grabWindow(wid);
                    if (!shot.isNull()) break;
                }
            }

            // 2) 失败则抓屏幕
            if (shot.isNull()) {
                const auto screens = QGuiApplication::screens();
                QScreen *screen = nullptr;
                if (targetScreen >= 0 && targetScreen < screens.size()) {
                    screen = screens[targetScreen];
                } else {
                    screen = QGuiApplication::primaryScreen();
                }
                if (!screen) {
                    QThread::msleep(30);
                    continue;
                }
                shot = screen->grabWindow(0);
            }

            if (shot.isNull()) {
                QThread::msleep(30);
                continue;
            }
            //去除透明通道：窗口 / 屏幕截图默认可能带 Alpha 通道（如 Format_ARGB32），转换为 RGB888 可减少数据量，且符合多数视频编码 / 传输的格式要求
            img = shot.toImage().convertToFormat(QImage::Format_RGB888);

            // 限制共享源上限分辨率，降低编码压力
            //Qt::KeepAspectRatio：核心参数，保证缩放后图片的宽高比不变（不会拉伸变形）；
            //Qt::SmoothTransformation：使用平滑插值算法缩放（比如双线性插值），相比默认的快速缩放，图片边缘更清晰，画质更好（代价是略增加一点计算量，但远低于高分辨率编码的压力）
            if (img.width() > shareMaxW || img.height() > shareMaxH) {
                img = img.scaled(shareMaxW, shareMaxH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }

        if (img.isNull()) {
            QThread::msleep(5);
            continue;
        }
        //帧数据存储与发送
        {
            QMutexLocker locker(&mutex);
            lastFrame = img;
        }
        lastSend = QDateTime::currentMSecsSinceEpoch();
        emit frameCaptured(img);//发送 frameCaptured 信号，将处理后的帧对外暴露（供编码 / 显示等后续环节使用）
    }
}

// 延迟加载人脸级联模型，支持多路径回退
bool VideoCapture::ensureFaceCascadeLoaded()
{   //避免重复加载
    if (faceCascadeTried) return faceCascadeLoaded;
    faceCascadeTried = true;
    // 初始化模型文件候选路径列表
    QStringList candidates;
    const QString appDir = QCoreApplication::applicationDirPath();
    candidates << QDir(appDir).filePath("haarcascade_frontalface_default.xml");
    candidates << QDir(appDir).filePath("models/haarcascade_frontalface_default.xml");
    candidates << QDir::current().filePath("haarcascade_frontalface_default.xml");
    candidates << QDir::current().filePath("models/haarcascade_frontalface_default.xml");
    // 常见 OpenCV 安装目录（Windows）
    candidates << "D:/Opencv/opencv-4.8.0-windows/opencv/build/etc/haarcascades/haarcascade_frontalface_default.xml";
    candidates << "D:/Opencv/opencv-4.8.0-windows/opencv/sources/data/haarcascades/haarcascade_frontalface_default.xml";
    candidates << "D:/Opencv/opencv-4.8.0/data/haarcascades/haarcascade_frontalface_default.xml";

    try {
        const cv::String p = cv::samples::findFile("haarcascade_frontalface_default.xml", false, false);
        if (!p.empty()) candidates << QString::fromStdString(p);
    } catch (...) {
    }
    //遍历候选路径，尝试加载模型
    for (const QString &p : candidates) {
        if (p.isEmpty()) continue;
        if (!QFileInfo::exists(p)) continue;
        if (faceCascade.load(p.toStdString())) {    //加载模型
            faceCascadeLoaded = true;
            qInfo() << "[Beauty] face cascade 加载成功:" << p;
            break;
        }
    }
    //加载失败的降级提示，降级到皮肤蒙版模式
    if (!faceCascadeLoaded) {
        qInfo() << "[Beauty] face cascade未找到, 降级到皮肤蒙版模式";
    }
    return faceCascadeLoaded;
}

//执行人脸检测并做降采样与缓存复用
std::vector<cv::Rect> VideoCapture::detectFaces(const cv::Mat &frame)
{   //初始化返回结果：存储检测到的人脸矩形框（cv::Rect 包含x,y坐标和宽高）
    std::vector<cv::Rect> result;
    if (frame.empty()) return result;
    if (!ensureFaceCascadeLoaded()) return result;

    ++faceDetectTick;   //人脸检测计数自增：用于控制检测频率（不是每帧都检测）
    // 优化1：每 12 帧检测一次，人脸框在中间帧复用，减少 CPU 开销
    if (faceDetectTick % 12 != 0 && !cachedFaces.empty()) {
        return cachedFaces;
    }

    //优化2：降采样（缩小图像）后检测，减少计算量
    const double scale = (frame.cols > 960) ? (960.0 / static_cast<double>(frame.cols)) : 1.0;
    cv::Mat small;
    if (scale < 0.999) {    // 比例小于1时才执行缩小（避免浮点精度问题，不用==1.0判断
        cv::resize(frame, small, cv::Size(), scale, scale, cv::INTER_LINEAR);
    } else {
        small = frame;
    }

    //人脸检测前置处理：转灰度图 + 直方图均衡化（提升检测准确率）
    cv::Mat gray;
    cv::cvtColor(small, gray, cv::COLOR_BGR2GRAY);
    cv::equalizeHist(gray, gray);   //直方图均衡化，提升图像对比度（比如逆光/过曝场景下，人脸特征更明显）
    //核心操作：执行人脸检测（基于Haar级联分类器）
    std::vector<cv::Rect> facesSmall;
    faceCascade.detectMultiScale(gray, facesSmall, 1.1, 3, 0, cv::Size(48, 48));
    //坐标还原：将缩小图像上的人脸框坐标，还原到原始帧的尺寸
    //result.reserve：预分配内存（避免频繁扩容），提升性能
    result.reserve(facesSmall.size());
    for (const auto &r : facesSmall) {
        cv::Rect rr = r;
        if (scale < 0.999) {    // 仅缩小过的图像需要还原坐标
            // 还原逻辑：缩小比例是scale，所以坐标/宽高要除以scale
            rr.x = static_cast<int>(rr.x / scale);
            rr.y = static_cast<int>(rr.y / scale);
            rr.width = static_cast<int>(rr.width / scale);
            rr.height = static_cast<int>(rr.height / scale);
        }
        rr = clipRect(rr, frame.size());    //边界裁剪：确保人脸框完全在帧内
        if (rr.area() > 0) result.push_back(rr);
    }
    //缓存本次检测结果：供后续11帧复用
    cachedFaces = result;
    return result;  //返回最终的人脸框列表（基于原始帧尺寸）
}

//执行美颜算法主流程（自然/清晰/柔和/磨皮/瘦脸/祛皱）
void VideoCapture::applyBeautyFilter(cv::Mat &frame, int style, int level)
{
    if (frame.empty()) return;
    //将美颜强度（0-100）归一化为 0.0~1.0 的浮点数 t，超出范围则 clamp 到边界
    const double t = std::clamp(level / 100.0, 0.0, 1.0);
    if (style <= 0 || t <= 0.0) return; //无美颜风格（style≤0）或强度为0（t≤0），直接返回

    // 1~3 走快速路径，避免全帧 bilateral 带来的卡顿。
    //轻量级路径避开计算昂贵的双边滤波（bilateralFilter），仅用高斯模糊和简单像素加权，保证高帧率。
    if (style >= 1 && style <= 3) {
        cv::Mat blur;   // 存储高斯模糊后的图像
        double sigma = 0.9 + 1.5 * t;   // 高斯模糊的sigma（越大越模糊）
        double blend = 0.20 + 0.25 * t; // 模糊图像与原图的融合比例
        double lift = 0.5 + 3.0 * t;    // 亮度提升值
        if (style == 2) {   // 风格2：清晰
            sigma = 0.8 + 1.2 * t;
            blend = 0.12 + 0.16 * t;
            lift = 0.2 + 1.5 * t;
        } else if (style == 3) {    // 风格3：柔和
            sigma = 1.2 + 2.0 * t;
            blend = 0.30 + 0.30 * t;
            lift = 0.8 + 4.0 * t;
        }
        cv::GaussianBlur(frame, blur, cv::Size(0, 0), sigma, sigma);    // 高斯模糊：核心API，Size(0,0)表示由sigma自动计算核大小，sigma越大模糊越强
        //图像加权融合：dst = src1*alpha + src2*beta + gamma
        // 公式：原图*(1-blend) + 模糊图*blend + 亮度提升值lift
        cv::addWeighted(frame, 1.0 - blend, blur, blend, lift, frame);

        if (style == 2) {
            // 清晰：轻锐化
            cv::Mat g;
            cv::GaussianBlur(frame, g, cv::Size(0, 0), 0.9);
            cv::addWeighted(frame, 1.12 + 0.10 * t, g, -(0.12 + 0.10 * t), 0.0, frame);
        } else if (style == 3) {
            // 柔和：微暖色（调整YCrCb色域的Cr通道）
            cv::Mat ycrcb;
            cv::cvtColor(frame, ycrcb, cv::COLOR_BGR2YCrCb);
            std::vector<cv::Mat> ch;    // 存储拆分后的三个通道
            cv::split(ycrcb, ch);   // 拆分通道：ch[0]=Y, ch[1]=Cr, ch[2]=Cb
            // 调整Cr通道（红色差）：提升红色调，t越大越暖
            // convertTo参数：-1=保持原深度，1.0=缩放系数，2.0+7.0*t=偏移量（加亮Cr）
            ch[1].convertTo(ch[1], -1, 1.0, 2.0 + 7.0 * t);
            cv::merge(ch, ycrcb);   // 合并通道
            cv::cvtColor(ycrcb, frame, cv::COLOR_YCrCb2BGR);    //转回BGR
        }
        return;
    }

    //重量级美颜前置：人脸检测（仅 style=5/6）
    std::vector<cv::Rect> faces;
    if (style == 5 || style == 6) {
        // 仅几何/局部处理模式才做人脸检测，减少无效 CPU 消耗。
        faces = detectFaces(frame);
    }

    // 5=瘦脸：先做几何瘦脸，再进入后续轻磨皮
    if (style == 5 && !faces.empty()) {
        applySlimFaceWarp(frame, faces, t);
    }
    //降采样优化：降低分辨率换取帧率,根据分辨率动态降采样，减少后续双边滤波的计算量
    double scale = 1.0;
    if (frame.cols > 1280 || frame.rows > 720) {    // 超720P：降为0.5倍
        scale = 0.5;
    } else if (frame.cols > 960 || frame.rows > 540) {  // 超540P：降为0.66倍
        scale = 0.66;
    } else if (style >= 4) {    // 中低分辨率+重美颜：统一降为0.75倍
        // 重美颜在中低分辨率也统一降采样，换取稳定帧率。
        scale = 0.75;
    }

    cv::Mat srcSmall;
    if (scale < 0.999) {
        cv::resize(frame, srcSmall, cv::Size(), scale, scale, cv::INTER_LINEAR);
    } else {
        srcSmall = frame;
    }

    //重量级磨皮核心：双边滤波（保留边缘的模糊）'
    //转换到YCrCb色域，仅处理亮度通道（Y），避免磨皮影响色彩
    cv::Mat ycrcb;
    cv::cvtColor(srcSmall, ycrcb, cv::COLOR_BGR2YCrCb);
    std::vector<cv::Mat> ch;
    cv::split(ycrcb, ch);
    // 双边滤波参数初始化（style=4磨皮/6祛皱参数不同）
    int d = (style == 4) ? 9 : 7;   // 滤波核直径（越大磨皮越强，计算越慢）
    double sigma = 18.0 + 26.0 * t;
    double alpha = 0.42 + 0.28 * t;
    double beta = 1.5 + 8.0 * t;
    if (style == 6) {   // 滤波核直径（越大磨皮越强，计算越慢）
        sigma = 22.0 + 28.0 * t;
        alpha = 0.50 + 0.24 * t;
        beta = 2.0 + 7.0 * t;
    } else if (style == 5) {
        // 瘦脸模式降低磨皮力度，保留纹理，避免“怪异塑料感”
        d = 5;
        sigma = 12.0 + 18.0 * t;
        alpha = 0.28 + 0.20 * t;
        beta = 0.8 + 3.0 * t;
    }

    // 双边滤波：核心磨皮API（保留边缘，仅模糊纹理）
    // 参数：src=Y通道, dst=ySmooth, d=核直径, sigmaColor=色彩标准差, sigmaSpace=空间标准差
    cv::Mat ySmooth;
    cv::bilateralFilter(ch[0], ySmooth, d, sigma, sigma);
    //融合磨皮后的Y通道：原图Y*(1-alpha) + 磨皮Y*alpha
    cv::addWeighted(ch[0], 1.0 - alpha, ySmooth, alpha, 0.0, ch[0]);
    //提升亮度：Y通道 += beta，t越大越亮
    ch[0].convertTo(ch[0], -1, 1.0, beta);

    cv::merge(ch, ycrcb);
    cv::Mat beautSmall;
    cv::cvtColor(ycrcb, beautSmall, cv::COLOR_YCrCb2BGR);
    //将降采样的美颜图缩放回原图尺寸
    cv::Mat beautBig;
    if (scale < 0.999) {
        cv::resize(beautSmall, beautBig, frame.size(), 0, 0, cv::INTER_LINEAR);
    } else {
        beautBig = beautSmall;
    }

    // 6=祛皱: 对脸部区域做额外纹理平滑
    if (style == 6 && !faces.empty()) {
        for (const auto &f : faces) {
            const cv::Rect rr = expandFaceRect(f, beautBig.size(), 1.20, 1.20);
            if (rr.width < 20 || rr.height < 20) continue;

            cv::Mat roi = beautBig(rr); // 提取人脸ROI区域
            cv::Mat smooth;
            cv::GaussianBlur(roi, smooth, cv::Size(0, 0), 1.6 + 1.4 * t);
            cv::addWeighted(roi, 0.40, smooth, 0.60, 1.0 + 2.5 * t, roi);
        }
    }

    //蒙版融合：控制美颜生效区域（人脸 / 全局）
    cv::Mat mask(frame.rows, frame.cols, CV_8UC1, cv::Scalar(0));
    if (style == 4) {
        // 磨皮默认全局生效，保证效果可见。
        mask.setTo(cv::Scalar(255));
    } else if (!faces.empty()) {
        for (const auto &f : faces) {   //有人脸：仅在人脸椭圆区域生效（避免美颜应用到背景）
            const int cx = f.x + f.width / 2;
            const int cy = f.y + f.height / 2;
            const int rw = std::max(8, static_cast<int>(f.width * 0.72));
            const int rh = std::max(10, static_cast<int>(f.height * 0.98));
            cv::ellipse(mask, cv::Point(cx, cy), cv::Size(rw, rh),
                        0, 0, 360, cv::Scalar(255), -1, cv::LINE_AA);
        }
    } else {
        // 无人脸时也要有可见效果：默认整帧生效，瘦脸模式稍弱
        const int base = (style == 5) ? 170 : 235;
        mask.setTo(cv::Scalar(base));
    }
    cv::GaussianBlur(mask, mask, cv::Size(0, 0), 10.0);

    //调整融合比例（不同风格融合强度不同）
    double blend = 0.70 + 0.18 * t;
    switch (style) {
    case 4: blend = 0.78 + 0.18 * t; break; // 磨皮:更强的融合比例
    case 5: blend = 0.32 + 0.16 * t; break; // 瘦脸:进一步减弱融合，观感更自然
    case 6: blend = 0.74 + 0.20 * t; break; // 祛皱:中等融合
    default: break;
    }

    //蒙版格式转换：8位单通道 → 32位浮点型（用于浮点运算）
    //blend/255.0：将蒙版值缩放到 [0, blend] 范围（控制融合强度）
    cv::Mat maskf;
    mask.convertTo(maskf, CV_32FC1, blend / 255.0);
    // 单通道蒙版 → 3通道蒙版（匹配BGR图像的通道数）
    cv::Mat m3;
    std::vector<cv::Mat> ms(3, maskf);
    cv::merge(ms, m3);
    //原图/美颜图转换为浮点型（避免uchar运算溢出）
    cv::Mat f0, f1;
    frame.convertTo(f0, CV_32FC3);
    beautBig.convertTo(f1, CV_32FC3);
    //融合计算：原图*（1-蒙版） + 美颜图*蒙版
    const cv::Mat inv = cv::Scalar::all(1.0) - m3;
    cv::Mat out = f0.mul(inv) + f1.mul(m3);
    out.convertTo(frame, CV_8UC3);  //转换回8位无符号整型，赋值给原帧
}

//切换采集模式（摄像头或屏幕共享）
void VideoCapture::setCaptureMode(int mode)
{
    QMutexLocker locker(&mutex);
    CaptureMode newMode = (mode == 1) ? CaptureMode::Screen : CaptureMode::Camera;
    if (captureMode == newMode) return;

    captureMode = newMode;
    qInfo() << "[VideoCapture] mode ->" << (captureMode == CaptureMode::Screen ? "screen" : "camera");

    if (captureMode == CaptureMode::Camera) {
        if (!cap.isOpened()) {
            cap.open(currentDeviceIndex);
        }
    } else {
        if (cap.isOpened()) {
            cap.release();
        }
    }
}

//设置屏幕共享目标（屏幕索引或窗口句柄）
void VideoCapture::setShareTarget(int screenIndex, quint64 windowId)
{
    QMutexLocker locker(&mutex);
    shareScreenIndex = std::max(0, screenIndex);
    shareWindowId = windowId;
}

//设置输出节流帧率
void VideoCapture::setTargetFps(int fps)
{
    QMutexLocker locker(&mutex);
    targetFps = std::clamp(fps, 8, 30);
}

//设置共享画面的最大输出分辨率
void VideoCapture::setShareMaxSize(int maxW, int maxH)
{
    QMutexLocker locker(&mutex);
    shareMaxWidth = std::max(640, maxW);
    shareMaxHeight = std::max(360, maxH);
}

//设置美颜总开关
void VideoCapture::setBeautyEnabled(bool on)
{
    QMutexLocker locker(&mutex);
    beautyEnabled = on;
}

//设置美颜强度并同步开关状态
void VideoCapture::setBeautyLevel(int level)
{
    QMutexLocker locker(&mutex);
    beautyLevel = std::clamp(level, 0, 100);
    beautyEnabled = (beautyLevel > 0);
}

//设置美颜风格类型
void VideoCapture::setBeautyStyle(int style)
{
    QMutexLocker locker(&mutex);
    beautyStyle = std::clamp(style, 0, 6);
}

void VideoCapture::setVirtualBackgroundEnabled(bool on)
{
    QMutexLocker locker(&mutex);
    virtualBackgroundEnabled = on;
    if (!virtualBackgroundEnabled) {
        latestVirtualBackgroundMask = QImage();
    }
}

void VideoCapture::setVirtualBackgroundMode(const QString &mode)
{
    QMutexLocker locker(&mutex);
    const QString normalized = mode.trimmed().toLower();
    if (normalized == QStringLiteral("blur")
        || normalized == QStringLiteral("color")
        || normalized == QStringLiteral("image")) {
        virtualBackgroundMode = normalized;
    } else {
        virtualBackgroundMode = QStringLiteral("off");
    }
}

void VideoCapture::setVirtualBackgroundColor(const QColor &color)
{
    if (!color.isValid()) return;
    QMutexLocker locker(&mutex);
    virtualBackgroundColor = color;
}

void VideoCapture::setVirtualBackgroundImage(const QImage &image)
{
    QMutexLocker locker(&mutex);
    virtualBackgroundImage = image;
}

void VideoCapture::setVirtualBackgroundBlurStrength(int level)
{
    QMutexLocker locker(&mutex);
    virtualBackgroundBlurStrength = std::clamp(level, 0, 100);
}

void VideoCapture::setVirtualBackgroundRequestInterval(int interval)
{
    QMutexLocker locker(&mutex);
    virtualBackgroundRequestInterval = std::max(1, interval);
}

void VideoCapture::updateVirtualBackgroundMask(const QImage &mask)
{
    QMutexLocker locker(&mutex);
    latestVirtualBackgroundMask = mask.isNull() ? QImage() : mask.convertToFormat(QImage::Format_Grayscale8);
}

void VideoCapture::clearVirtualBackgroundMask()
{
    QMutexLocker locker(&mutex);
    latestVirtualBackgroundMask = QImage();
}
