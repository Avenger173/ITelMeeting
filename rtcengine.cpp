#include "rtcengine.h"
#include<QDebug>
#include<QElapsedTimer>
#include<cstring>

RtcEngine::RtcEngine(QObject *parent)
    : QObject{parent}
{}

RtcEngine::~RtcEngine()
{
    //先把本地音视频从收发器上解绑，避免残留回调
    if(videoTrans_){
        mrsTransceiverSetLocalVideoTrack(videoTrans_,nullptr);
    }
    if(audioTrans_){
        mrsTransceiverSetLocalAudioTrack(audioTrans_,nullptr);
    }
    //关闭PeerConnection
    if(pc_){
        mrsPeerConnectionClose(pc_);
        pc_=nullptr;
    }
    //关闭其他句柄
    videoTrans_=nullptr;
    audioTrans_=nullptr;
    localVideo_=nullptr;
    localAudio_=nullptr;
    decVideoSrc_=nullptr;
    devAudioSrc_=nullptr;
}

bool RtcEngine::init(const QString &stun)
{
    if(pc_) return true;
    mrsPeerConnectionConfiguration cfg{};
    std::memset(&cfg,0,sizeof(cfg));
    //直接字符串配置
    const QByteArray stunUtf8=stun.toUtf8();
    cfg.encoded_ice_servers=stunUtf8.constData();
    //创建PC
    auto res=mrsPeerConnectionCreate(&cfg,&pc_);
    if(res!=mrsResult::kSuccess||!pc_){
        emit logMsg("[RTC] PeerConnectionCreate failed");
        return false;
    }
    //注册SDP&ICE回调
    mrsPeerConnectionRegisterLocalSdpReadytoSendCallback(pc_,&RtcEngine::onLocalSdpReadytoSend,this);
    mrsPeerConnectionRegisterIceCandidateReadytoSendCallback(pc_,&RtcEngine::onIceCandidateReadytoSend,this);
    //连接状态
    mrsPeerConnectionRegisterConnectedCallback(pc_,&RtcEngine::onIceConStateChanged,this);
    //远端视频帧(I420A)回调
    mrsRemoteVideoTrackRegisterI420AFrameCallback(pc_,&RtcEngine::onI420AFrame,this);
    mrsPeerConnectionRegisterTransceiverAddedCallback(pc_,&RtcEngine::onRemoteTrackAdded,this);

    emit logMsg("[RTC] PeerConnection created");
    return true;
}
//本地外部视频
bool RtcEngine::createLocalTracksFromExternalVideo(int width, int height, int fps)
{
    if(!pc_){
        emit logMsg("[RTC] init() first");
        return false;
    }
    if(localVideo_) return true;
    //外部视频源(ARGB32)
    mrsExternalVideoTrackSourceConfig
}

MRS_CALL RtcEngine::onRemoteTrackAdded(void *user, mrsRemoteVideoTrackHandle track)
{
    auto self = reinterpret_cast<RtcEngine*>(user);
    if (!self || !track) return;

    mrsRemoteVideoTrackRegisterI420AFrameCallback(track, &RtcEngine::onI420AFrame, self);

    emit self->logMsg("[RTC] Remote video track added and frame callback set");
}



MRS_CALL RtcEngine::onIceConStateChanged(void *user, mrsIceConnectionState state)
{
    auto self=reinterpret_cast<RtcEngine*>(user);
    if(!self) return;

    switch(state){
    case MRS_ICE_CONNECTION_STATE_CHECKING:
        emit self->logMsg("[RTC] ICE: CHECKING"); break;
    case MRS_ICE_CONNECTION_STATE_CONNECTED:
        emit self->logMsg("[RTC] ICE: CONNECTED");break;
    case MRS_ICE_CONNECTION_STATE_COMPLETED:
        emit self->logMsg("[RTC] ICE: COMPLETED");break;
    case MRS_ICE_CONNECTION_STATE_DISCONNECTED:
        emit self->logMsg("[RTC] ICE: DISCONNECTED");break;
    case MRS_ICE_CONNECTION_STATE_FAILED:
        emit self->logMsg("[RTC] ICE: FAILED");break;
    case MRS_ICE_CONNECTION_STATE_CLOESD:
        emit self->logMsg("[RTC] ICE: CLOESD");break;
    default:
        emit self->logMsg("[RTC] ICE: (unknown)");
    }
}
