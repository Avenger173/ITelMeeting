#include "rtmppuller.h"

#include <QDebug>
#include <QMediaDevices>
#include <cstring>

static QString ffErr(int err) {
    char buf[256] = {0};
    av_strerror(err, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

int rtmppuller::interruptCb(void *opaque) {
    auto *self = static_cast<rtmppuller *>(opaque);
    if (!self) return 0;
    return self->stopFlag.loadAcquire() ? 1 : 0;
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
    stopFlag.storeRelease(false);

    qInfo() << "[RtmpPuller] start(R3) ->" << url;

    fmtCtx = avformat_alloc_context();
    if (!fmtCtx) {
        emit errorOccurred("alloc format context failed");
        emit finished();
        return;
    }
    fmtCtx->interrupt_callback.callback = &rtmppuller::interruptCb;
    fmtCtx->interrupt_callback.opaque = this;

    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "flags", "low_delay", 0);
    av_dict_set(&opts, "analyzeduration", "100000", 0);
    av_dict_set(&opts, "probesize", "16384", 0);
    av_dict_set(&opts, "reorder_queue_size", "0", 0);
    av_dict_set(&opts, "max_delay", "0", 0);
    av_dict_set(&opts, "rtmp_live", "live", 0);

    qInfo() << "[RtmpPuller] opening input...";
    const QByteArray urlUtf8 = url.toUtf8();
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

    vStream = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    aStream = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (vStream < 0) {
        for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
            if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                vStream = static_cast<int>(i);
                break;
            }
        }
    }
    if (aStream < 0) {
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

    if (aStream >= 0) {
        auto *aPar = fmtCtx->streams[aStream]->codecpar;
        const AVCodec *aDec = avcodec_find_decoder(aPar->codec_id);
        if (aDec) {
            aDecCtx = avcodec_alloc_context3(aDec);
            if (aDecCtx && avcodec_parameters_to_context(aDecCtx, aPar) >= 0 && avcodec_open2(aDecCtx, aDec, nullptr) == 0) {
                aFrame = av_frame_alloc();

                const int inCh = aDecCtx->ch_layout.nb_channels > 0 ? aDecCtx->ch_layout.nb_channels : 1;
                const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
                QAudioFormat desired;
                desired.setSampleRate(aDecCtx->sample_rate);
                desired.setChannelCount(inCh);
                desired.setSampleFormat(QAudioFormat::Int16);
                playFmt_ = dev.isFormatSupported(desired) ? desired : dev.preferredFormat();

                audioSink_ = new QAudioSink(dev, playFmt_);
                audioSink_->setBufferSize(playFmt_.bytesForDuration(60000));
                audioOut_ = audioSink_->start();

                AVSampleFormat outFmt = AV_SAMPLE_FMT_S16;
                if (playFmt_.sampleFormat() == QAudioFormat::Float) outFmt = AV_SAMPLE_FMT_FLT;
                else if (playFmt_.sampleFormat() == QAudioFormat::UInt8) outFmt = AV_SAMPLE_FMT_U8;

                AVChannelLayout outLayout;
                av_channel_layout_default(&outLayout, playFmt_.channelCount());

                aswr = swr_alloc();
                swr_alloc_set_opts2(&aswr,
                                    &outLayout, outFmt, playFmt_.sampleRate(),
                                    &aDecCtx->ch_layout, aDecCtx->sample_fmt, aDecCtx->sample_rate,
                                    0, nullptr);
                swr_init(aswr);
                av_channel_layout_uninit(&outLayout);

                // 录制分支固定输出为 44.1kHz/mono/S16，供 AvRecorder::pushAudioPCM 使用
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

    sws = sws_getContext(vDecCtx->width, vDecCtx->height, vDecCtx->pix_fmt,
                         vDecCtx->width, vDecCtx->height, AV_PIX_FMT_RGB24,
                         SWS_BILINEAR, nullptr, nullptr, nullptr);
    frame = av_frame_alloc();
    rgbFrame = av_frame_alloc();
    pkt = av_packet_alloc();

    if (!sws || !frame || !rgbFrame || !pkt) {
        emit errorOccurred("alloc decoder buffers failed");
        cleanup();
        emit finished();
        return;
    }

    const int bufSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, vDecCtx->width, vDecCtx->height, 1);
    rgbBuf = static_cast<uint8_t *>(av_malloc(bufSize));
    if (!rgbBuf) {
        emit errorOccurred("alloc rgb buffer failed");
        cleanup();
        emit finished();
        return;
    }
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuf,
                         AV_PIX_FMT_RGB24, vDecCtx->width, vDecCtx->height, 1);

    bool loggedFirstFrame = false;
    while (!stopFlag.loadAcquire()) {
        ret = av_read_frame(fmtCtx, pkt);
        if (ret < 0) {
            if (ret != AVERROR_EXIT && !stopFlag.loadAcquire()) {
                emit errorOccurred("read_frame: " + ffErr(ret));
            }
            break;
        }

        if (pkt->stream_index == aStream && aDecCtx && aFrame && aswr) {
            ret = avcodec_send_packet(aDecCtx, pkt);
            av_packet_unref(pkt);
            if (ret >= 0) {
                while (!stopFlag.loadAcquire()) {
                    ret = avcodec_receive_frame(aDecCtx, aFrame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) break;

                    const int outSamples = swr_get_out_samples(aswr, aFrame->nb_samples);
                    const int outCh = playFmt_.channelCount();
                    const int outBps = (playFmt_.sampleFormat() == QAudioFormat::Int16) ? 2
                                      : (playFmt_.sampleFormat() == QAudioFormat::Float) ? 4
                                                                                           : 1;
                    QByteArray outBuf(outSamples * outCh * outBps, 0);
                    uint8_t *outData[1] = {reinterpret_cast<uint8_t *>(outBuf.data())};
                    const uint8_t **inData = const_cast<const uint8_t **>(aFrame->extended_data);
                    const int converted = swr_convert(aswr, outData, outSamples, inData, aFrame->nb_samples);
                    if (converted > 0) {
                        const int bytes = converted * outCh * outBps;
                        outBuf.resize(bytes);
                        if (audioSink_ && audioOut_) {
                            audioPending_.append(outBuf.constData(), bytes);
                            const int maxPending = playFmt_.bytesForDuration(120000);
                            if (audioPending_.size() > maxPending) {
                                audioPending_.remove(0, audioPending_.size() - maxPending);
                            }
                            flushAudioPending_();
                        }
                    }

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

        ret = avcodec_send_packet(vDecCtx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            continue;
        }

        while (!stopFlag.loadAcquire()) {
            ret = avcodec_receive_frame(vDecCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            sws_scale(sws, frame->data, frame->linesize, 0, vDecCtx->height, rgbFrame->data, rgbFrame->linesize);

            QImage img(vDecCtx->width, vDecCtx->height, QImage::Format_RGB888);
            for (int y = 0; y < vDecCtx->height; ++y) {
                const int copyBytes = qMin(img.bytesPerLine(), rgbFrame->linesize[0]);
                memcpy(img.scanLine(y), rgbFrame->data[0] + y * rgbFrame->linesize[0], static_cast<size_t>(copyBytes));
            }

            if (!loggedFirstFrame) {
                qInfo() << "[RtmpPuller] first video frame received";
                loggedFirstFrame = true;
            }
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
    audioEnabled_.storeRelease(enabled);
    if (!enabled) {
        audioPending_.clear();
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

void rtmppuller::flushAudioPending_() {
    if (!audioSink_ || !audioOut_) return;
    if (!audioEnabled_.loadAcquire()) {
        audioPending_.clear();
        return;
    }
    while (!audioPending_.isEmpty()) {
        const qint64 freeBytes = audioSink_->bytesFree();
        if (freeBytes <= 0) break;

        const int n = qMin<int>(audioPending_.size(), int(freeBytes));
        if (n <= 0) break;

        const qint64 written = audioOut_->write(audioPending_.constData(), n);
        if (written <= 0) break;

        audioPending_.remove(0, int(written));
    }
}
