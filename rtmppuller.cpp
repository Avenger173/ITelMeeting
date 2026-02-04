#include "rtmppuller.h"
#include<QDebug>
#include<QMediaDevices>

static QString ffErr(int err){
    char buf[256]={0};
    av_strerror(err,buf,sizeof(buf));
    return QString::fromUtf8(buf);
}

rtmppuller::rtmppuller(QObject *parent)
    : QObject{parent}
{
    avformat_network_init();
}

rtmppuller::~rtmppuller()
{
    stop();
    avformat_network_deinit();
}

void rtmppuller::startPull(const QString &url)
{
    stopFlag.storeRelease(false);
    cleanup();

    qInfo()<<"[RtmpPuller] start ->"<<url;

    AVDictionary* opts=nullptr;
    av_dict_set(&opts,"fflags","nobuffer",0);
    av_dict_set(&opts,"flags","low_delay",0);
    av_dict_set(&opts,"analyzeduration","100000",0);
    av_dict_set(&opts,"probesize","16384",0);
    av_dict_set(&opts,"reorder_queue_size","0",0);
    av_dict_set(&opts,"max_delay","0",0);
    av_dict_set(&opts,"rtmp_live","live",0);

    int ret=avformat_open_input(&fmtCtx,url.toUtf8().constData(),nullptr,&opts);
    av_dict_free(&opts);
    if(ret<0){
        emit errorOccurred("open_input:"+ffErr(ret));
        cleanup();
        return;
    }

    ret=avformat_find_stream_info(fmtCtx,nullptr);
    if(ret<0){
        emit errorOccurred("find_stream_info:"+ffErr(ret));
        cleanup();
        return;
    }

    for(unsigned i=0;i<fmtCtx->nb_streams;i++){
        if(fmtCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO){
            vStream=(int)i;
            break;
        }
    }
    for(unsigned i=0;i<fmtCtx->nb_streams;i++){
        if(fmtCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO){
            aStream=(int)i;
            break;
        }
    }
    if(vStream<0){
        emit errorOccurred("no video stream");
        cleanup();
        return;
    }

    auto* par=fmtCtx->streams[vStream]->codecpar;
    const AVCodec* dec=avcodec_find_decoder(par->codec_id);
    if(!dec){
        emit errorOccurred("find_decoder failed");
        cleanup();
        return;
    }

    vDecCtx=avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(vDecCtx,par);

    ret=avcodec_open2(vDecCtx,dec,nullptr);
    if(ret<0){
        emit errorOccurred("open2:"+ffErr(ret));
        cleanup();
        return;
    }
    if(aStream>=0){
        auto* apar=fmtCtx->streams[aStream]->codecpar;
        const AVCodec* adec=avcodec_find_decoder(apar->codec_id);
        if(adec){
            aDecCtx=avcodec_alloc_context3(adec);
            avcodec_parameters_to_context(aDecCtx,apar);
            if(avcodec_open2(aDecCtx,adec,nullptr)==0){
                aFrame=av_frame_alloc();

                const int inCh=aDecCtx->ch_layout.nb_channels>0?aDecCtx->ch_layout.nb_channels:1;
                const QAudioDevice dev=QMediaDevices::defaultAudioOutput();
                QAudioFormat desired;
                desired.setSampleRate(aDecCtx->sample_rate);
                desired.setChannelCount(inCh);
                desired.setSampleFormat(QAudioFormat::Int16);
                if(dev.isFormatSupported(desired)){
                    playFmt_=desired;
                }else{
                    playFmt_=dev.preferredFormat();
                }

                audioSink_=new QAudioSink(dev,playFmt_);
                audioSink_->setBufferSize(playFmt_.bytesForDuration(60000)); // 60ms
                audioOut_=audioSink_->start();

                AVSampleFormat outFmt=AV_SAMPLE_FMT_S16;
                if(playFmt_.sampleFormat()==QAudioFormat::Int16) outFmt=AV_SAMPLE_FMT_S16;
                else if(playFmt_.sampleFormat()==QAudioFormat::Float) outFmt=AV_SAMPLE_FMT_FLT;
                else if(playFmt_.sampleFormat()==QAudioFormat::UInt8) outFmt=AV_SAMPLE_FMT_U8;

                AVChannelLayout outLayout;
                av_channel_layout_default(&outLayout,playFmt_.channelCount());

                aswr=swr_alloc();
                swr_alloc_set_opts2(&aswr,
                                    &outLayout,outFmt,playFmt_.sampleRate(),
                                    &aDecCtx->ch_layout,aDecCtx->sample_fmt,aDecCtx->sample_rate,
                                    0,nullptr);
                swr_init(aswr);
                av_channel_layout_uninit(&outLayout);
            }
        }
    }

    sws=sws_getContext(vDecCtx->width,vDecCtx->height,vDecCtx->pix_fmt
                         ,vDecCtx->width,vDecCtx->height,AV_PIX_FMT_RGB24
                         ,SWS_BILINEAR,nullptr,nullptr,nullptr);
    frame=av_frame_alloc();
    rgbFrame=av_frame_alloc();
    pkt=av_packet_alloc();

    int bufSize=av_image_get_buffer_size(AV_PIX_FMT_RGB24,vDecCtx->width,vDecCtx->height,1);
    rgbBuf=(uint8_t*)av_malloc(bufSize);
    av_image_fill_arrays(rgbFrame->data,rgbFrame->linesize,rgbBuf,AV_PIX_FMT_RGB24,vDecCtx->width,vDecCtx->height,1);

    while(!stopFlag.loadAcquire()){
        ret=av_read_frame(fmtCtx,pkt);
        if(ret<0) break;
        if(pkt->stream_index==aStream&&aDecCtx){
            ret=avcodec_send_packet(aDecCtx,pkt);
            av_packet_unref(pkt);
            if(ret>=0){
                while(!stopFlag.loadAcquire()){
                    ret=avcodec_receive_frame(aDecCtx,aFrame);
                    if(ret==AVERROR(EAGAIN)||ret==AVERROR_EOF) break;
                    if(ret<0)   break;

                    int outSamples=swr_get_out_samples(aswr,aFrame->nb_samples);
                    int outCh=playFmt_.channelCount();
                    int outBps=(playFmt_.sampleFormat()==QAudioFormat::Int16)?2:
                                 (playFmt_.sampleFormat()==QAudioFormat::Float)?4:1;
                    int outBufSize=outSamples*outCh*outBps;
                    QByteArray outBuf(outBufSize,0);

                    uint8_t* outData[1]={reinterpret_cast<uint8_t*>(outBuf.data())};
                    int converted=swr_convert(aswr,outData,outSamples,(const uint8_t**)aFrame->data,aFrame->nb_samples);
                    if(converted>0){
                        const int bytes=converted*outCh*outBps;
                        outBuf.resize(bytes);
                        if(audioSink_&&audioOut_){
                            audioPending_.append(outBuf.constData(),bytes);
                            const int maxPending=playFmt_.bytesForDuration(120000); // 120ms
                            if(audioPending_.size()>maxPending){
                                const int drop=audioPending_.size()-maxPending;
                                audioPending_.remove(0,drop);
                            }
                            flushAudioPending_();
                        }
                    }
                    av_frame_unref(aFrame);
                }
            }
            continue;
        }
        if(pkt->stream_index!=vStream){
            av_packet_unref(pkt);
            continue;
        }

        ret=avcodec_send_packet(vDecCtx,pkt);
        av_packet_unref(pkt);
        if(ret<0){
            continue;
        }

        while(!stopFlag.loadAcquire()){
            ret=avcodec_receive_frame(vDecCtx,frame);
            if(ret==AVERROR(EAGAIN)||ret==AVERROR_EOF) break;
            if(ret<0) break;

            sws_scale(sws,frame->data,frame->linesize,0,vDecCtx->height,rgbFrame->data,rgbFrame->linesize);

            QImage img(vDecCtx->width,vDecCtx->height,QImage::Format_RGB888);
            for(int y=0;y<vDecCtx->height;y++){
                memcpy(img.scanLine(y),rgbFrame->data[0]+y*rgbFrame->linesize[0],(size_t)vDecCtx->width*3);
            }
            emit videoFrameReady(img);
            av_frame_unref(frame);
        }
    }

    emit finished();
    cleanup();
}

void rtmppuller::stop()
{
    stopFlag.storeRelease(true);
}

void rtmppuller::cleanup()
{
    if(pkt){
        av_packet_free(&pkt);
        pkt=nullptr;
    }
    if(frame){
        av_frame_free(&frame);
        frame=nullptr;
    }
    if(rgbFrame){
        av_frame_free(&rgbFrame);
        rgbFrame=nullptr;
    }
    if(rgbBuf){
        av_free(rgbBuf);
        rgbBuf=nullptr;
    }
    if(sws){
        sws_freeContext(sws);
        sws=nullptr;
    }
    if(vDecCtx){
        avcodec_free_context(&vDecCtx);
        vDecCtx=nullptr;
    }
    if(fmtCtx){
        avformat_close_input(&fmtCtx);
        fmtCtx=nullptr;
    }
    vStream=-1;
    if(aFrame){
        av_frame_free(&aFrame);
        aFrame=nullptr;
    }
    if(aswr){
        swr_free(&aswr);
        aswr=nullptr;
    }
    if(audioSink_){
        audioSink_->stop();
        delete audioSink_;
        audioSink_=nullptr;
        audioOut_=nullptr;
    }
    if(aDecCtx){
        avcodec_free_context(&aDecCtx);
        aDecCtx=nullptr;
    }
    audioPending_.clear();
    aStream=-1;
}

void rtmppuller::flushAudioPending_()
{
    if(!audioSink_||!audioOut_) return;
    while(!audioPending_.isEmpty()){
        const qint64 freeBytes=audioSink_->bytesFree();
        if(freeBytes<=0) break;
        const int n=qMin<int>(audioPending_.size(),int(freeBytes));
        if(n<=0) break;
        const qint64 w=audioOut_->write(audioPending_.constData(),n);
        if(w<=0) break;
        audioPending_.remove(0,int(w));
    }
}

