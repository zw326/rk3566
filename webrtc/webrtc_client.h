#pragma once
#include "api/peer_connection_interface.h"
#include "rtc_base/thread.h"
#include "peer_connection_observer_impl.h"
#include "../signaling/signaling_client.h"
#include <memory>
#include <string>
#include <functional>
#include <json/json.h>

// WebRTC核心逻辑的封装类
class WebRTCClient {
public:
    WebRTCClient();
    ~WebRTCClient();

    // 初始化所有WebRTC组件
    bool Initialize();
    
    // 清理所有资源
    void Cleanup();

    // 连接到信令服务器
    void ConnectToSignalingServer(const std::string& url);
    
    // 处理从信令服务器收到的消息
    void HandleSignalingMessage(const std::string& message);

    // 由PeerConnectionObserver回调，用于发送信令消息
    void SendSdpAnswer(const std::string& sdp_answer);
    void SendIceCandidateToPeer(const std::string& sdp_mid, int sdp_mline_index, const std::string& candidate_sdp);

    // 设置状态变化回调
    using StateChangeCallback = std::function<void(const std::string& state, const std::string& description)>;
    void SetStateChangeCallback(StateChangeCallback callback) { state_change_callback_ = std::move(callback); }

private:
    // 创建PeerConnection
    bool CreatePeerConnection();
    
    // 处理SDP Offer
    void OnOfferReceived(const std::string& sdp_offer);
    
    // 处理远端ICE Candidate
    void OnRemoteIceCandidateReceived(const std::string& sdp_mid, int sdp_mline_index, const std::string& candidate_sdp);
    
    // 处理信令连接状态变化
    void OnSignalingConnected();
    void OnSignalingDisconnected(int code, const std::string& reason);
    void OnSignalingError(const std::string& error);
    
    // 通知状态变化
    void NotifyStateChange(const std::string& state, const std::string& description);

    // WebRTC线程
    std::unique_ptr<rtc::Thread> network_thread_;
    std::unique_ptr<rtc::Thread> worker_thread_;
    std::unique_ptr<rtc::Thread> signaling_thread_;
    
    // WebRTC核心对象
    rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peer_connection_factory_;
    rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
    
    // 观察者
    std::unique_ptr<PeerConnectionObserverImpl> pc_observer_;
    
    // 信令客户端
    std::unique_ptr<SignalingClient> signaling_client_;
    
    // 状态回调
    StateChangeCallback state_change_callback_;
    
    // JSON解析器
    Json::Reader json_reader_;
    Json::FastWriter json_writer_;
    
    // 状态标志
    bool is_initialized_;
    bool is_connected_to_signaling_;
};
