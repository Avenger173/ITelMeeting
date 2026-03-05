#include "audiocapture.h"
#include <QDebug>
#include <QDateTime>
#include<QMediaDevices>
//WAV文件头结构体
#pragma pack(push, 1)   //强制1字节对齐，避免结构体因编译器优化错位
struct WAVHeader {
    char riff[4] = {'R','I','F','F'};   //RIFF标识，固定值
    uint32_t fileSize = 0;  // 填写整个文件长度-8（RIFF头占8字节）
    char wave[4] = {'W','A','V','E'};   //WAVE标识，固定值
    char fmt[4] = {'f','m','t',' '};    //fmt块标识，固定值
    uint32_t fmtSize = 16;  //fmt 块大小（PCM格式固定为16）
    uint16_t audioFormat = 1;   // 音频格式：1=PCM（无压缩）
    uint16_t numChannels = 1;   // 声道数：1=单声道
    uint32_t sampleRate = 44100;    // 采样率（每秒采集的音频样本数）：44100Hz（标准音频）
    uint32_t byteRate = 44100 * 2; // sampleRate * channels * bytesPerSample 字节率（每秒音频数据的字节数） = 采样率 × 声道数 × 采样位深/8（16位=2字节）
    uint16_t blockAlign = 2;    // 块对齐（每次采样的总字节数） = 声道数 × 采样位深/8（单声道16位=2）播放器靠这个值读取数据
    uint16_t bitsPerSample = 16;    // 采样位深（每个音频样本的位数）：16位  8 位 = 1 字节（音质低），16 位 = 2 字节（常用），24 位 = 3 字节
    char data[4] = {'d','a','t','a'};   // data 块标识，固定值
    uint32_t dataSize = 0;  // 音频数据总字节数
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
    //1.FFmpeg设备注册与输入配置
    avdevice_register_all();    // 注册所有FFmpeg设备（必须调用，否则无法识别dshow）
    const AVInputFormat *inputFormat = av_find_input_format("dshow");   // 指定输入格式为dshow（Windows音频采集）
    AVDictionary *options = nullptr;    // FFmpeg 的键值对字典结构，用来传递输入 / 输出的参数配置。创建一个空的参数字典，后续往里面塞音频采集的具体参数。
    av_dict_set(&options,"sample_rate","44100",0);  // 强制采样率44100Hz 往字典里添加参数的函数，格式为(字典地址, 键名, 键值, 标志位)
    av_dict_set(&options,"channels","1",0); // 强制单声道
    //尽量让设备给小包(单位ms)
    av_dict_set(&options,"audio_buffer_size","20",0);   // 音频缓冲区大小（20ms，降低延迟）
    //保持较小缓冲，避免半秒一卡
    av_dict_set(&options,"rtbufsize","8M",0);   // 实时缓冲区大小（8MB，避免缓冲不足卡顿）
    av_dict_set(&options,"thread_queue_size","64",0);   // 线程队列大小（64帧，平衡性能与延迟）

    //2.初始化 AVFormatContext（格式上下文）
    fmtCtx=avformat_alloc_context();    // 分配格式上下文（FFmpeg核心结构体）
    if(!fmtCtx){
        emit logMessage("创建AVFormatContext失败");
        return false;
    }
    // 设置中断回调：用于优雅停止采集（通过running标志）
    fmtCtx->interrupt_callback.callback=&AudioCapture::interruptCallback;
    fmtCtx->interrupt_callback.opaque=this;
    // 打开音频输入源（这里是音频设备，比如麦克风），并把设备的格式信息填充到 AVFormatContext 中
    if (avformat_open_input(&fmtCtx, deviceName.toStdString().c_str(), inputFormat, &options) < 0) {
        emit logMessage("无法打开音频设备");
        cleanup();
        return false;
    }
    //3.获取音频流信息
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        emit logMessage("无法找到音频流信息");
        cleanup();
        return false;
    }

    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) { // 遍历流，找到音频流索引
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
    //4.初始化解码器上下文
    const AVCodec *codec = avcodec_find_decoder(fmtCtx->streams[audioStreamIndex]->codecpar->codec_id);
    codecCtx = avcodec_alloc_context3(codec);   // 分配解码器上下文
    avcodec_parameters_to_context(codecCtx, fmtCtx->streams[audioStreamIndex]->codecpar);   // 拷贝流参数到解码器上下文

    // 补充 ch_layout（FFmpeg 7.1.1 必须显式提供）
    if (codecCtx->ch_layout.nb_channels == 0) {
        int chs = fmtCtx->streams[audioStreamIndex]->codecpar->ch_layout.nb_channels;
        if (chs == 0) chs = 1;
        av_channel_layout_default(&codecCtx->ch_layout, chs);   //根据声道数生成默认的声道布局（比如 2 声道对应立体声），确保解码器能识别声道信息。
    }
    // 打开解码器
    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        emit logMessage("音频解码器打开失败");
        cleanup();
        return false;
    }
    //5.初始化重采样器（SwrContext）：将采集到的音频格式转为 单声道/16位/44100Hz（WAV标准格式）
    // 此时 codecCtx 已准备好，开始初始化重采样器
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, 1); // 输出单声道

    swrCtx = swr_alloc();   //分配重采样器上下文
    if (!swrCtx) {
        emit logMessage("创建 SwrContext 失败");
        cleanup();
        return false;
    }
    // 配置重采样参数
    if (swr_alloc_set_opts2(&swrCtx,
                            &outLayout,             // 输出声道布局
                            AV_SAMPLE_FMT_S16,      // 输出采样格式（16位整型WAV 标准格式）
                            44100,                  // 输出采样率
                            &codecCtx->ch_layout,   // 输入声道布局
                            codecCtx->sample_fmt,   // 输入采样格式
                            codecCtx->sample_rate,  // 输入采样率
                            0, nullptr) < 0) {
        emit logMessage("配置 SwrContext 失败");
        cleanup();
        return false;
    }

    if (swr_init(swrCtx) < 0) { // 初始化重采样器
        emit logMessage("初始化 SwrContext 失败");
        cleanup();
        return false;
    }

    // 7.保存到 WAV 文件
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
        totalAudioBytes = 0;    // 初始化音频数据字节计数器

        emit logMessage("保存音频到: " + filename);
    }
    return true;
}


void AudioCapture::captureLoop()
{
    AVPacket *pkt = av_packet_alloc();  //存储编码后的音频帧数据
    if (!pkt) {
        emit logMessage("无法分配AVPacket");
        return;
    }

    if (audioStreamIndex < 0) {
        emit logMessage("未找到音频流");
        av_packet_free(&pkt);
        return;
    }

    emit logMessage("开始音频采集线程");

    running = true;

    while (running) {
        // 1.读取一帧编码数据
        int readRet=av_read_frame(fmtCtx,pkt);
        if(readRet<0){
            if(!running.load()) break;
            continue;
        }

        if (pkt->stream_index != audioStreamIndex) {    // 2.过滤非音频流数据
            av_packet_unref(pkt);   // 释放包引用（避免内存泄漏）
            continue;
        }
        // 3.发送数据包到解码器
        int ret = avcodec_send_packet(codecCtx, pkt);
        if (ret < 0) {
            emit logMessage("发送音频数据失败");
            av_packet_unref(pkt);
            continue;
        }
        // 4.接收解码后的音频帧
        AVFrame *frame = av_frame_alloc();  // 分配解码帧（存储原始音频数据）
        ret = avcodec_receive_frame(codecCtx, frame);   //从解码器接收解码后的原始音频帧
        if (ret == 0) {
            // 准备重采样缓冲区
            uint8_t **convertedSamples = nullptr;
            int maxSamples = frame->nb_samples; // 输入帧的采样数
            int outBytesPerSample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16); // 16位=2字节

            //分配重采样输出缓冲区
            av_samples_alloc_array_and_samples(
                &convertedSamples,
                nullptr,
                1, // 输出单声道
                maxSamples, // 最大采样数（与原帧一致）
                AV_SAMPLE_FMT_S16,  // 输出格式：16位有符号整数（通用音频格式wav）
                0);
            //执行重采样:将原音频格式转为 单声道/S16
            int convertedCount = swr_convert(
                swrCtx,
                convertedSamples,               //输出缓冲区
                maxSamples,                     //输出最大采样数
                (const uint8_t **)frame->data,  //输入数据
                frame->nb_samples);             //输入采样数

            if (convertedCount > 0) {
                int outBytes = convertedCount * 1 * outBytesPerSample;

                QByteArray outBuffer((const char *)convertedSamples[0], outBytes);  // 转为Qt字节数组
                //音频帧分发与保存
                //按1024样本切块（≈23ms）,降低延迟
                const int chunkSample=1024;
                const int chunkBytes=chunkSample*2;//S16+mono=>2bytes
                int offset=0;
                const int outRate=44100;
                while(offset<outBytes){
                    int n=qMin(chunkBytes,outBytes-offset); // 最后一块可能不足1024采样
                    QByteArray chunk(outBuffer.constData()+offset,n);


                    // 发送信号，将音频块传递给播放模块
                    emit audioFrameReady(chunk);

                    // 保存到WAV文件
                    if (enableSave && outFile.isOpen()) {
                        outFile.write(chunk);
                        totalAudioBytes += chunk.size();
                    }
                    offset+=n;
                }

                // 日志，方便调试
                // emit logMessage(QString("采集到音频帧，字节数: %1").arg(outBuffer.size()));
            }
            //释放重采样缓冲区
            av_freep(&convertedSamples[0]); //释放 FFmpeg 分配的内存（自动置空指针）；
            av_freep(&convertedSamples);
            av_frame_free(&frame);  //释放解码帧
        } else {
            av_frame_free(&frame);  //解码失败，释放帧
        }

        av_packet_unref(pkt);   //释放数据包引用
    }

    av_packet_free(&pkt);   //循环结束，释放数据包

    emit logMessage("音频采集线程已退出");
}
//中断回调函数,在 av_read_frame() 等阻塞操作中，会定期调用此函数；
int AudioCapture::interruptCallback(void *opaque)
{
    auto self=static_cast<AudioCapture*>(opaque);
    if(!self) return 0;
    // 返回1表示中断（停止采集），0表示继续
    return self->running.load()?0:1;    //原子读取运行标志，线程安全；返回 1 时，FFmpeg 会终止当前阻塞操作，实现优雅停止。
}



void AudioCapture::stop()
{
    running = false;
    // 不在这里 cleanup，避免采集线程未退出时资源被提前释放
    qDebug() << "[AudioCapture] 停止音频采集线程";
}

void AudioCapture::cleanup()
{
    if (outFile.isOpen()) {
        // 更新 WAV 文件头
        WAVHeader header;
        header.dataSize = static_cast<uint32_t>(totalAudioBytes);
        header.fileSize = header.dataSize + sizeof(WAVHeader) - 8;
        outFile.seek(0);    // 回到文件开头
        outFile.write(reinterpret_cast<const char*>(&header), sizeof(WAVHeader));
        outFile.close();    // 关闭文件
    }

    if (codecCtx) {
        avcodec_free_context(&codecCtx);    // 释放解码器上下文
        codecCtx = nullptr;
    }
    if (fmtCtx) {
        avformat_close_input(&fmtCtx);  // 关闭输入流
        fmtCtx = nullptr;
    }
    if (swrCtx) {
        swr_free(&swrCtx);  // 释放重采样器
    }

    qDebug() << "[AudioCapture] 清理完成.";
}



