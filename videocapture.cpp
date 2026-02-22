#include "videocapture.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QList>
#include <QPixmap>
#include <QScreen>

#include <algorithm>

namespace {

static cv::Rect clipRect(const cv::Rect &r, const cv::Size &s)
{
    return r & cv::Rect(0, 0, s.width, s.height);
}

static cv::Rect expandFaceRect(const cv::Rect &r, const cv::Size &size, double sx, double sy)
{
    const int cx = r.x + r.width / 2;
    const int cy = r.y + r.height / 2;
    const int w = std::max(2, static_cast<int>(r.width * sx));
    const int h = std::max(2, static_cast<int>(r.height * sy));
    const cv::Rect rr(cx - w / 2, cy - h / 2, w, h);
    return clipRect(rr, size);
}

static void applySlimFaceWarp(cv::Mat &img, const std::vector<cv::Rect> &faces, double strength01)
{
    if (img.empty() || faces.empty()) return;

    const double ratio = 0.06 + 0.18 * std::clamp(strength01, 0.0, 1.0); // 横向压缩比例

    for (const auto &f : faces) {
        cv::Rect roi = expandFaceRect(f, img.size(), 1.35, 1.20);
        if (roi.width < 20 || roi.height < 20) continue;

        cv::Mat src = img(roi).clone();
        const int targetW = std::max(2, static_cast<int>(src.cols * (1.0 - ratio)));
        if (targetW >= src.cols) continue;

        cv::Mat squeezed;
        cv::resize(src, squeezed, cv::Size(targetW, src.rows), 0, 0, cv::INTER_LINEAR);

        cv::Mat warped = src.clone();
        const int x0 = (src.cols - targetW) / 2;
        squeezed.copyTo(warped(cv::Rect(x0, 0, targetW, src.rows)));

        // 只在脸部椭圆区域混合，避免边缘形变明显
        cv::Mat mask(src.rows, src.cols, CV_8UC1, cv::Scalar(0));
        cv::ellipse(mask,
                    cv::Point(src.cols / 2, src.rows / 2),
                    cv::Size(std::max(6, static_cast<int>(src.cols * 0.40)),
                             std::max(6, static_cast<int>(src.rows * 0.52))),
                    0, 0, 360, cv::Scalar(255), -1, cv::LINE_AA);
        cv::GaussianBlur(mask, mask, cv::Size(0, 0), 8.0);

        cv::Mat m1;
        mask.convertTo(m1, CV_32FC1, 1.0 / 255.0);
        cv::Mat m3;
        std::vector<cv::Mat> mvec(3, m1);
        cv::merge(mvec, m3);

        cv::Mat f0, f1;
        src.convertTo(f0, CV_32FC3);
        warped.convertTo(f1, CV_32FC3);

        cv::Mat out = f0.mul(cv::Scalar::all(1.0) - m3) + f1.mul(m3);
        out.convertTo(img(roi), CV_8UC3);
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

void VideoCapture::stop()
{
    QMutexLocker locker(&mutex);
    running = false;
    if (cap.isOpened()) cap.release();
}

void VideoCapture::capturePhoto(const QString &path)
{
    QMutexLocker locker(&mutex);
    if (!lastFrame.isNull()) {
        lastFrame.save(path);
    }
}

bool VideoCapture::reopen(int deviceIndex)
{
    QMutexLocker locker(&mutex);
    if (deviceIndex == currentDeviceIndex) return true;
    cap.release();
    if (!cap.open(deviceIndex)) return false;
    currentDeviceIndex = deviceIndex;
    return true;
}

QSize VideoCapture::frameSize() const
{
    int w = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    int h = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    if (w <= 0 || h <= 0) return QSize(640, 480);
    return QSize(w, h);
}

void VideoCapture::captureLoop()
{
    // 输出节流（默认 30fps，可动态调整）
    qint64 lastSend = 0;

    while (running) {
        QImage img;
        CaptureMode mode = CaptureMode::Camera;

        int fpsHint = 30;
        int shareMaxW = 1920;
        int shareMaxH = 1080;
        bool beautyOn = false;
        int beautyLv = 0;
        int beautySty = 0;

        {
            QMutexLocker locker(&mutex);
            mode = captureMode;
            fpsHint = std::clamp(targetFps, 8, 30);
            shareMaxW = std::max(640, shareMaxWidth);
            shareMaxH = std::max(360, shareMaxHeight);
            beautyOn = beautyEnabled;
            beautyLv = beautyLevel;
            beautySty = beautyStyle;
        }

        int effectiveFps = fpsHint;
        if (mode == CaptureMode::Camera && beautyOn && beautyLv > 0 && beautySty > 0) {
            // 重度美颜样式进一步限帧，避免 CPU 峰值导致整体卡顿。
            if (beautySty >= 5) effectiveFps = std::min(effectiveFps, 18);
            else if (beautySty == 4) effectiveFps = std::min(effectiveFps, 24);
        }
        const int minIntervalMs = std::max(1, 1000 / std::max(1, effectiveFps));
        const qint64 nowPre = QDateTime::currentMSecsSinceEpoch();
        if (nowPre - lastSend < minIntervalMs) {
            QThread::msleep(1);
            continue;
        }

        if (mode == CaptureMode::Camera) {
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

            img = QImage(frame.cols, frame.rows, QImage::Format_RGB888);
            if (img.isNull()) {
                QThread::msleep(5);
                continue;
            }

            // 将 BGR 拷贝为 RGB
            for (int y = 0; y < frame.rows; ++y) {
                const uchar *src = frame.ptr<uchar>(y);
                uchar *dst = img.scanLine(y);
                for (int x = 0; x < frame.cols; ++x) {
                    dst[3 * x + 0] = src[3 * x + 2];
                    dst[3 * x + 1] = src[3 * x + 1];
                    dst[3 * x + 2] = src[3 * x + 0];
                }
            }
        } else {
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

            img = shot.toImage().convertToFormat(QImage::Format_RGB888);

            // 限制共享源上限分辨率，降低编码压力
            if (img.width() > shareMaxW || img.height() > shareMaxH) {
                img = img.scaled(shareMaxW, shareMaxH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }

        if (img.isNull()) {
            QThread::msleep(5);
            continue;
        }

        {
            QMutexLocker locker(&mutex);
            lastFrame = img;
        }

        lastSend = QDateTime::currentMSecsSinceEpoch();

        emit frameCaptured(img);
    }
}

bool VideoCapture::ensureFaceCascadeLoaded()
{
    if (faceCascadeTried) return faceCascadeLoaded;
    faceCascadeTried = true;

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

    for (const QString &p : candidates) {
        if (p.isEmpty()) continue;
        if (!QFileInfo::exists(p)) continue;
        if (faceCascade.load(p.toStdString())) {
            faceCascadeLoaded = true;
            qInfo() << "[Beauty] face cascade loaded:" << p;
            break;
        }
    }

    if (!faceCascadeLoaded) {
        qInfo() << "[Beauty] face cascade not found, fallback to skin-mask mode";
    }
    return faceCascadeLoaded;
}

std::vector<cv::Rect> VideoCapture::detectFaces(const cv::Mat &frame)
{
    std::vector<cv::Rect> result;
    if (frame.empty()) return result;
    if (!ensureFaceCascadeLoaded()) return result;

    ++faceDetectTick;
    // 每 12 帧检测一次，人脸框在中间帧复用，减少 CPU 开销
    if (faceDetectTick % 12 != 0 && !cachedFaces.empty()) {
        return cachedFaces;
    }

    const double scale = (frame.cols > 960) ? (960.0 / static_cast<double>(frame.cols)) : 1.0;
    cv::Mat small;
    if (scale < 0.999) {
        cv::resize(frame, small, cv::Size(), scale, scale, cv::INTER_LINEAR);
    } else {
        small = frame;
    }

    cv::Mat gray;
    cv::cvtColor(small, gray, cv::COLOR_BGR2GRAY);
    cv::equalizeHist(gray, gray);

    std::vector<cv::Rect> facesSmall;
    faceCascade.detectMultiScale(gray, facesSmall, 1.1, 3, 0, cv::Size(48, 48));

    result.reserve(facesSmall.size());
    for (const auto &r : facesSmall) {
        cv::Rect rr = r;
        if (scale < 0.999) {
            rr.x = static_cast<int>(rr.x / scale);
            rr.y = static_cast<int>(rr.y / scale);
            rr.width = static_cast<int>(rr.width / scale);
            rr.height = static_cast<int>(rr.height / scale);
        }
        rr = clipRect(rr, frame.size());
        if (rr.area() > 0) result.push_back(rr);
    }

    cachedFaces = result;
    return result;
}

void VideoCapture::applyBeautyFilter(cv::Mat &frame, int style, int level)
{
    if (frame.empty()) return;

    const double t = std::clamp(level / 100.0, 0.0, 1.0);
    if (style <= 0 || t <= 0.0) return;

    // 1~3 走快速路径，避免全帧 bilateral 带来的卡顿。
    if (style >= 1 && style <= 3) {
        cv::Mat blur;
        double sigma = 0.9 + 1.5 * t;
        double blend = 0.20 + 0.25 * t;
        double lift = 0.5 + 3.0 * t;
        if (style == 2) {
            sigma = 0.8 + 1.2 * t;
            blend = 0.12 + 0.16 * t;
            lift = 0.2 + 1.5 * t;
        } else if (style == 3) {
            sigma = 1.2 + 2.0 * t;
            blend = 0.30 + 0.30 * t;
            lift = 0.8 + 4.0 * t;
        }
        cv::GaussianBlur(frame, blur, cv::Size(0, 0), sigma, sigma);
        cv::addWeighted(frame, 1.0 - blend, blur, blend, lift, frame);

        if (style == 2) {
            // 清晰：轻锐化
            cv::Mat g;
            cv::GaussianBlur(frame, g, cv::Size(0, 0), 0.9);
            cv::addWeighted(frame, 1.12 + 0.10 * t, g, -(0.12 + 0.10 * t), 0.0, frame);
        } else if (style == 3) {
            // 柔和：微暖色
            cv::Mat ycrcb;
            cv::cvtColor(frame, ycrcb, cv::COLOR_BGR2YCrCb);
            std::vector<cv::Mat> ch;
            cv::split(ycrcb, ch);
            ch[1].convertTo(ch[1], -1, 1.0, 2.0 + 7.0 * t);
            cv::merge(ch, ycrcb);
            cv::cvtColor(ycrcb, frame, cv::COLOR_YCrCb2BGR);
        }
        return;
    }

    std::vector<cv::Rect> faces;
    if (style == 5 || style == 6) {
        // 仅几何/局部处理模式才做人脸检测，减少无效 CPU 消耗。
        faces = detectFaces(frame);
    }

    // 5=瘦脸：先做几何瘦脸，再进入后续轻磨皮
    if (style == 5 && !faces.empty()) {
        applySlimFaceWarp(frame, faces, t);
    }

    double scale = 1.0;
    if (frame.cols > 1280 || frame.rows > 720) {
        scale = 0.5;
    } else if (frame.cols > 960 || frame.rows > 540) {
        scale = 0.66;
    } else if (style >= 4) {
        // 重美颜在中低分辨率也统一降采样，换取稳定帧率。
        scale = 0.75;
    }

    cv::Mat srcSmall;
    if (scale < 0.999) {
        cv::resize(frame, srcSmall, cv::Size(), scale, scale, cv::INTER_LINEAR);
    } else {
        srcSmall = frame;
    }

    cv::Mat ycrcb;
    cv::cvtColor(srcSmall, ycrcb, cv::COLOR_BGR2YCrCb);
    std::vector<cv::Mat> ch;
    cv::split(ycrcb, ch);

    int d = (style == 4) ? 9 : 7;
    double sigma = 18.0 + 26.0 * t;
    double alpha = 0.42 + 0.28 * t;
    double beta = 1.5 + 8.0 * t;
    if (style == 6) {
        sigma = 22.0 + 28.0 * t;
        alpha = 0.50 + 0.24 * t;
        beta = 2.0 + 7.0 * t;
    }

    cv::Mat ySmooth;
    cv::bilateralFilter(ch[0], ySmooth, d, sigma, sigma);
    cv::addWeighted(ch[0], 1.0 - alpha, ySmooth, alpha, 0.0, ch[0]);
    ch[0].convertTo(ch[0], -1, 1.0, beta);

    cv::merge(ch, ycrcb);
    cv::Mat beautSmall;
    cv::cvtColor(ycrcb, beautSmall, cv::COLOR_YCrCb2BGR);

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

            cv::Mat roi = beautBig(rr);
            cv::Mat smooth;
            cv::GaussianBlur(roi, smooth, cv::Size(0, 0), 1.6 + 1.4 * t);
            cv::addWeighted(roi, 0.40, smooth, 0.60, 1.0 + 2.5 * t, roi);
        }
    }

    cv::Mat mask(frame.rows, frame.cols, CV_8UC1, cv::Scalar(0));
    if (style == 4) {
        // 磨皮默认全局生效，保证效果可见。
        mask.setTo(cv::Scalar(255));
    } else if (!faces.empty()) {
        for (const auto &f : faces) {
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

    double blend = 0.70 + 0.18 * t;
    switch (style) {
    case 4: blend = 0.78 + 0.18 * t; break; // 磨皮
    case 5: blend = 0.44 + 0.18 * t; break; // 瘦脸
    case 6: blend = 0.74 + 0.20 * t; break; // 祛皱
    default: break;
    }

    cv::Mat maskf;
    mask.convertTo(maskf, CV_32FC1, blend / 255.0);
    cv::Mat m3;
    std::vector<cv::Mat> ms(3, maskf);
    cv::merge(ms, m3);

    cv::Mat f0, f1;
    frame.convertTo(f0, CV_32FC3);
    beautBig.convertTo(f1, CV_32FC3);

    const cv::Mat inv = cv::Scalar::all(1.0) - m3;
    cv::Mat out = f0.mul(inv) + f1.mul(m3);
    out.convertTo(frame, CV_8UC3);
}

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

void VideoCapture::setShareTarget(int screenIndex, quint64 windowId)
{
    QMutexLocker locker(&mutex);
    shareScreenIndex = std::max(0, screenIndex);
    shareWindowId = windowId;
}

void VideoCapture::setTargetFps(int fps)
{
    QMutexLocker locker(&mutex);
    targetFps = std::clamp(fps, 8, 30);
}

void VideoCapture::setShareMaxSize(int maxW, int maxH)
{
    QMutexLocker locker(&mutex);
    shareMaxWidth = std::max(640, maxW);
    shareMaxHeight = std::max(360, maxH);
}

void VideoCapture::setBeautyEnabled(bool on)
{
    QMutexLocker locker(&mutex);
    beautyEnabled = on;
}

void VideoCapture::setBeautyLevel(int level)
{
    QMutexLocker locker(&mutex);
    beautyLevel = std::clamp(level, 0, 100);
    beautyEnabled = (beautyLevel > 0);
}

void VideoCapture::setBeautyStyle(int style)
{
    QMutexLocker locker(&mutex);
    beautyStyle = std::clamp(style, 0, 6);
}
