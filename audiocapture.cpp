#include "audiocapture.h"
#include <QDebug>
#include <QDateTime>
#include<QMediaDevices>
#pragma pack(push, 1)
struct WAVHeader {
    char riff[4] = {'R','I','F','F'};
    uint32_t fileSize = 0;  // 填写整个文件长度-8
    char wave[4] = {'W','A','V','E'};
    char fmt[4] = {'f','m','t',' '};
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1; // PCM
    uint16_t numChannels = 1;
    uint32_t sampleRate = 44100;
    uint32_t byteRate = 44100 * 2; // sampleRate * channels * bytesPerSample
    uint16_t blockAlign = 2;
    uint16_t bitsPerSample = 16;
    char data[4] = {'d','a','t','a'};
    uint32_t dataSize = 0;
};
#pragma pack(pop)

AudioCapture::AudioCapture(QObject *parent)
    : QObject{parent}
{

}

AudioCapture::~AudioCapture()
{
    cleanup(); // 只析构时 cleanup，避免重复释放
}
bool AudioCapture::startCapture(const QString &deviceName, bool saveAudio, bool playAudio, QAudioFormat outputFormat)
{
    running = true;
    enableSave = saveAudio;
    enablePlay = playAudio;

    avdevice_register_all();

    const AVInputFormat *inputFormat = av_find_input_format("dshow");
    AVDictionary *options = nullptr;
    av_dict_set(&options, "rtbufsize", "100M", 0);

    if (avformat_open_input(&fmtCtx, deviceName.toStdString().c_str(), inputFormat, &options) < 0) {
        emit logMessage("无法打开音频设备");
        cleanup();
        return false;
    }

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        emit logMessage("无法找到音频流信息");
        cleanup();
        return false;
    }

    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = i;
            break;
        }
    }

    if (audioStreamIndex == -1) {
        emit logMessage("未找到音频流");
        cleanup();
        return false;
    }

    const AVCodec *codec = avcodec_find_decoder(fmtCtx->streams[audioStreamIndex]->codecpar->codec_id);
    codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx, fmtCtx->streams[audioStreamIndex]->codecpar);

    // 补充 ch_layout（FFmpeg 7.1.1 必须显式提供）
    if (codecCtx->ch_layout.nb_channels == 0) {
        int chs = fmtCtx->streams[audioStreamIndex]->codecpar->ch_layout.nb_channels;
        if (chs == 0) chs = 1;
        av_channel_layout_default(&codecCtx->ch_layout, chs);
    }

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        emit logMessage("音频解码器打开失败");
        cleanup();
        return false;
    }

    // 此时 codecCtx 已准备好，开始初始化重采样器
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, 1); // 输出单声道

    swrCtx = swr_alloc();
    if (!swrCtx) {
        emit logMessage("创建 SwrContext 失败");
        cleanup();
        return false;
    }

    if (swr_alloc_set_opts2(&swrCtx,
                            &outLayout,
                            AV_SAMPLE_FMT_S16,
                            44100,
                            &codecCtx->ch_layout,
                            codecCtx->sample_fmt,
                            codecCtx->sample_rate,
                            0, nullptr) < 0) {
        emit logMessage("配置 SwrContext 失败");
        cleanup();
        return false;
    }

    if (swr_init(swrCtx) < 0) {
        emit logMessage("初始化 SwrContext 失败");
        cleanup();
        return false;
    }

    // 保存到 WAV 文件
    if (enableSave) {
        QString filename = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss") + ".wav";
        outFile.setFileName(filename);
        if (!outFile.open(QIODevice::WriteOnly)) {
            emit logMessage("音频文件打开失败");
            cleanup();
            return false;
        }

        WAVHeader header; // 写入空白 WAV 头
        outFile.write(reinterpret_cast<const char*>(&header), sizeof(WAVHeader));
        totalAudioBytes = 0;

        emit logMessage("保存音频到: " + filename);
    }
    return true;
}


void AudioCapture::captureLoop()
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        emit logMessage("无法分配AVPacket");
        return;
    }

    // 查找音频流
    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = i;
            break;
        }
    }

    if (audioStreamIndex < 0) {
        emit logMessage("未找到音频流");
        av_packet_free(&pkt);
        return;
    }

    emit logMessage("开始音频采集线程");

    running = true;

    while (running) {
        if (av_read_frame(fmtCtx, pkt) < 0) {
            continue;
        }

        if (pkt->stream_index != audioStreamIndex) {
            av_packet_unref(pkt);
            continue;
        }

        int ret = avcodec_send_packet(codecCtx, pkt);
        if (ret < 0) {
            emit logMessage("发送音频数据失败");
            av_packet_unref(pkt);
            continue;
        }

        AVFrame *frame = av_frame_alloc();
        ret = avcodec_receive_frame(codecCtx, frame);
        if (ret == 0) {
            // 准备重采样缓冲区
            uint8_t **convertedSamples = nullptr;
            int maxSamples = frame->nb_samples;
            int outBytesPerSample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);

            av_samples_alloc_array_and_samples(
                &convertedSamples,
                nullptr,
                1, // 输出单声道
                maxSamples,
                AV_SAMPLE_FMT_S16,
                0);

            int convertedCount = swr_convert(
                swrCtx,
                convertedSamples,
                maxSamples,
                (const uint8_t **)frame->data,
                frame->nb_samples);

            if (convertedCount > 0) {
                int outBytes = convertedCount * 1 * outBytesPerSample;

                QByteArray outBuffer((const char *)convertedSamples[0], outBytes);

                // 只通过信号分发音频数据
                emit audioFrameReady(outBuffer);

                // 保存
                if (enableSave && outFile.isOpen()) {
                    outFile.write(outBuffer);
                    outFile.flush();
                    totalAudioBytes += outBuffer.size();
                }
                // 新增：日志，方便调试
                emit logMessage(QString("采集到音频帧，字节数: %1").arg(outBuffer.size()));
            }

            av_freep(&convertedSamples[0]);
            av_freep(&convertedSamples);
            av_frame_free(&frame);
        } else {
            av_frame_free(&frame);
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);

    emit logMessage("音频采集线程已退出");
}



void AudioCapture::stop()
{
    running = false;
    // 不再这里 cleanup，避免采集线程未退出时资源被提前释放
    qDebug() << "[AudioCapture] 停止音频采集线程";
}

void AudioCapture::cleanup()
{
    if (outFile.isOpen()) {
        // 更新 WAV 文件头
        WAVHeader header;
        header.dataSize = static_cast<uint32_t>(totalAudioBytes);
        header.fileSize = header.dataSize + sizeof(WAVHeader) - 8;
        outFile.seek(0);
        outFile.write(reinterpret_cast<const char*>(&header), sizeof(WAVHeader));
        outFile.close();
    }

    if (codecCtx) {
        avcodec_free_context(&codecCtx);
        codecCtx = nullptr;
    }
    if (fmtCtx) {
        avformat_close_input(&fmtCtx);
        fmtCtx = nullptr;
    }
    if (swrCtx) {
        swr_free(&swrCtx);
    }

    qDebug() << "[AudioCapture] cleanup complete.";
}



