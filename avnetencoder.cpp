#include "avnetencoder.h"
#include<QDebug>

static bool isAnnexB(const uint8_t* p,int n);
AvNetEncoder::AvNetEncoder(QObject *parent)
    : QObject{parent}
{}

AvNetEncoder::~AvNetEncoder()
{
    close();
}
//初始化编码器
bool AvNetEncoder::openVideo(int w, int h, int fps, int bitrate)
{
    if(opened_) return true;
    w_=w;
    h_=h;
    fps_=fps>0?fps:30;
    bitrate_ = bitrate > 0 ? bitrate : 2200000;

    vCodec_=avcodec_find_encoder_by_name("libx264");
    if(!vCodec_) vCodec_=avcodec_find_encoder(AV_CODEC_ID_H264);
    if(!vCodec_){
        qWarning()<<"[AvNetEncoder] H264编码器未找到";
        return false;
    }
    //分配上下文内存
    vCtx_=avcodec_alloc_context3(vCodec_);
    vCtx_->codec_type=AVMEDIA_TYPE_VIDEO;   //媒体类型：视频
    vCtx_->width=w_;
    vCtx_->height=h_;
    vCtx_->pix_fmt=AV_PIX_FMT_YUV420P;  // 编码像素格式（H264 标准要求 YUV420P）
    vCtx_->time_base=AVRational{1,1000};    // 时间基：1/1000（毫秒）比如某个帧的 pts（显示时间戳）值为 500，结合这个时间基，就表示该帧应在 500 × (1/1000) = 0.5 秒时显示。
    vCtx_->framerate=AVRational{fps_,1};
    vCtx_->bit_rate=bitrate_;
    const int gop=qMax(1,fps_); // GOP 大小：等于帧率（即 1 秒 1 个关键帧），两 I 帧间帧数，决定压缩率、随机访问与容错性。
    vCtx_->gop_size=gop;
    vCtx_->max_b_frames=0;  //实时禁言B帧（实时编码必须，B 帧依赖前后帧，增加延迟）
    vCtx_->keyint_min=gop;  // 最小关键帧间隔（与 GOP 一致，避免频繁关键帧）

    if(vCodec_->id==AV_CODEC_ID_H264&&vCtx_->priv_data){
        setX2640pts_(vCtx_,fps_);   // 专门设置 x264 私有参数
    }

    //启用全局头：让H264的SPS/PPS（核心参数集（序列参数集 / 图像参数集），包含分辨率、帧率、编码配置等基础信息，是解码 H264 数据的必要前提）进extradata，而非每帧前重复
    //减少 H264 码流的冗余数据，降低整体码率，同时符合 RTMP、RTSP 等流媒体传输的规范（这类协议通常要求 SPS/PPS 单独携带，而非嵌在每帧里）
    vCtx_->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;  //用位或操作给编码器上下文的 flags 添加这个标志（不会覆盖原有标志，只新增）

    AVDictionary* opts=nullptr;

    //x264的参数必须通过opts传入，最稳
    av_dict_set(&opts,"preset","ultrafast",0);  // 编码速度：超快（牺牲画质换速度）
    av_dict_set(&opts,"tune","zerolatency",0);  // 场景调优：零延迟（实时编码）

    //repeat headers+annexb+固定GOP
    //IDR：特殊 I 帧，解码器收到后清空参考帧缓存、更新参数，后续帧不参考此前数据，用于视频断点解码 / 纠错。
    //repeat-headers=1:每个IDR前都带SPS/PPS(接收端中途加入也能解）
    //annexb=1:输出00 00 00 01起始码
    //keyint/min-keyint:控制IDR间隔
    //scenecut=0:不让场景切换乱改GOP
    av_dict_set(&opts,"x264-params",QString("repeat-headers=1:annexb=1:keyint=%1:min-keyint=%1:scenecut=0").arg(gop).toUtf8().constData(),0);
    av_dict_set(&opts,"g",QString::number(gop).toUtf8().constData(),0);

    //打开编码器
    int openRet=avcodec_open2(vCtx_,vCodec_,&opts);
    av_dict_free(&opts);    //释放字典内存
    if(openRet<0){
        qWarning()<<"[AvNetEncoder] 编码器打开失败";
        close();
        return false;
    }
    configEmitted=false;    // 标记配置包未发送


    qDebug()<<"[AvNetEncoder] encoder="<<(vCodec_?vCodec_->name:"null")
             <<"id="<<(vCodec_?vCodec_->id:-1);

    //open2之后再看extradata，验证编码器打开后关键的额外数据（extradata）是否正确生成。extradata 有效性直接影响后续编解码流程（比如推流、封装格式）
    //extradata：编码器上下文的字段，存储编解码所需的额外数据（如 H264 的 SPS/PPS），AV_CODEC_FLAG_GLOBAL_HEADER 启用后会填充
    if(vCtx_->extradata&&vCtx_->extradata_size>0){
        qDebug()<<"[AvNetEncoder] extradata ready.size="<<vCtx_->extradata_size
                 <<"first16="
                 // 提取extradata的前16个字节（或全部，若不足16），转成16进制字符串输出
                 <<QByteArray((const char*)vCtx_->extradata,qMin(16,vCtx_->extradata_size)).toHex();
    }else{
        qWarning()<<"[AvNetEncoder] still no extradata after open2";
    }

    //分配YUV帧缓冲区
    yuv_=av_frame_alloc();
    yuv_->format=vCtx_->pix_fmt;
    yuv_->width=w_;
    yuv_->height=h_;
    if(av_frame_get_buffer(yuv_,32)<0){ //为 AVFrame 分配数据缓冲区（32 是对齐值），用于存储像素数据（如 YUV）或音频采样数据
        qWarning()<<"[AvNetEncoder] av_frame_get_buffer failed";
        close();
        return false;
    }

    //分配 AVPacket 与初始化像素格式转换器：RGB24 -> YUV420P
    pkt_=av_packet_alloc();

    sws_=sws_getContext(w_,h_,AV_PIX_FMT_RGB24,
                        w_,h_,AV_PIX_FMT_YUV420P,
                        SWS_BILINEAR,nullptr,nullptr,nullptr);//SWS_BILINEAR：插值算法（双线性插值，平衡速度和画质）

    if(!sws_){
        qWarning()<<"[AvNetEncoder] sws_getContext failed";
        close();
        return false;
    }

    opened_=true;           // 标记编码器已打开
    vFrameIndex_=0;         // 视频帧索引
    vClockStarted_=false;   // 时钟未启动
    lastPtsMs_=0;           // 上一帧的 PTS（毫秒）
    qDebug()<<"[AvNetEncoder] openVideo OK"<<w_<<h_<<"@"<<fps_;
    return true;
}

void AvNetEncoder::close()
{
    opened_=false;
    if(sws_){sws_freeContext(sws_);sws_=nullptr;}
    if(pkt_){av_packet_free(&pkt_);}
    if(yuv_){av_frame_free(&yuv_);}
    if(vCtx_){avcodec_free_context(&vCtx_);}
    vCodec_=nullptr;
}
//编码单帧RGB图像
void AvNetEncoder::pushVideoFrame(const QImage &img)
{
    if(!opened_||!vCtx_) return;
    if(img.isNull()) return;

    //只接受RGB888,其他格式先转
    QImage rgb=img.format()==QImage::Format_RGB888?img:img.convertToFormat(QImage::Format_RGB888);

    if(rgb.width()!=w_||rgb.height()!=h_){  // 图像尺寸不符则缩放
        rgb=rgb.scaled(w_,h_,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    }

    //准备YUV帧缓冲区
    av_frame_make_writable(yuv_);   // 确保 YUV 帧可写（避免只读）
    // 源数据指针：RGB 图像数据，建了适配 4 通道图像处理接口的数据源结构，仅填充 RGB 通道的有效指针和行字节数，其余通道置空 / 0
    const uint8_t* srcData[4]={rgb.constBits(),nullptr,nullptr,nullptr};
    int srcLinesize[4]={static_cast<int>(rgb.bytesPerLine()),0,0,0};
    // RGB 转 YUV420P
    sws_scale(sws_,srcData,srcLinesize,0,h_,yuv_->data,yuv_->linesize);
    const int gop=qMax(1,fps_);
    //强制关键帧：第一帧+每GOP帧（1秒）一帧
    const bool forceKey=(vFrameIndex_==0||(vFrameIndex_%gop==0));
    if(forceKey){
        yuv_->pict_type=AV_PICTURE_TYPE_I;//强制I帧
    }else{
        yuv_->pict_type=AV_PICTURE_TYPE_NONE;//自动选择帧类型
    }
    yuv_->key_frame=forceKey?1:0;// 标记是否为关键帧
    if(!vClockStarted_){
        vClock_.start();    // 启动 Qt 计时器（计算 PTS）
        vClockStarted_=true;
    }
    qint64 ptsMs=vClock_.elapsed(); // 自计时器启动后的毫秒数
    if(ptsMs<=lastPtsMs_) ptsMs=lastPtsMs_+1;   // 保证 PTS 递增
    lastPtsMs_=ptsMs;
    // 转换 PTS 到编码器时间基（1/1000）
    yuv_->pts=av_rescale_q(ptsMs,AVRational{1,1000},vCtx_->time_base);
    vFrameIndex_++;

    //发送帧到编码器 & 接收编码后数据包
    int ret=avcodec_send_frame(vCtx_,yuv_);
    if(ret<0) return;

    while(true){
        ret=avcodec_receive_packet(vCtx_,pkt_); // 接收编码后的数据包
        if(ret==AVERROR(EAGAIN)||ret==AVERROR_EOF) break;   // 无数据/结束，退出循环
        if(ret<0) break;

        //计算数据包的 PTS（毫秒）
        int64_t pktPts=(pkt_->pts!=AV_NOPTS_VALUE)?pkt_->pts:yuv_->pts;
        const quint32 pts_ms=(quint32)av_rescale_q(pktPts,vCtx_->time_base,AVRational{1,1000});
        // 将编码数据转为 QByteArray
        QByteArray ba(reinterpret_cast<const char*>(pkt_->data),pkt_->size);
        //一旦extradata出现，立刻发一次SPS/PPS配置包(AnnexB)
        //首次生成编码包时，检查 extradata（SPS/PPS），转换为 AnnexB 格式后通过信号发送（接收端需要先解码 SPS/PPS 才能解析后续码流）
        //视频编码的配置数据（SPS/PPS）有两种常见格式：
        //AnnexB：以 00 00 00 01 分隔符开头，是 RTSP/RTMP 等流媒体常用格式；
        //AVCC：是 MP4/FLV 等封装格式的配置数据格式，需要转换为 AnnexB 才能用于流媒体传输
        if(!configEmitted&&vCtx_->extradata&&vCtx_->extradata_size>0){
            QByteArray cfg;
            if(isAnnexB(vCtx_->extradata,vCtx_->extradata_size)){
                cfg=QByteArray(reinterpret_cast<const char*>(vCtx_->extradata),vCtx_->extradata_size);
            }else{
                cfg=avccExtradataToAnnexB(vCtx_->extradata,vCtx_->extradata_size);
            }
            if(!cfg.isEmpty()){
                configEmitted=true;
                qDebug()<<"[AvNetEncoder] extradata ready after packet,emit config size="
                         <<cfg.size()<<"first16="<<cfg.left(16).toHex();
                emit videoPacketReady(cfg,0);
            }
        }
        emit videoPacketReady(ba,pts_ms);
        av_packet_unref(pkt_);
    }
}

void AvNetEncoder::setX2640pts_(AVCodecContext *c, int fps)
{
    //低延迟+关键：每个关键帧前重复SPS/PPS,并输出AnnexB
    av_opt_set(c->priv_data,"preset","ultrafast",0);
    av_opt_set(c->priv_data,"tune","zerolatency",0);
    av_opt_set(c->priv_data,"repeat-headers","1",0);
    av_opt_set(c->priv_data,"annexb","1",0);
    //GOP:1秒一个IDR，方便接收端中途加入
    const int gop=qMax(1,fps);
    av_opt_set_int(c->priv_data,"keyint",gop,0);
    av_opt_set_int(c->priv_data,"min-keyint",gop,0);
    av_opt_set(c->priv_data,"scenecut","0",0);
}
//AVCC 转 AnnexB 格式，读取 AVCC 格式的 SPS/PPS 长度和数据，在每个 NALU（视频编码） 前添加 AnnexB 起始码，拼接成 AnnexB 格式的配置数据
QByteArray AvNetEncoder::avccExtradataToAnnexB(const uint8_t *extra, int extraSize)
{
    QByteArray out;
    if(!extra||extraSize<7) return out;// 长度不足，返回空

    int pos=0;
    if(extra[pos]!=1) return out;   // AVCC 格式首字节必须是 1（版本号）
    pos+=5; // 跳过版本号（1）+ 配置文件 + 级别 + 保留位（共 5 字节）
    //在 H.264 码流解析中，SPS 数量被规定存储在该字节的低 5 位，因此用这个掩码提取是符合协议的标准做法
    int numSps=extra[pos]&0x1F; // SPS 数量（低 5 位）通过 & 0x1F 这个按位与操作，过滤掉字节的高 3 位，只保留低 5 位的有效数据。
    pos++;

    // 遍历所有 SPS
    for(int i=0;i<numSps;++i){
        if(pos+2>extraSize) return QByteArray();    // 长度不足
        // SPS 长度（2 字节）
        int spslen=(extra[pos]<<8) | extra[pos+1];
        pos+=2;
        if(pos+spslen>extraSize) return QByteArray();
        out.append("\x00\x00\x00\x01",4);   // AnnexB 起始码
        out.append(reinterpret_cast<const char*>(extra+pos),spslen);    // PPS 数据
        pos+=spslen;
    }

    if(pos+1>extraSize)  return QByteArray();
    int numPps=extra[pos];  // PPS 数量
    pos++;
    // 遍历所有 PPS
    for(int i=0;i<numPps;++i){
        if(pos+2>extraSize) return QByteArray();
        int ppsLen=(extra[pos]<<8) | extra[pos+1];
        pos+=2;
        if(pos+ppsLen>extraSize) return QByteArray();
        out.append("\x00\x00\x00\x01",4);
        out.append(reinterpret_cast<const char*>(extra+pos),ppsLen);
        pos+=ppsLen;
    }

    return out;
}

static bool isAnnexB(const uint8_t* p,int n){
    if(!p||n<4)  return false;
    // 检查是否以 00 00 01 或 00 00 00 01 开头
    return (p[0]==0&&p[1]==0&&p[2]==1)||(p[0]==0&&p[1]==0&&p[2]==0&&p[3]==1);
}
//主动触发配置包（SPS/PPS）发送，补充 pushVideoFrame 中自动发送的逻辑（如外部需要手动触发时调用）
void AvNetEncoder::emitVideoConfigOnce()
{
    if(!vCtx_||configEmitted) return;   // 无上下文/已发送，返回
    if(!vCtx_->extradata||vCtx_->extradata_size<=0){
        qWarning()<<"[AvNetEncoder] no extradata";
        return;
    }

    const uint8_t* extra=vCtx_->extradata;
    const int extraSize=vCtx_->extradata_size;

    QByteArray cfg;

    if(isAnnexB(extra,extraSize)){
        //extradata已经是AnnexB
        cfg=QByteArray(reinterpret_cast<const char*>(extra),extraSize);
        qDebug()<<"[AvNetEncoder] extradata is AnnexB,send directly size="<<cfg.size()
                 <<"first16="<<cfg.left(16).toHex();
    }else{
        //AVCC才需要转换
        cfg=avccExtradataToAnnexB(extra,extraSize);
        qDebug()<<"[AvNetEncoder] extradata is AVCC,converted size="<<cfg.size()
                 <<"first16="<<cfg.left(16).toHex();
    }

    if(cfg.isEmpty()){
        qWarning()<<"[AvNetEncoder] config empty,not sent";
        return;
    }

    configEmitted=true;
    emit videoPacketReady(cfg,0);
}

