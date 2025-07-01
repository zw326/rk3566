#pragma once
#include "api/peer_connection_interface.h" // [修改] 包含了 webrtc::PeerConnectionObserver
#include "api/scoped_refptr.h"           // [修改] 引入 webrtc::scoped_refptr
#include "api/frame_transformer_interface.h" // [新增]
#include <memory>
#include <vector>                         // [修改] 为 vector<...> 添加



// 前向声明
class EncodedVideoFrameHandler;
class WebRTCClient;
class AudioReceiver;

// [修正1] 重新定义 VideoFrameTransformer，并确保所有函数签名正确
class VideoFrameTransformer : public webrtc::FrameTransformerInterface {
public:
    explicit VideoFrameTransformer(std::shared_ptr<EncodedVideoFrameHandler> handler);

    void Transform(std::unique_ptr<webrtc::TransformableFrameInterface> frame) override;

    // 确保这个函数签名与基类完全一致
    void RegisterTransformedFrameCallback(
        webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback) override;

    void UnregisterTransformedFrameCallback() override;

private:
    std::shared_ptr<EncodedVideoFrameHandler> handler_;
}; 

/**
 * @brief WebRTC PeerConnection事件的观察者实现类。
 * * 这个类继承了 webrtc::PeerConnectionObserver，负责监听和处理
 * 所有与 PeerConnection 相关的事件，例如媒体流的添加、ICE候选者的生成、连接状态的改变等。
 * 它是WebRTC核心逻辑与您的应用程序代码之间的桥梁。
 */
class PeerConnectionObserverImpl : public webrtc::PeerConnectionObserver {
public:
    /**
     * @brief 构造函数。
     * @param client 一个指向主WebRTCClient实例的指针，用于回调和通信。
     */
    explicit PeerConnectionObserverImpl(WebRTCClient* client);
    
    /**
     * @brief 注入媒体处理器。
     * 在PeerConnection创建后，通过此方法将外部创建的音视频处理器设置进来。
     * @param video_handler 编码视频帧处理器。
     * @param audio_handler 音频接收和处理器。
     */
    void SetMediaHandlers(std::shared_ptr<EncodedVideoFrameHandler> video_handler, std::shared_ptr<AudioReceiver> audio_handler);

    // -------------------------------------------------------------------
    // PeerConnectionObserver 接口的实现部分 (Override)
    // -------------------------------------------------------------------

    // 当信令状态改变时被调用 (e.g., have-local-offer, have-remote-offer)
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override;

    // 当远端添加一个新的媒体轨道时被调用，这是接收音视频流的入口。
    void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                   const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) override;

    // 当远端移除一个媒体轨道时被调用。
    void OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override;

    // 当一个新的数据通道被创建时被调用。
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;

    // 当需要重新协商连接时被调用。
    void OnRenegotiationNeeded() override;

    // 当ICE连接状态改变时被调用 (e.g., checking, connected, failed, disconnected)。
    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override;

    

    // 当ICE收集状态改变时被调用。
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override;

    // 当本地生成一个新的ICE候选者时被调用。
    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;

private:
    /**
     * @brief 内部辅助函数，用于处理视频轨道。
     * @param track 指向视频轨道接口的指针。
     * @param receiver 指向该轨道的RTP接收器，用于注册回调。
     */
    void ProcessVideoTrack(webrtc::VideoTrackInterface* track, webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver);

    /**
     * @brief 内部辅助函数，用于处理音频轨道。
     * @param track 指向音频轨道接口的指针。
     */
    void ProcessAudioTrack(webrtc::AudioTrackInterface* track);

    // 指向主控类WebRTCClient的指针，用于事件通知和回调。
    WebRTCClient* client_;
    
    // 编码视频帧处理器，负责与Rockit VDEC交互。
    std::shared_ptr<EncodedVideoFrameHandler> encoded_video_handler_;
    
    // 音频接收器，负责与Rockit AO交互。
    std::shared_ptr<AudioReceiver> audio_receiver_;
};