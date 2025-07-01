#pragma once

#include "api/peer_connection_interface.h"
#include "rtc_base/ref_counted_object.h" 
#include "rtc_base/thread.h"
#include "api/scoped_refptr.h"
#include "peer_connection_observer_impl.h"
#include "../signaling/signaling_client.h"
#include <memory>
#include <string>
#include <functional>
#include <atomic> // [FIX] 引入 atomic 头文件
#include <json/json.h>

class WebRTCClient {
public:
    WebRTCClient();
    ~WebRTCClient();

    // 初始化所有WebRTC组件
    bool Initialize();
    
    // 清理所有资源
    void Cleanup();

    // 连接到信令服务器
    void ConnectToSignalingServer(const std::string& url, const std::string& room_id, const std::string& client_id = "");
    
    // 由PeerConnectionObserver回调，用于发送信令消息
    void SendIceCandidateToPeer(const webrtc::IceCandidateInterface* candidate);

    // 设置状态变化回调
    using StateChangeCallback = std::function<void(const std::string& state, const std::string& description)>;
    void SetStateChangeCallback(StateChangeCallback callback) { state_change_callback_ = std::move(callback); }
    
    // 设置媒体处理器
    void SetMediaHandlers(std::shared_ptr<EncodedVideoFrameHandler> video_handler, std::shared_ptr<AudioReceiver> audio_handler);

private:
    // 创建PeerConnection
    bool CreatePeerConnection();
    
    // 处理从信令服务器收到的消息
    void HandleSignalingMessage(SignalingClient::MessageType type, const std::string& message);
    void OnOfferReceived(const Json::Value& message_json);
    void OnCandidateReceived(const Json::Value& message_json);

    // 发送SDP Answer
    void SendSdpAnswer(const std::string& sdp);
    
    // 通知状态变化
    void NotifyStateChange(const std::string& state, const std::string& description);

    // WebRTC线程
    std::unique_ptr<webrtc::Thread> network_thread_;
    std::unique_ptr<webrtc::Thread> worker_thread_;
    std::unique_ptr<webrtc::Thread> signaling_thread_;
    
    // WebRTC核心对象
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peer_connection_factory_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
    
    // 观察者与媒体处理器
    std::unique_ptr<PeerConnectionObserverImpl> pc_observer_;
    std::shared_ptr<EncodedVideoFrameHandler> video_handler_;
    std::shared_ptr<AudioReceiver> audio_handler_;
    
    // 信令客户端
    std::unique_ptr<SignalingClient> signaling_client_;
    std::string remote_client_id_; // 用于存储通信对端的ID
    
    // 状态回调
    StateChangeCallback state_change_callback_;
    
    // [FIX] 状态标志使用 atomic
    std::atomic<bool> is_initialized_;
    std::atomic<bool> is_connected_to_signaling_;
};