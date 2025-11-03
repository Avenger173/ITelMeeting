#include "videocapture.h"

VideoCapture::VideoCapture(QObject *parent)
    : QObject{parent}, running(false)
{}

VideoCapture::~VideoCapture()
{
    stop();
}

bool VideoCapture::open(int deviceIndex)
{
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
    int w=(int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int h=(int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    if(w<=0||h<=0) return QSize(640,480);
    return QSize(w,h);
}


void VideoCapture::captureLoop()
{
    //防止视频帧过多(30fps限速)
    qint64 lastSend=0;
    while (running) {
        cv::Mat frame;
        {
            QMutexLocker locker(&mutex);
            if (!cap.isOpened()) break;
            cap >> frame;
        }

        if(frame.empty()) continue;

        QImage img(frame.cols, frame.rows, QImage::Format_RGB888);
        if (!img.isNull()) {
            // 将 BGR 拷贝为 RGB
            for (int y = 0; y < frame.rows; ++y) {
                const uchar* src = frame.ptr<uchar>(y);
                uchar* dst = img.scanLine(y);
                for (int x = 0; x < frame.cols; ++x) {
                    dst[3*x + 0] = src[3*x + 2];
                    dst[3*x + 1] = src[3*x + 1];
                    dst[3*x + 2] = src[3*x + 0];
                }
            }
            {
                QMutexLocker locker(&mutex);
                lastFrame=img;
            }
            qint64 now=QDateTime::currentMSecsSinceEpoch();
            if(now-lastSend<33) continue;//33ms约定于30fps
            lastSend=now;

            emit frameCaptured(img);   // 已经是 RGB888 了
        }

    }
}

