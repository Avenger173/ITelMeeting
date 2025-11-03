#ifndef RTCENGINE_H
#define RTCENGINE_H

#include <QObject>
#include<QImage>
#include<QByteArray>
#include<QMutex>
#include<QPointer>
#include<QTimer>
#include"rtcsignaling.h"
//FFmpeg swscale/swr 仅用于把Qt 图像/pcm转为webRTC需要的格式
extern "C"{
#include<libswscale/swscale.h>
#include<libswresample/swresample.h>
#include<mrwebrtc/peer_connection_interop.h>
#include<mrwebrtc/audio_track_source_interop.h>
#include<mrwebrtc/video_track_source_interop.h>
#include<mrwebrtc/audio_frame.h>
#include<mrwebrtc/video_frame.h>
#include<mrwebrtc/external_video_track_source_interop.h>
#include<mrwebrtc/local_audio_track_interop.h>
#include<mrwebrtc/local_video_track_interop.h>
#include<mrwebrtc/transceiver_interop.h>
#include<mrwebrtc/data_channel_interop.h>
#include<mrwebrtc/object_interop.h>
#include<mrwebrtc/result.h>
#include<mrwebrtc/device_audio_track_source_interop.h>
#include<mrwebrtc/remote_video_track_interop.h>
#include<mrwebrtc/remote_audio_track_interop.h>
#include<mrwebrtc/external_video_track_source_interop.h>
}



namespace mswebrtc=Microsoft::MixedReality::WebRTC;

class RtcEngine : public QObject
{
    Q_OBJECT
public:
    explicit RtcEngine(QObject *parent = nullptr);
    ~RtcEngine();
    //初始化peerconnection（可在UI“开始会议”时使用）
    bool init(const QString& stun="stun:stun.l.google.com:19302");
    //创建/挂接本地媒体
    bool createLocalTracksFromExternalVideo(int width,int height,int fps);//视频由外部QImage推送
    bool createLocalAudioFromDevice();//音频由设备采集
    //呼叫/应答
    bool createOffer();
    bool createAnswer();
    bool setRemoteDescription(const QString& type,const QString& sdp);
    bool addRemoteIceCandidate(const QString& sdpMid,int sdpMlineIndex,const QString& candidate);
    //外部视频帧推送(QImage->ARGB32->EXternalVideoTrackSource)
    void pushlocalVideoFrame(const QImage& img);
signals:
    //给UI/信令
    void localSdpReady(const QString& type,const QString& sdp);
    void iceCandidateReady(const QString& sdpMid,int sdpMLineIndex,const QString& candidate);
    //给UI播放
    void remoteVideoFrame(const QImage& img);
    void logMsg(const QString& msg);
private:
    //回调静态桥
    static void MRS_CALL onLocalSdpReadytoSend(void* user_data, const char* type, const char* sdp);
    static void MRS_CALL onIceCandidateReadytoSend(void* user_data, const char* mid, int mlineindex, const char* cand);
    static void MRS_CALL onI420AFrame(void* user_data, const mrsI420AVideoFrame* frame);
    static void MRS_CALL onRemoteTrackAdded(void* user,mrsRemoteVideoTrackHandle track);
    static void MRS_CALL onIceConStateChanged(void* user,mrsIceConnectionState state);
    static void MRS_CALL onConnectionStateChanged(void* user);
    static void MRS_CALL onConnected(void* user_data);
    static void MRS_CALL onDisconnected(void* user_data);

    void handleRemoteI420AFrame(const mrsI420AVideoFrame* f);
private:
    //核心句柄
    mrsPeerConnectionHandle pc_=nullptr;
    //本地视频:外部推送
    mrsDeviceVideoTrackSourceHandle decVideoSrc_=nullptr;
    mrsLocalVideoTrackHandle localVideo_=nullptr;
    mrsTransceiverHandle videoTrans_=nullptr;
    //本地音频:设备采集
    mrsDeviceAudioTrackSourceHandle devAudioSrc_=nullptr;
    mrsLocalAudioTrackHandle localAudio_=nullptr;
    mrsTransceiverHandle audioTrans_=nullptr;
    //推送时的规格(若视频源不是固定分辨率，可以每帧转入真实宽高)
    int pushW_=0,pushH_=0,fps_=0;
    QMutex pushMtx_;
};

#endif // RTCENGINE_H
