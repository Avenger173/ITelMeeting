#include "avreceiver.h"
#include<QDateTime>
#include<QTimer>
#include<QDebug>
#include<QUdpSocket>
#include<QMediaDevices>
#include<QNetworkDatagram>
AVReceiver::AVReceiver(QObject *parent)
    : QObject{parent},
    avQueue([](const FrameItem &a,const FrameItem &b){
        return a.pts>b.pts;//按时间戳排序(小的优先)
    })
{
    avformat_network_init();
    //定期清理超时部分帧（比如3s未完成）
    QTimer *cleanTimer=new QTimer(this);
    connect(cleanTimer,&QTimer::timeout,this,[this](){
        cleanupStaleBuffers();
    });
    cleanTimer->start(1000);
}

AVReceiver::~AVReceiver()
{
    stop();
    if(sws) sws_freeContext(sws);
    if(swr) swr_free(&swr);
    if(vCodecCtx) avcodec_free_context(&vCodecCtx);
    if(aCodecCtx) avcodec_free_context(&aCodecCtx);
    avformat_network_deinit();
}

bool AVReceiver::start(const QString &host, int videoPort,int audioPort)
{
    Q_UNUSED(host);
    if(videoSocket||audioSocket) return false;


    //视频socket-监听本地端口
    videoSocket=new QUdpSocket(this);
    if(!videoSocket->bind(QHostAddress::AnyIPv4,videoPort,QUdpSocket::ShareAddress|QUdpSocket::ReuseAddressHint)){
        emit logMsg("[Receiver] 视频端口连接失败");
        delete videoSocket;
        videoSocket=nullptr;
        return false;
    }
    connect(videoSocket,&QUdpSocket::readyRead,this,&AVReceiver::onVideoReadyRead);

    //音频socket
    audioSocket=new QUdpSocket(this);
    if(!audioSocket->bind(QHostAddress::AnyIPv4,audioPort,QUdpSocket::ShareAddress|QUdpSocket::ReuseAddressHint)){
        emit logMsg("[Receiver] 音频端口连接失败");
        delete audioSocket;
        audioSocket=nullptr;
        //同时把videoSocket清理了
        videoSocket->deleteLater();
        videoSocket=nullptr;
        return false;
    }
    connect(audioSocket,&QUdpSocket::readyRead,this,&AVReceiver::onAudioReadyRead);

    emit logMsg("[Receiver] 已连接到发送端(音频+视频)");
    initCodecs();

    //音频输出初始化
    audioFmt.setSampleRate(44100);
    audioFmt.setChannelCount(1);
    audioFmt.setSampleFormat(QAudioFormat::Int16);

    if(!QMediaDevices::defaultAudioOutput().isFormatSupported(audioFmt)){
        audioFmt=QMediaDevices::defaultAudioOutput().preferredFormat();
        emit logMsg("[Receiver] 使用推荐的音频格式");
    }
    audioSink=new QAudioSink(audioFmt,this);
    audioOut=audioSink->start();

    startTimeMs=QDateTime::currentMSecsSinceEpoch();
    return true;
}

void AVReceiver::stop()
{
    if(videoSocket){
        videoSocket->disconnectFromHost();
        videoSocket->deleteLater();
        videoSocket=nullptr;
    }
    if(audioSocket){
        audioSocket->disconnectFromHost();
        audioSocket->deleteLater();
        audioSocket=nullptr;
    }
    if(audioSink){
        audioSink->stop();
        delete audioSink;
        audioSink=nullptr;
        audioOut=nullptr;
    }

    //清理buffers
    QMutexLocker locker(&recvBuffersMutex);
    recvBuffers.clear();
}

void AVReceiver::onVideoReadyRead()
{
    while(videoSocket&&videoSocket->hasPendingDatagrams()){
        QNetworkDatagram dg=videoSocket->receiveDatagram();
        qDebug()<<"[Receiver] got datagram size="<<dg.data().size()
                 <<"from"<<dg.senderAddress().toString()
                 <<":"<<dg.senderPort();
        QByteArray dat=dg.data();
        if(dat.size()<HEADER_SIZE) continue;//太小

        QDataStream ds(dat);
        ds.setByteOrder(QDataStream::BigEndian);
        quint32 frameId;
        quint16 chunkId,chunkCount;
        quint32 pts_ms,totalSize;
        ds>>frameId;
        ds>>chunkId;
        ds>>chunkCount;
        ds>>pts_ms;
        ds>>totalSize;

        //打印日志
        qDebug()<<"[Receiver] got datagram size="<<dat.size()
                 <<"from"<<dg.senderAddress().toString()<<":"<<dg.senderPort()
                 <<"header(fId/chunkId/ccount/pts/total)="
                 <<frameId<<chunkId<<chunkCount<<pts_ms<<totalSize;
        const int payloadSize=dat.size()-HEADER_SIZE;
        const char*payloadPtr=dat.constData()+HEADER_SIZE;

        QMutexLocker locker(&recvBuffersMutex);
        FrameBuffer &fb=recvBuffers[frameId];
        if(fb.frameId==0){
            //初始化
            fb.frameId=frameId;
            fb.pts_ms=pts_ms;
            fb.totalSize=totalSize;
            fb.chunkCount=chunkCount;
            fb.receivedCount=0;
            fb.buffer.resize(totalSize);
            fb.receivedMap=QVector<char>(chunkCount,0);
            fb.lastUpdateMs=QDateTime::currentMSecsSinceEpoch();
        }
        if(chunkId<fb.chunkCount){
            int offset=chunkId*CHUNK_SIZE;
            int copySize=payloadSize;
            if(offset+copySize>static_cast<int>(fb.totalSize)){
                copySize=static_cast<int>(fb.totalSize)-offset;
                if(copySize<=0) continue;
            }
            memcpy(fb.buffer.data()+offset,payloadPtr,payloadSize);
            if(!fb.receivedMap[chunkId]){
                fb.receivedMap[chunkId]=1;
                fb.receivedCount++;
            }
            fb.lastUpdateMs=QDateTime::currentMSecsSinceEpoch();
        }
        //完整收到，提交解码（视频）
        if(fb.receivedCount==fb.chunkCount){
            QByteArray fullPacket=fb.buffer;
            recvBuffers.remove(frameId);
            //decode
            decodePacket(fullPacket,true);
            }
        }
    playQueuedFrames();
}
void AVReceiver::onAudioReadyRead()
{
    while(audioSocket&&audioSocket->hasPendingDatagrams()){
        QNetworkDatagram dg=audioSocket->receiveDatagram();
        QByteArray dat=dg.data();
        if(dat.size()<HEADER_SIZE) continue;//太小

        QDataStream ds(dat);
        ds.setByteOrder(QDataStream::BigEndian);
        quint32 frameId;
        quint16 chunkId,chunkCount;
        quint32 pts_ms,totalSize;
        ds>>frameId;
        ds>>chunkId;
        ds>>chunkCount;
        ds>>pts_ms;
        ds>>totalSize;

        //打印日志
        qDebug()<<"[Receiver] got datagram size="<<dat.size()
                 <<"from"<<dg.senderAddress().toString()<<":"<<dg.senderPort()
                 <<"header(fId/chunkId/ccount/pts/total)="
                 <<frameId<<chunkId<<chunkCount<<pts_ms<<totalSize;
        const int payloadSize=dat.size()-HEADER_SIZE;
        const char*payloadPtr=dat.constData()+HEADER_SIZE;

        QMutexLocker locker(&recvBuffersMutex);
        FrameBuffer &fb=recvBuffers[frameId];
        if(fb.frameId==0){
            //初始化
            fb.frameId=frameId;
            fb.pts_ms=pts_ms;
            fb.totalSize=totalSize;
            fb.chunkCount=chunkCount;
            fb.receivedCount=0;
            fb.buffer.resize(totalSize);
            fb.receivedMap=QVector<char>(chunkCount,0);
            fb.lastUpdateMs=QDateTime::currentMSecsSinceEpoch();
        }
        if(chunkId<fb.chunkCount){
            int offset=chunkId*CHUNK_SIZE;
            int copySize=payloadSize;
            if(offset+copySize>static_cast<int>(fb.totalSize)){
                copySize=static_cast<int>(fb.totalSize)-offset;
                if(copySize<=0) continue;
            }
            memcpy(fb.buffer.data()+offset,payloadPtr,payloadSize);
            if(!fb.receivedMap[chunkId]){
                fb.receivedMap[chunkId]=1;
                fb.receivedCount++;
            }
            fb.lastUpdateMs=QDateTime::currentMSecsSinceEpoch();
        }
        //完整收到，提交解码（音频）
        if(fb.receivedCount==fb.chunkCount){
            QByteArray fullPacket=fb.buffer;
            recvBuffers.remove(frameId);
            //decode
            decodePacket(fullPacket,true);
            //直接写到audioOut
            if(audioOut){
                audioOut->write(fullPacket);
                emit newAudioPCM(fullPacket);//给ui或日志
                qDebug()<<"[Receiver] 播放音频字节数:"<<fullPacket.size();
            }
        }
    }
    playQueuedFrames();
}
void AVReceiver::initCodecs()
{
    const AVCodec *vcodec=avcodec_find_decoder(AV_CODEC_ID_H264);
    vCodecCtx=avcodec_alloc_context3(vcodec);
    avcodec_open2(vCodecCtx,vcodec,nullptr);

    const AVCodec *acodec=avcodec_find_decoder(AV_CODEC_ID_AAC);
    aCodecCtx=avcodec_alloc_context3(acodec);
    avcodec_open2(aCodecCtx,acodec,nullptr);

    //为音频解码后的重采样保留swr(若需要)
    swr=swr_alloc();
    //这里在decodeAudio时根据frame初始化swr参数
}

void AVReceiver::decodePacket(const QByteArray &raw,bool isVideo)
{
    if(raw.isEmpty()) return;

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data=reinterpret_cast<uint8_t*>(const_cast<char*>(raw.data()));
    pkt.size=raw.size();

    if(isVideo){//视频
        int ret=avcodec_send_packet(vCodecCtx,&pkt);
        if(ret<0){
            qWarning()<<"[Receiver] video avcode_send_packet failed:"<<ret;

            return;
        }
        AVFrame *frame=av_frame_alloc();
        while((ret=avcodec_receive_frame(vCodecCtx,frame))>=0){
            handleVideoFrame(frame);
        }
        if(ret!=AVERROR(EAGAIN)&&ret!=AVERROR_EOF){
            qWarning()<<"[Receiver]video avcodec_receive_frame failed:"<<ret;
        }
        av_frame_free(&frame);
    }else{//音频
        int ret=avcodec_send_packet(aCodecCtx,&pkt);
        if(ret<0){
            qWarning()<<"[Receiver] audio avcodec_send_packet failed:"<<ret;
            return;
        }
        AVFrame *frame=av_frame_alloc();
        while((ret=avcodec_receive_frame(aCodecCtx,frame))>=0){
            handleAudioFrame(frame);
        }
        if(ret!=AVERROR(EAGAIN)&&ret!=AVERROR_EOF){
            qWarning()<<"[Receiver] audio avcodec_receiver_frame failed:"<<ret;
        }
        av_frame_free(&frame);
    }
}

void AVReceiver::handleVideoFrame(AVFrame *frame)
{
    if(!sws){
        sws=sws_getContext(
            frame->width,frame->height,(AVPixelFormat)frame->format
            ,frame->width,frame->height,AV_PIX_FMT_RGB24
            ,SWS_BICUBIC,nullptr,nullptr,nullptr);
    }

    QImage img(frame->width,frame->height,QImage::Format_RGB888);
    uint8_t *dst[1]={img.bits()};
    int dstLinesize[1]={static_cast<int>(img.bytesPerLine())};

    sws_scale(sws,frame->data,frame->linesize,0,frame->height,dst,dstLinesize);
    FrameItem item;
    item.pts=frame->pts;
    item.img=img;
    item.isAudio=false;

    QMutexLocker locker(&queueMutex);
    avQueue.push(item);
}

void AVReceiver::handleAudioFrame(AVFrame *frame)
{
    if(!frame) return;

    //1.获取声道数
    int ch=1;
    if(frame->ch_layout.nb_channels>0){
        ch=frame->ch_layout.nb_channels;
    }else if(aCodecCtx&&aCodecCtx->ch_layout.nb_channels>0){
        //回退到解码器上下文已知通道数
        ch=aCodecCtx->ch_layout.nb_channels;
    }else{
        qWarning()<<"[Receiver] 无法确定声道数,使用默认1(mono)";
    }

    //2.输入样本格式与每样本字节
    AVSampleFormat inFmt=static_cast<AVSampleFormat>(frame->format);
    int bytesPerSample=av_get_bytes_per_sample(inFmt);
    if(bytesPerSample<=0) bytesPerSample=2;//保险兜底

    QByteArray pcm;//用来存放最终的interleaved S16 PCM数据

    //3.如果是planner,需要用swr转为interleaved S16
    if(av_sample_fmt_is_planar(inFmt)){
        //准备一个临时swr,上下文按frame的ch_layout/sample_rate/farmat转换到S16interleaved
        SwrContext *localSwr=swr_alloc();
        if(!localSwr){
            qWarning()<<"[Receiver] swr_alloc failed";
            return;
        }

        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout,ch);//输出layout:按通道数生成
        //输出目标格式：S16
        AVSampleFormat outFmt=AV_SAMPLE_FMT_S16;

        if(swr_alloc_set_opts2(&localSwr,
                                &outLayout,outFmt,frame->sample_rate,
                                &frame->ch_layout,inFmt,frame->sample_rate,
                                0,nullptr)<0){
            qWarning()<<"[Receiver]swr_alloc_set_opts failed";
            swr_free(&localSwr);
            av_channel_layout_uninit(&outLayout);
            return;
        }
        if(swr_init(localSwr)<0){
            qWarning()<<"[Receiver]swr_init failed";
            swr_free(&localSwr);
            av_channel_layout_uninit(&outLayout);
            return;
        }
        //估算输出样本数与缓冲区大小
        int outSamples=swr_get_out_samples(localSwr,frame->nb_samples);
        if(outSamples<=0) outSamples=frame->nb_samples;
        int outBufSize=av_samples_get_buffer_size(nullptr,ch,outSamples,outFmt,1);
        if(outBufSize<=0) outBufSize=frame->nb_samples*ch*2;

        pcm.resize(outBufSize);
        uint8_t *outPtr=reinterpret_cast<uint8_t*>(pcm.data());

        //输入为planner,使用frame->extended_data作为属于指针数组
        const uint8_t **in=const_cast<const uint8_t**>(frame->extended_data);

        int converted=swr_convert(localSwr,&outPtr,outSamples,in,frame->nb_samples);
        if(converted<0){
            qWarning()<<"[Receiver]swr_convert failed:"<<avErr(converted);
            //仍然释放资源并返回(不要push空数据)
            swr_free(&localSwr);
            av_channel_layout_uninit(&outLayout);
            return;
        }

        //swr_converted可能返回实际转出的样本数，重新计算实际字节数
        int actualBytes=av_samples_get_buffer_size(nullptr,ch,converted,outFmt,1);
        if(actualBytes>0&&actualBytes<=pcm.size()){
            pcm.resize(actualBytes);
        }
        //释放临时资源
        swr_free(&localSwr);
        av_channel_layout_uninit(&outLayout);
    }else{
        //4.interleaved格式，直接拷贝frame->data[0]
        int dataSize=frame->nb_samples*ch*bytesPerSample;
        if(dataSize>0){
            pcm.resize(dataSize);
            memcpy(pcm.data(),frame->data[0],dataSize);
        }else{
            //保险：若计算异常，不push
            qWarning()<<"[Receiver] unexpected audio dataSize<=0";
            return;
        }
    }

    //5.push到播放队列
    FrameItem item;
    item.pts=frame->pts;
    item.pcm=pcm;
    item.isAudio=true;

    QMutexLocker locker(&queueMutex);
    avQueue.push(item);
}

void AVReceiver::playQueuedFrames()
{
    qint64 now=QDateTime::currentMSecsSinceEpoch()-startTimeMs;

    while(!avQueue.empty()){
        FrameItem item=avQueue.top();
        qint64 ptsMs=0;
        if(item.isAudio){
            if(aCodecCtx&&item.pts!=AV_NOPTS_VALUE){
                ptsMs=av_rescale_q(item.pts,aCodecCtx->time_base,AVRational{1,1000});
            }else{
                //按原来的近似处理
                ptsMs=item.pts*1000/90000;
            }
        }else{
            if(vCodecCtx&&item.pts!=AV_NOPTS_VALUE){
                ptsMs=av_rescale_q(item.pts,vCodecCtx->time_base,AVRational{1,1000});
            }else{
                ptsMs=item.pts*1000/90000;
            }
        }

        if(ptsMs>now) break;//未到播放时间

        avQueue.pop();
        if(item.isAudio){
            if(audioOut&&!item.pcm.isEmpty()){
                audioOut->write(item.pcm);
                qDebug()<<"[Receiver] play audio bytes:"<<item.pcm.size()<<"ptsMs="<<ptsMs<<"now="<<now;
            }
            emit newAudioPCM(item.pcm);
        }else{
            qDebug()<<"[Receiver] emit video frame ptsMs="<<ptsMs<<"now="<<now;
            emit newVideoFrame(item.img);
        }
    }
}
void AVReceiver::cleanupStaleBuffers()
{
    QMutexLocker locker(&recvBuffersMutex);
    qint64 now=QDateTime::currentMSecsSinceEpoch();
    QList<quint32> toRemove;
    for(auto it=recvBuffers.begin();it!=recvBuffers.end();++it){
        if(now-it->lastUpdateMs>3000) toRemove.append(it.key());
    }
    for(auto k:toRemove) recvBuffers.remove(k);
}

QString AVReceiver::avErr(int errnum)
{
    char buf[256];
    av_strerror(errnum,buf,sizeof(buf));
    return QString::fromUtf8(buf);
}

