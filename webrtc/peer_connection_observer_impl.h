#pragma once

#include "api/peer_connection_interface.h"
#include "api/media_stream_interface.h"
#include "audio_receiver.h"
#include "encoded_video_frame_handler.h"
#include <memory>
#include <functional>

/**
 * @brief WebRTC PeerConnection观察者实现类
 * 
 * 该类负责处理WebRTC连接事件，包括ICE连接状态变化、
 * 媒体轨道添加、数据通道等事件
 */
class PeerConnectionObserverImpl : public webrtc::PeerConnectionObserver {
public:
    /**
     * @brief 构造函数
     * @param client WebRTC客户端指针
     */
    explicit PeerConnectionObserverImpl(class WebRTCClient* client);
    
    /**
     * @brief 设置编码视频帧处理器
     * @param handler 编码视频帧处理器
     */
    void SetEncodedVideoFrameHandler(std::shared_ptr<EncodedVideoFrameHandler> handler) {
        encoded_video_handler_ = handler;
    }
    
    /**
     * @brief 设置音频接收器
     * @param receiver 音频接收器
     */
    void SetAudioReceiver(std::shared_ptr<AudioReceiver> receiver) {
        audio_receiver_ = receiver;
    }

    // PeerConnectionObserver接口实现
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override;
    void OnAddTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                   const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) override;
    void OnRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override;
    void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
    void OnRenegotiationNeeded() override;
    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
    void OnIceConnectionReceivingChange(bool receiving) override;

private:
    /**
     * @brief 处理音频轨道
     * @param track 音频轨道
     */
    void ProcessAudioTrack(webrtc::AudioTrackInterface* track);
    
    /**
     * @brief 处理视频轨道
     * @param track 视频轨道
     */
    void ProcessVideoTrack(webrtc::VideoTrackInterface* track);

    // WebRTC客户端指针
    class WebRTCClient* client_;
    
    // 编码视频帧处理器
    std::shared_ptr<EncodedVideoFrameHandler> encoded_video_handler_;
    
    // 音频接收器
    std::shared_ptr<AudioReceiver> audio_receiver_;
    
    // 当前音频轨道
    rtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track_;
    
    // 当前视频轨道
    rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_;
};
