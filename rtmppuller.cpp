#include "rtmppuller.h"

#include <QDebug>
#include <QMediaDevices>
#include <QThread>
#include <algorithm>
#include <cstring>

static QString ffErr(int err) { // 将FFmpeg 库返回的错误码（整数） 转换成人类可读的 UTF-8 格式字符串
    char buf[256] = {0};
    av_strerror(err, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}
//中断回调函数：用于 FFmpeg 阻塞操作的优雅退出
int rtmppuller::interruptCb(void *opaque) {
    auto *self = static_cast<rtmppuller *>(opaque); // 透传的类实例指针
    if (!self) return 0;
    return self->stopFlag.loadAcquire() ? 1 : 0;    // 检测停止标志，1=中断，0=继续
}

rtmppuller::rtmppuller(QObject *parent)
    : QObject(parent) {
    avformat_network_init();
    qInfo() << "[RtmpPuller] init build=R3 ffmpeg=" << av_version_info();
}

rtmppuller::~rtmppuller() {
    stop();
    cleanup();
}

void rtmppuller::startPull(const QString &url) {
    cleanup();
    stopFlag.storeRelease(false);   // 原子操作：重置停止标志为false（允许拉流）

    qInfo() << "[RtmpPuller] start(R3) ->" << url;

    fmtCtx = avformat_alloc_context();
    if (!fmtCtx) {
        emit errorOccurred("alloc format context failed");
        emit finished();
        return;
    }
    // 设置中断回调：用于强制退出FFmpeg阻塞操作
    fmtCtx->interrupt_callback.callback = &rtmppuller::interruptCb;
    fmtCtx->interrupt_callback.opaque = this;

    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "fflags", "nobuffer", 0);        // 禁用缓冲区（低延迟）
    av_dict_set(&opts, "flags", "low_delay", 0);        // 强制低延迟模式
    av_dict_set(&opts, "analyzeduration", "100000", 0); // 分析流的时长（微秒）：100ms（默认5秒，改小减少首帧延迟）
    av_dict_set(&opts, "probesize", "16384", 0);        // 探测流的数据包大小（字节）：16KB（默认5MB，改小减少首帧延迟）
    av_dict_set(&opts, "reorder_queue_size", "0", 0);   // 禁用重排序队列（直播无B帧，无需排序）
    av_dict_set(&opts, "max_delay", "0", 0);            // 最大延迟（微秒）：0（无延迟）
    av_dict_set(&opts, "rtmp_live", "live", 0);         // 强制RTMP为直播模式（避免识别为点播）

    qInfo() << "[RtmpPuller] opening input...";
    const QByteArray urlUtf8 = url.toUtf8();
    // 打开URL对应的流，关联到fmtCtx，传入参数opts
    int ret = avformat_open_input(&fmtCtx, urlUtf8.constData(), nullptr, &opts);
    av_dict_free(&opts);
    qInfo() << "[RtmpPuller] open_input ret=" << ret;
    if (ret < 0) {
        if (!stopFlag.loadAcquire()) {
            emit errorOccurred("open_input: " + ffErr(ret));
        }
        cleanup();
        emit finished();
        return;
    }

    qInfo() << "[RtmpPuller] finding stream info...";
    ret = avformat_find_stream_info(fmtCtx, nullptr);
    qInfo() << "[RtmpPuller] find_stream_info ret=" << ret;
    if (ret < 0) {
        if (!stopFlag.loadAcquire()) {
            emit errorOccurred("find_stream_info: " + ffErr(ret));
        }
        cleanup();
        emit finished();
        return;
    }
     // 查找最佳视频/音频流（自动选优先级最高的）
    vStream = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    aStream = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (vStream < 0) {  // 容错：如果av_find_best_stream没找到，手动遍历流列表找视频流
        for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
            if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                vStream = static_cast<int>(i);
                break;
            }
        }
    }
    if (aStream < 0) {  // 容错：手动遍历找音频流
        for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
            if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                aStream = static_cast<int>(i);
                break;
            }
        }
    }

    if (vStream < 0) {
        emit errorOccurred("no video stream");
        cleanup();
        emit finished();
        return;
    }

    qInfo() << "[RtmpPuller] stream ready, video=" << vStream << "audio=" << aStream;

    //初始化视频解码器
    auto *vPar = fmtCtx->streams[vStream]->codecpar;
    const AVCodec *vDec = avcodec_find_decoder(vPar->codec_id);
    if (!vDec) {
        emit errorOccurred("find video decoder failed");
        cleanup();
        emit finished();
        return;
    }

    vDecCtx = avcodec_alloc_context3(vDec);
    if (!vDecCtx) {
        emit errorOccurred("alloc video decoder context failed");
        cleanup();
        emit finished();
        return;
    }

    ret = avcodec_parameters_to_context(vDecCtx, vPar);
    if (ret < 0) {
        emit errorOccurred("video parameters_to_context: " + ffErr(ret));
        cleanup();
        emit finished();
        return;
    }

    ret = avcodec_open2(vDecCtx, vDec, nullptr);
    if (ret < 0) {
        emit errorOccurred("open video decoder: " + ffErr(ret));
        cleanup();
        emit finished();
        return;
    }

    //初始化音频解码器 + Qt 音频播放 + 音频重采样
    if (aStream >= 0) {
        auto *aPar = fmtCtx->streams[aStream]->codecpar;
        const AVCodec *aDec = avcodec_find_decoder(aPar->codec_id);
        if (aDec) {
            aDecCtx = avcodec_alloc_context3(aDec);
            if (aDecCtx && avcodec_parameters_to_context(aDecCtx, aPar) >= 0 && avcodec_open2(aDecCtx, aDec, nullptr) == 0) {
                aFrame = av_frame_alloc();
                // 1. 获取音频输入通道数
                const int inCh = aDecCtx->ch_layout.nb_channels > 0 ? aDecCtx->ch_layout.nb_channels : 1;
                // 2. Qt音频设备配置：默认输出设备
                const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
                QAudioFormat desired;
                desired.setSampleRate(aDecCtx->sample_rate);
                desired.setChannelCount(inCh);
                desired.setSampleFormat(QAudioFormat::Int16);
                // 3. 兼容：如果设备不支持期望格式，用设备首选格式
                playFmt_ = dev.isFormatSupported(desired) ? desired : dev.preferredFormat();
                // 4. 创建Qt音频输出器+启动音频输出
                audioSink_ = new QAudioSink(dev, playFmt_);
                audioSink_->setBufferSize(playFmt_.bytesForDuration(45000));
                audioOut_ = audioSink_->start();    // 启动音频输出（返回写入设备）
                // 5. 配置FFmpeg音频重采样（输入格式→Qt播放格式）
                AVSampleFormat outFmt = AV_SAMPLE_FMT_S16;
                if (playFmt_.sampleFormat() == QAudioFormat::Float) outFmt = AV_SAMPLE_FMT_FLT;
                else if (playFmt_.sampleFormat() == QAudioFormat::UInt8) outFmt = AV_SAMPLE_FMT_U8;
                // 输出声道布局（匹配Qt播放格式的通道数）
                AVChannelLayout outLayout;
                av_channel_layout_default(&outLayout, playFmt_.channelCount());

                aswr = swr_alloc();
                swr_alloc_set_opts2(&aswr,
                                    &outLayout, outFmt, playFmt_.sampleRate(),
                                    &aDecCtx->ch_layout, aDecCtx->sample_fmt, aDecCtx->sample_rate,
                                    0, nullptr);
                swr_init(aswr);
                av_channel_layout_uninit(&outLayout);

                // 6. 录制分支用重采样，固定输出为 44.1kHz/mono/S16，供 AvRecorder::pushAudioPCM 使用
                AVChannelLayout recLayout;
                av_channel_layout_default(&recLayout, 1);
                recSwr_ = swr_alloc();
                if (recSwr_) {
                    if (swr_alloc_set_opts2(&recSwr_,
                                            &recLayout, AV_SAMPLE_FMT_S16, 44100,
                                            &aDecCtx->ch_layout, aDecCtx->sample_fmt, aDecCtx->sample_rate,
                                            0, nullptr) < 0 || swr_init(recSwr_) < 0) {
                        swr_free(&recSwr_);
                    }
                }
                av_channel_layout_uninit(&recLayout);
            }
        }
    }
    //初始化视频解码后处理（YUV→RGB）+ 缓冲区
    // 初始化SWS上下文：视频像素格式转换（解码后的YUV→RGB24）
    sws = sws_getContext(vDecCtx->width, vDecCtx->height, vDecCtx->pix_fmt,
                         vDecCtx->width, vDecCtx->height, AV_PIX_FMT_RGB24,
                         SWS_BILINEAR, nullptr, nullptr, nullptr);
    // 分配视频帧缓冲区
    frame = av_frame_alloc();   // 存储解码后的YUV帧
    rgbFrame = av_frame_alloc();// 存储转换后的RGB帧
    pkt = av_packet_alloc();    // 存储读取的数据包（未解码）
    // 缓冲区分配失败则退出
    if (!sws || !frame || !rgbFrame || !pkt) {
        emit errorOccurred("alloc decoder buffers failed");
        cleanup();
        emit finished();
        return;
    }
    // 分配RGB缓冲区内存（按RGB24格式计算大小）
    const int bufSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, vDecCtx->width, vDecCtx->height, 1);
    rgbBuf = static_cast<uint8_t *>(av_malloc(bufSize));
    if (!rgbBuf) {
        emit errorOccurred("alloc rgb buffer failed");
        cleanup();
        emit finished();
        return;
    }
    // 将RGB缓冲区关联到rgbFrame（填充data和linesize）
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuf,
                         AV_PIX_FMT_RGB24, vDecCtx->width, vDecCtx->height, 1);

    //主循环：读取 + 解码 + 播放 / 分发
    bool loggedFirstFrame = false;  // 标记是否输出首帧日志
    while (!stopFlag.loadAcquire()) {   // 未收到停止信号则循环
        ret = av_read_frame(fmtCtx, pkt);
        if (ret < 0) {
            if (ret != AVERROR_EXIT && !stopFlag.loadAcquire()) {
                emit errorOccurred("read_frame: " + ffErr(ret));
            }
            break;
        }

        // ========== 音频包处理 ==========
        if (pkt->stream_index == aStream && aDecCtx && aFrame && aswr) {
             // 1. 发送音频数据包到解码器
            ret = avcodec_send_packet(aDecCtx, pkt);
            av_packet_unref(pkt);
            if (ret >= 0) {
                // 2. 循环接收解码后的音频帧（可能多帧）
                while (!stopFlag.loadAcquire()) {
                    ret = avcodec_receive_frame(aDecCtx, aFrame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) break;

                    // 3. 计算重采样后的输出样本数
                    const int outSamples = swr_get_out_samples(aswr, aFrame->nb_samples);
                    const int outCh = playFmt_.channelCount();
                    // 计算每个样本的字节数（16位=2字节，浮点=4字节，8位=1字节）
                    const int outBps = (playFmt_.sampleFormat() == QAudioFormat::Int16) ? 2
                                      : (playFmt_.sampleFormat() == QAudioFormat::Float) ? 4
                                                                                           : 1;
                    // 4. 分配输出缓冲区
                    QByteArray outBuf(outSamples * outCh * outBps, 0);
                    uint8_t *outData[1] = {reinterpret_cast<uint8_t *>(outBuf.data())};
                    const uint8_t **inData = const_cast<const uint8_t **>(aFrame->extended_data);
                    // 5. 执行重采样（输入帧→输出缓冲区）
                    const int converted = swr_convert(aswr, outData, outSamples, inData, aFrame->nb_samples);
                    if (converted > 0) {
                        // 6. 调整缓冲区大小（实际转换的样本数）
                        const int bytes = converted * outCh * outBps;
                        outBuf.resize(bytes);
                        if (audioSink_ && audioOut_) {
                            // 7. 缓存音频数据（防止播放卡顿）
                            audioPending_.append(outBuf.constData(), bytes);
                            const int maxPending = playFmt_.bytesForDuration(85000);   // 最大缓存约85ms，避免音质因欠缓冲变差
                            if (audioPending_.size() > maxPending) {
                                audioPending_.remove(0, audioPending_.size() - maxPending);
                            }
                            // 8. 刷缓存到音频设备
                            flushAudioPending_();
                        }
                    }

                    // ========== 录制用音频处理 ==========
                    if (recSwr_) {
                        const int recSamples = swr_get_out_samples(recSwr_, aFrame->nb_samples);
                        if (recSamples > 0) {
                            QByteArray recBuf(recSamples * 2, 0); // mono s16
                            uint8_t *recData[1] = { reinterpret_cast<uint8_t*>(recBuf.data()) };
                            const int recConverted = swr_convert(recSwr_, recData, recSamples, inData, aFrame->nb_samples);
                            if (recConverted > 0) {
                                recBuf.resize(recConverted * 2);
                                emit audioPcmReady(recBuf, 44100, 1);
                            }
                        }
                    }
                    av_frame_unref(aFrame);
                }
            }
            continue;
        }

        if (pkt->stream_index != vStream) {
            av_packet_unref(pkt);
            continue;
        }
        // ========== 视频包处理 ==========
        // 1. 发送视频数据包到解码器
        ret = avcodec_send_packet(vDecCtx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            continue;
        }
        // 2. 循环接收解码后的视频帧
        while (!stopFlag.loadAcquire()) {
            ret = avcodec_receive_frame(vDecCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            // 3. YUV→RGB24转换
            sws_scale(sws, frame->data, frame->linesize, 0, vDecCtx->height, rgbFrame->data, rgbFrame->linesize);

            // 4. 转换为QImage（RGB888格式）
            QImage img(vDecCtx->width, vDecCtx->height, QImage::Format_RGB888);
            for (int y = 0; y < vDecCtx->height; ++y) {
                // 每行复制的字节数（取QImage和RGB帧的较小值，防止越界）
                const int copyBytes = qMin(img.bytesPerLine(), rgbFrame->linesize[0]);
                memcpy(img.scanLine(y), rgbFrame->data[0] + y * rgbFrame->linesize[0], static_cast<size_t>(copyBytes));
            }

            // 5. 首帧日志
            if (!loggedFirstFrame) {
                qInfo() << "[RtmpPuller] first video frame received";
                loggedFirstFrame = true;
            }
            paceVideoAgainstAudio_();
            // 6. 发射视频帧信号（供外部UI显示）
            emit videoFrameReady(img);
            av_frame_unref(frame);
        }
    }

    cleanup();
    emit finished();
}

void rtmppuller::stop() {
    stopFlag.storeRelease(true);
}

void rtmppuller::setAudioEnabled(bool enabled) {
    audioEnabled_.storeRelease(enabled);    // 原子操作：设置音频使能标志
    if (!enabled) {
        audioPending_.clear();  // 禁用则清空音频缓存
    }
}

void rtmppuller::cleanup() {
    if (pkt) {
        av_packet_free(&pkt);
        pkt = nullptr;
    }
    if (frame) {
        av_frame_free(&frame);
        frame = nullptr;
    }
    if (rgbFrame) {
        av_frame_free(&rgbFrame);
        rgbFrame = nullptr;
    }
    if (rgbBuf) {
        av_free(rgbBuf);
        rgbBuf = nullptr;
    }
    if (sws) {
        sws_freeContext(sws);
        sws = nullptr;
    }
    if (vDecCtx) {
        avcodec_free_context(&vDecCtx);
        vDecCtx = nullptr;
    }
    if (fmtCtx) {
        avformat_close_input(&fmtCtx);
        fmtCtx = nullptr;
    }
    vStream = -1;

    if (aFrame) {
        av_frame_free(&aFrame);
        aFrame = nullptr;
    }
    if (aswr) {
        swr_free(&aswr);
        aswr = nullptr;
    }
    if (recSwr_) {
        swr_free(&recSwr_);
        recSwr_ = nullptr;
    }
    if (audioSink_) {
        audioSink_->stop();
        delete audioSink_;
        audioSink_ = nullptr;
        audioOut_ = nullptr;
    }
    if (aDecCtx) {
        avcodec_free_context(&aDecCtx);
        aDecCtx = nullptr;
    }
    audioPending_.clear();
    aStream = -1;
}

qint64 rtmppuller::currentAudioBufferedMs_() const
{
    if (!audioSink_) return 0;

    const qint64 sinkBuffered = std::max<qint64>(0, audioSink_->bufferSize() - audioSink_->bytesFree());
    const qint64 totalBytes = sinkBuffered + audioPending_.size();
    if (totalBytes <= 0) return 0;

    const qint64 us = playFmt_.durationForBytes(totalBytes);
    return us > 0 ? (us / 1000) : 0;
}

void rtmppuller::paceVideoAgainstAudio_()
{
    if (!audioSink_ || !audioOut_ || !audioEnabled_.loadAcquire()) return;

    const qint64 bufferedMs = currentAudioBufferedMs_();
    if (bufferedMs <= 34) return;

    const unsigned long sleepMs = static_cast<unsigned long>(std::clamp<qint64>(bufferedMs - 26, 1, 4));
    QThread::msleep(sleepMs);
}

//音频缓存刷写：将缓存的音频数据写入 Qt 音频设备
void rtmppuller::flushAudioPending_() {
    if (!audioSink_ || !audioOut_) return;
    if (!audioEnabled_.loadAcquire()) {
        audioPending_.clear();
        return;
    }
    while (!audioPending_.isEmpty()) {
        const qint64 freeBytes = audioSink_->bytesFree();   // 获取音频设备空闲缓冲区大小
        if (freeBytes <= 0) break;

        // 取缓存数据和空闲缓冲区的较小值
        const int n = qMin<int>(audioPending_.size(), int(freeBytes));
        if (n <= 0) break;

        // 写入音频数据到设备
        const qint64 written = audioOut_->write(audioPending_.constData(), n);
        if (written <= 0) break;

        // 移除已写入的缓存数据
        audioPending_.remove(0, int(written));
    }
}
