#include "avrecorder.h"
#include<QDebug>
AvRecorder::AvRecorder(QObject *parent)
    : QObject{parent}
{
    //FFmpeg全局注册
    av_log_set_level(AV_LOG_INFO);
}

AvRecorder::~AvRecorder()
{

}

bool AvRecorder::open(const QString &filename, int width, int height, int fps)
{
    if(m_opened){
        qWarning()<<"{Recorder]已打开";
        return true;
    }
    w=width;
    h=height;
    m_fps=fps;
    tb=AVRational{1,fps};

    //1.分配输出上下文(mp4)
    int ret=avformat_alloc_output_context2(&fmtCtx,nullptr,"mp4",filename.toUtf8().constData());

    if(ret<0||!fmtCtx){
        qWarning()<<"[Recorder] avformat_alloc_output_context2 failed:"<<avErr(ret);
        return false;
    }
    //2.选择H.264编码器
    const AVCodec *vcodec=pickH264Encoder();
    if(!vcodec){
        qWarning()<<"[Recorder] no H.264 encoder found";
        return false;
    }

    //3.创建视频流
    vStream=avformat_new_stream(fmtCtx,nullptr);
    if(!vStream){
        qWarning()<<"[Recorder] avformat_new_stream failed";
        return false;
    }
    vStream->id=(int)fmtCtx->nb_streams-1;
    //4.分配并配置编码器上下文
    vCodecCtx=avcodec_alloc_context3(vcodec);
    if(!vCodecCtx){
        qWarning()<<"[Recorder] avcodec_alloc_context3 failed";
        return false;
    }

    vCodecCtx->codec_id=vcodec->id;
    vCodecCtx->width=w;
    vCodecCtx->height=h;
    vCodecCtx->time_base=tb;    //编码器时间基=1/fps
    vStream->time_base=tb;  //流时间基与编码器一致
    vCodecCtx->framerate=AVRational{fps,1};//便于写入avg_frame_rate
    vCodecCtx->pix_fmt=AV_PIX_FMT_YUV420P;

    //码率与GOP可按需调整
    vCodecCtx->bit_rate=1500*1000;//1.5Mbps
    vCodecCtx->gop_size=12;
    vCodecCtx->max_b_frames=0;//低延迟，适合互动

    //MP4常需要全局头
    if(fmtCtx->oformat->flags&AVFMT_GLOBALHEADER){
        vCodecCtx->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    //H.264常见私有参数
    if(vCodecCtx->priv_data){
        av_opt_set(vCodecCtx->priv_data,"preset","veryfast",0);
        av_opt_set(vCodecCtx->priv_data,"tune","zerolatency",0);
    }

    //5.打开编码器
    ret=avcodec_open2(vCodecCtx,vcodec,nullptr);
    if(ret<0){
        qWarning()<<"[Recorder] avcodec_open2(v) failed:"<<avErr(ret);
        return false;
    }

    //6.把编码器参数拷到流(关键)
    ret=avcodec_parameters_from_context(vStream->codecpar,vCodecCtx);
    if(ret<0){
        qWarning()<<"[Recorder] avcodec_parameters_from_context failed:"<<avErr(ret);
        return false;
    }

    //7.打开文件IO
    if(!(fmtCtx->oformat->flags&AVFMT_NOFILE)){
        ret=avio_open(&fmtCtx->pb,filename.toUtf8().constData(),AVIO_FLAG_WRITE);
        if(ret<0){
            qWarning()<<"[Recorder] avio_open failed:"<<avErr(ret);
            return false;
        }
    }

    //8.写文件头
    AVDictionary *muxOpts=nullptr;

    ret=avformat_write_header(fmtCtx,&muxOpts);
    av_dict_free(&muxOpts);
    if(ret<0){
        qWarning()<<"[Recorder] avformat_write_header failed:"<<avErr(ret);
        return false;
    }

    //9.预建sws
    sws=sws_getContext(
        w,h,AV_PIX_FMT_RGB24,
        w,h,AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,nullptr,nullptr,nullptr);

    if(!sws){
        qWarning()<<"[Recorder] sws_getContext failed:"<<avErr(ret);
        return false;
    }

    m_opened=true;
    qInfo()<<"[Recorder] open ok,file="<<filename
            <<"size="<<w<<"x"<<h<<"fps="<<m_fps;
    return true;
}

bool AvRecorder::openAV(const QString &filename, int width, int height, int fps, int sampleRate)
{
    if(m_opened){
        qWarning()<<"[Recorder] 已打开";
        return false;
    }
    w=width;h=height;m_fps=fps;
    tb=AVRational{1,fps};
    atb=AVRational{1,sampleRate};

    //1.分配输出上下文
    int ret=avformat_alloc_output_context2(&fmtCtx,nullptr,"mp4",filename.toUtf8().constData());
    if(ret<0||!fmtCtx){
        qWarning()<<"[Recorder] avformat_alloc_output_context2 failed:"<<avErr(ret);
        return false;
    }

    //2.创建视频编码器/流
    const AVCodec *vcodec=pickH264Encoder();
    if(!vcodec) {
        qWarning()<<"[Recorder] no H.264 encoder";
        return false;
    }
    vStream=avformat_new_stream(fmtCtx,nullptr);
    if(!vStream){
        qWarning()<<"[Recorder] new video stream failed";
        return false;
    }
    vStream->id=(int)fmtCtx->nb_streams-1;

    vCodecCtx=avcodec_alloc_context3(vcodec);
    if(!vCodecCtx){
        qWarning()<<"[Recorder] alloc video codecctx failed";
        return false;
    }
    vCodecCtx->codec_id=vcodec->id;
    vCodecCtx->width=w;
    vCodecCtx->height=h;
    vCodecCtx->time_base=tb;
    vCodecCtx->framerate=AVRational{fps,1};
    vCodecCtx->pix_fmt=AV_PIX_FMT_YUV420P;
    vCodecCtx->bit_rate=1500*1000;
    vCodecCtx->gop_size=12;
    vCodecCtx->max_b_frames=0;

    if(fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        vCodecCtx->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
    if(vCodecCtx->priv_data){
        av_opt_set(vCodecCtx->priv_data,"preset","veryfast",0);
        av_opt_set(vCodecCtx->priv_data,"tune","zerolatency",0);
    }

    ret=avcodec_open2(vCodecCtx,vcodec,nullptr);
    if(ret<0){
        qWarning()<<"[Recorder] open video encoder failed:"<<avErr(ret);
        return false;
    }

    ret=avcodec_parameters_from_context(vStream->codecpar,vCodecCtx);
    if(ret<0){
        qWarning()<<"[Recorder] video params_from_ctx failed:"<<avErr(ret);
        return false;
    }
    vStream->time_base=tb;

    //3.创建音频编码器/流
    const AVCodec *acodec=pickAACEncoder();
    if(!acodec){
        qWarning()<<"[Recorder] no AAC encoder";
        return false;
    }

    aStream=avformat_new_stream(fmtCtx,nullptr);
    if(!aStream){
        qWarning()<<"[Recorder] new audio stream failed";
        return false;
    }
    aStream->id=(int)fmtCtx->nb_streams-1;

    aCodecCtx=avcodec_alloc_context3(acodec);
    if(!aCodecCtx)
    {
        qWarning()<<"[Recorder] alloc audio codecctx failed";
        return false;
    }

    av_channel_layout_default(&aChLayout,1);//先用单声道
    aCodecCtx->ch_layout=aChLayout;
    aCodecCtx->sample_rate=sampleRate;

    //选编码器支持的采样格式
    const AVSampleFormat *p=acodec->sample_fmts;
    AVSampleFormat pick=AV_SAMPLE_FMT_FLTP;
    if(p){
        pick=*p;
        while(*p!=AV_SAMPLE_FMT_NONE){
            if(*p==AV_SAMPLE_FMT_FLTP){
                pick=AV_SAMPLE_FMT_FLTP;
                break;
            }
            ++p;
        }
    }
    aCodecCtx->sample_fmt=pick;

    aCodecCtx->bit_rate=128*1000;//128kbps
    aCodecCtx->time_base=atb;

    if(fmtCtx->oformat->flags& AVFMT_GLOBALHEADER)
        aCodecCtx->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;

    ret=avcodec_open2(aCodecCtx,acodec,nullptr);
    if(ret<0){
        qWarning()<<"[Recorder] open audio encoder failed:"<<avErr(ret);
        return false;
    }

    ret=avcodec_parameters_from_context(aStream->codecpar,aCodecCtx);
    if(ret<0){
        qWarning()<<"[Recorder] audio params_from_ctx failed:"<<avErr(ret);
        return false;
    }
    aStream->time_base=atb;

    //4.打开IO并写头文件
    if(!(fmtCtx->oformat->flags&AVFMT_NOFILE)){
        ret=avio_open(&fmtCtx->pb,filename.toUtf8().constData(),AVIO_FLAG_WRITE);
        if(ret<0){
            qWarning()<<"[Recorder] avio_open failed:"<<avErr(ret);
            return false;
        }
    }

    AVDictionary *muxOpts=nullptr;

    ret=avformat_write_header(fmtCtx,&muxOpts);
    av_dict_free(&muxOpts);
    if(ret<0){
        qWarning()<<"[Recorder] write_header failed:"<<avErr(ret);
        return false;
    }

    //预建sws与音频重采样
    sws=sws_getContext(
        w,h,AV_PIX_FMT_RGB24,
        w,h,AV_PIX_FMT_YUV420P,
        SWS_BICUBIC,nullptr,nullptr,nullptr);
    if(!sws){
        qWarning()<<"[Recorder] sws_getContext failed";
        return false;
    }

    //音频重采样
    AVChannelLayout inLayout;
    av_channel_layout_default(&inLayout,1);
    int inRate=sampleRate;
    AVSampleFormat inFmt=AV_SAMPLE_FMT_S16;

    aSwr=swr_alloc();
    if(!aSwr){
        qWarning()<<"[Recorder] swr_alloc failed";
        return false;
    }

    if(swr_alloc_set_opts2(&aSwr,
                            &aCodecCtx->ch_layout,
                            aCodecCtx->sample_fmt,
                            aCodecCtx->sample_rate,
                            &inLayout,inFmt,inRate,
                            0,nullptr)<0){
        qWarning()<<"[Recorder] swr_alloc_set_opts2 failed";
        return false;
    }
    if(swr_init(aSwr)<0){
        qWarning()<<"[Recorder] swr_init failed";
        return false;
    }

    vNextPts=0;
    aNextPts=0;
    m_opened=true;
    qInfo()<<"[Recorder] openAV OK file="<<filename
            <<"video="<<w<<"x"<<h<<"@"<<fps
            <<"audio="<<sampleRate<<"Hz/mono";
    return true;
}

void AvRecorder::pushAudioPCM(const uint8_t *data, int nb_samples)
{
    if(!m_opened || !fmtCtx || !aCodecCtx || !aStream || !aSwr) return;
    if(!data || nb_samples <= 0) return;

    // 把采集的 PCM 样本追加到缓存
    const int16_t* samples = reinterpret_cast<const int16_t*>(data);
    audioBuffer.insert(audioBuffer.end(), samples, samples + nb_samples);

    // AAC 编码器要求固定 frame_size
    const int frame_size = aCodecCtx->frame_size > 0 ? aCodecCtx->frame_size : 1024;

    while(audioBuffer.size() >= frame_size) {
        // 1. 拿出 frame_size 个样本
        std::vector<int16_t> frameData(audioBuffer.begin(), audioBuffer.begin() + frame_size);
        audioBuffer.erase(audioBuffer.begin(), audioBuffer.begin() + frame_size);

        // 2. 填充 aFrame
        if(!aFrame) {
            aFrame = av_frame_alloc();
            aFrame->nb_samples = frame_size;
            aFrame->format = aCodecCtx->sample_fmt;
            aFrame->ch_layout = aCodecCtx->ch_layout;
            aFrame->sample_rate = aCodecCtx->sample_rate;
            av_frame_get_buffer(aFrame, 0);
        }

        // 3. resample 成 codec 需要的格式
        const uint8_t* inData[1] = { reinterpret_cast<const uint8_t*>(frameData.data()) };
        int ret = swr_convert(aSwr,
                              aFrame->data, aFrame->nb_samples,
                              inData, frame_size);
        if (ret < 0) {
            qWarning() << "[Recorder] swr_convert failed:" << avErr(ret);
            return;
        }
        aFrame->nb_samples=ret;
        aFrame->pts = aNextPts;
        aNextPts += ret;

        // 4. 送编码器
        ret = avcodec_send_frame(aCodecCtx, aFrame);
        if (ret < 0) {
            qWarning() << "[Recorder] avcodec_send_frame(audio) failed:" << avErr(ret);
        }


        AVPacket pkt;
        av_init_packet(&pkt);
        while (ret >= 0) {
            ret = avcodec_receive_packet(aCodecCtx, &pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                qWarning() << "[Recorder] avcodec_receive_packet(audio) failed:" << avErr(ret);
                break;
            }
            av_packet_rescale_ts(&pkt, aCodecCtx->time_base, aStream->time_base);
            pkt.stream_index = aStream->index;
            //avsender:
            int64_t pts_ms=0;
            if(pkt.pts!=AV_NOPTS_VALUE){
                pts_ms=av_rescale_q(pkt.pts,aCodecCtx->time_base,AVRational{1,1000});
            }
            QByteArray ba(reinterpret_cast<const char*>(pkt.data),pkt.size);
            emit audioPacketReady(ba,static_cast<quint32>(pts_ms));


            qDebug()<<"[Recorder] wirte pkt stream="<<pkt.stream_index
                     <<" pts="<<pkt.pts<<"dts"<<pkt.dts
                     <<" size="<<pkt.size;
            av_interleaved_write_frame(fmtCtx, &pkt);
            av_packet_unref(&pkt);
        }
    }
}

void AvRecorder::pushVideoFrame(const QImage &img)
{
    if(!m_opened||!fmtCtx||!vCodecCtx||!vStream||!sws){
        qWarning()<<"[Recorder] pushVideoFrame:recorder not opened or not ready";
        return;
    }

    //1.统一源图格式为RGB888
    QImage rgb=img.convertToFormat(QImage::Format_RGB888);
    if(rgb.isNull()){
        qWarning()<<"[Recorder] pushVideoFrame:input image is null";
        return;
    }
    //尺寸不一致时临时缩放
    if(rgb.width()!=w||rgb.height()!=h){
        rgb=rgb.scaled(w,h,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    }

    //2.懒分配可复用的输出帧(YUV420p)
    if(!vFrame){
        vFrame=av_frame_alloc();
        if(!vFrame){
            qWarning()<<"[Recorder] av_frame_alloc failed";
            return;
        }
        vFrame->format=vCodecCtx->pix_fmt;
        vFrame->width=w;
        vFrame->height=h;

        int ret=av_frame_get_buffer(vFrame,32);
        if(ret<0){
            qWarning()<<"[Recorder] av_frame_get_buffer failed:"<<avErr(ret);
            av_frame_free(&vFrame);
            return;
        }
    }

    //3.将RGB数据通过sws_scale 转到YUV420P帧缓冲
    const uint8_t *srcData[1]={rgb.bits()};
    const int  srcLinesize[1]={static_cast<int>(rgb.bytesPerLine())};

    int ret=sws_scale(
        sws,
        srcData,srcLinesize,
        0,h,
        vFrame->data,vFrame->linesize);
    if(ret<=0){
        qWarning()<<"[Recorder] sws_scale failed,ret="<<ret;
        return;
    }

    //4.设置PTS（基于真实采集时间）
    static int64_t start_time=av_gettime_relative();
    int64_t now=av_gettime_relative()-start_time;

    //把微秒换算成流的time_base单位
    vFrame->pts=av_rescale_q(now,AVRational{1,1000000},vCodecCtx->time_base);

    //5.送入编码器
    ret=avcodec_send_frame(vCodecCtx,vFrame);
    if(ret<0){
        qWarning()<<"[Recorder] avcodec_send_frame failed:"<<avErr(ret);
        return;
    }

    //6.取出packet并写入容器
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data=nullptr;
    pkt.size=0;

    for(;;){
        ret=avcodec_receive_packet(vCodecCtx,&pkt);
        if(ret==AVERROR(EAGAIN)||ret==AVERROR_EOF){
            break;//编码器还没产出或已结束
        }
        if(ret<0){
            qWarning()<<"[Recorder] avcodec_receive_packet failed:"<<avErr(ret);
            break;
        }

        //key:时间戳从codecCtx的time_base->流的time_base
        av_packet_rescale_ts(&pkt,vCodecCtx->time_base,vStream->time_base);
        pkt.stream_index=vStream->index;

        //avsender视频流传输：
        int64_t pts_ms=0;
        if(pkt.pts!=AV_NOPTS_VALUE){
            pts_ms=av_rescale_q(pkt.pts,vCodecCtx->time_base,AVRational{1,1000});
        }
        //avsender复制packet数据到QByteArray并发signal
        QByteArray ba(reinterpret_cast<const char*>(pkt.data),pkt.size);
        emit videoPacketReady(ba,static_cast<quint32>(pts_ms));

        ret=av_interleaved_write_frame(fmtCtx,&pkt);
        if(ret<0){
            qWarning()<<"[Recorder] av_interleaved_write_frame failed:"<<avErr(ret);
        }
        av_packet_unref(&pkt);
    }
}

void AvRecorder::close()
{
    if (!m_opened) return;

    int ret = 0;
    if(!audioBuffer.empty()) {
        // 填零补齐到 1024
        int frame_size = aCodecCtx->frame_size;
        int need = frame_size - audioBuffer.size();
        audioBuffer.insert(audioBuffer.end(), need, 0);

        pushAudioPCM(reinterpret_cast<const uint8_t*>(audioBuffer.data()), frame_size);
        audioBuffer.clear();
    }

    // === 1) flush 音频编码器（取出残留 packet） ===
    if (aCodecCtx) {
        // 发送空帧触发 flush
        ret = avcodec_send_frame(aCodecCtx, nullptr);
        if (ret < 0) {
            qWarning() << "[Recorder] audio flush send_frame(nullptr) failed:" << avErr(ret);
        } else {
            AVPacket pkt;
            av_init_packet(&pkt);
            pkt.data = nullptr;
            pkt.size = 0;
            while (true) {
                ret = avcodec_receive_packet(aCodecCtx, &pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    qWarning() << "[Recorder] audio flush receive_packet failed:" << avErr(ret);
                    break;
                }
                av_packet_rescale_ts(&pkt, aCodecCtx->time_base, aStream->time_base);
                pkt.stream_index = aStream->index;
                av_interleaved_write_frame(fmtCtx, &pkt);
                av_packet_unref(&pkt);
            }
        }
    }

    // === 2) flush 视频编码器（若需要） ===
    if (vCodecCtx) {
        ret = avcodec_send_frame(vCodecCtx, nullptr);
        if (ret < 0) {
            qWarning() << "[Recorder] video flush send_frame(nullptr) failed:" << avErr(ret);
        } else {
            AVPacket pkt;
            av_init_packet(&pkt);
            pkt.data = nullptr;
            pkt.size = 0;
            while (true) {
                ret = avcodec_receive_packet(vCodecCtx, &pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    qWarning() << "[Recorder] video flush receive_packet failed:" << avErr(ret);
                    break;
                }
                av_packet_rescale_ts(&pkt, vCodecCtx->time_base, vStream->time_base);
                pkt.stream_index = vStream->index;
                av_interleaved_write_frame(fmtCtx, &pkt);
                av_packet_unref(&pkt);
            }
        }
    }

    // === 3) 写文件尾（trailer） ===
    ret = av_write_trailer(fmtCtx);
    if (ret < 0) {
        qWarning() << "[Recorder] av_write_trailer failed:" << avErr(ret);
    }

    // === 4) 释放 sws 与 swr 等资源 ===
    if (sws) {
        sws_freeContext(sws);
        sws = nullptr;
    }

    if (aSwr) {
        swr_free(&aSwr);
        aSwr = nullptr;
    }

    // 释放音频 AVFrame
    if (aFrame) {
        av_frame_free(&aFrame);
        aFrame = nullptr;
    }

    // 释放视频 AVFrame（若你有 vFrame，请类似释放）
    if (vFrame) {
        av_frame_free(&vFrame);
        vFrame = nullptr;
    }

    // 释放编码器上下文
    if (vCodecCtx) {
        avcodec_free_context(&vCodecCtx);
        vCodecCtx = nullptr;
    }
    if (aCodecCtx) {
        avcodec_free_context(&aCodecCtx);
        aCodecCtx = nullptr;
    }

    // 关闭 IO
    if (fmtCtx && !(fmtCtx->oformat->flags & AVFMT_NOFILE) && fmtCtx->pb) {
        avio_closep(&fmtCtx->pb);
    }

    // 重置成员
    vStream = nullptr;
    aStream = nullptr;
    aNextPts = 0;
    m_opened = false;

    qInfo() << "[Recorder] close ok";
}

const AVCodec *AvRecorder::pickAACEncoder() const
{
    const AVCodec* c=avcodec_find_encoder_by_name("libfdk_aac");
    if(!c)  c=avcodec_find_encoder(AV_CODEC_ID_AAC);
    return c;
}

const AVCodec *AvRecorder::pickH264Encoder() const
{
    const AVCodec *c=avcodec_find_encoder_by_name("libx264");
    if(!c)  c=avcodec_find_encoder(AV_CODEC_ID_H264);
    return c;
}

QString AvRecorder::avErr(int errnum)
{
    char buf[256];
    av_strerror(errnum,buf,sizeof(buf));
    return QString::fromUtf8(buf);
}

