#include "webrtc_client.h"
#include "../signaling/signaling_client_ws.h"
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/sctp_transport_interface.h"
#include <iostream>

// 创建SetSessionDescriptionObserver类
class SetSessionDescriptionObserver : public webrtc::SetSessionDescriptionObserver {
public:
    static rtc::scoped_refptr<SetSessionDescriptionObserver> Create(
            std::function<void()> on_success = nullptr,
            std::function<void(const std::string&)> on_failure = nullptr) {
        return rtc::make_ref_counted<SetSessionDescriptionObserver>(
                std::move(on_success), std::move(on_failure));
    }

    void OnSuccess() override {
        if (on_success_) {
            on_success_();
        }
    }

    void OnFailure(webrtc::RTCError error) override {
        if (on_failure_) {
            on_failure_(error.message());
        }
    }

protected:
    SetSessionDescriptionObserver(
            std::function<void()> on_success,
            std::function<void(const std::string&)> on_failure)
        : on_success_(std::move(on_success))
        , on_failure_(std::move(on_failure)) {}

    ~SetSessionDescriptionObserver() override = default;

private:
    std::function<void()> on_success_;
    std::function<void(const std::string&)> on_failure_;
};

// 创建CreateSessionDescriptionObserver类
class CreateSessionDescriptionObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    static rtc::scoped_refptr<CreateSessionDescriptionObserver> Create(
            WebRTCClient* client) {
        return rtc::make_ref_counted<CreateSessionDescriptionObserver>(client);
    }

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        if (!client_) return;

        std::string sdp;
        desc->ToString(&sdp);

        // 设置本地描述
        client_->peer_connection_->SetLocalDescription(
            SetSessionDescriptionObserver::Create(),
            desc);

        // 发送SDP Answer给对方
        client_->SendSdpAnswer(sdp);
    }

    void OnFailure(webrtc::RTCError error) override {
        std::cerr << "Failed to create session description: " 
                  << error.message() << std::endl;
    }

protected:
    explicit CreateSessionDescriptionObserver(WebRTCClient* client)
        : client_(client) {}

    ~CreateSessionDescriptionObserver() override = default;

private:
    WebRTCClient* client_;
};

WebRTCClient::WebRTCClient()
    : is_initialized_(false)
    , is_connected_to_signaling_(false) {
}

WebRTCClient::~WebRTCClient() {
    Cleanup();
}

bool WebRTCClient::Initialize() {
    if (is_initialized_) {
        std::cout << "WebRTCClient already initialized" << std::endl;
        return true;
    }

    std::cout << "Initializing WebRTCClient..." << std::endl;

    // 创建WebRTC线程
    network_thread_ = rtc::Thread::CreateWithSocketServer();
    if (!network_thread_->Start()) {
        std::cerr << "Failed to start network thread" << std::endl;
        return false;
    }

    worker_thread_ = rtc::Thread::Create();
    if (!worker_thread_->Start()) {
        std::cerr << "Failed to start worker thread" << std::endl;
        return false;
    }

    signaling_thread_ = rtc::Thread::Create();
    if (!signaling_thread_->Start()) {
        std::cerr << "Failed to start signaling thread" << std::endl;
        return false;
    }

    // 创建PeerConnectionFactory
    peer_connection_factory_ = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(),
        worker_thread_.get(),
        signaling_thread_.get(),
        nullptr,  // 默认ADM
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(),
        nullptr,  // 音频混合器
        nullptr   // 音频处理
    );

    if (!peer_connection_factory_) {
        std::cerr << "Failed to create PeerConnectionFactory" << std::endl;
        return false;
    }

    // 创建PeerConnection
    if (!CreatePeerConnection()) {
        std::cerr << "Failed to create PeerConnection" << std::endl;
        return false;
    }

    // 创建信令客户端
    signaling_client_ = std::make_unique<WebSocketSignalingClient>();
    
    // 设置信令客户端回调
    signaling_client_->SetOnConnected([this]() {
        this->OnSignalingConnected();
    });
    
    signaling_client_->SetOnDisconnected([this](int code, const std::string& reason) {
        this->OnSignalingDisconnected(code, reason);
    });
    
    signaling_client_->SetOnMessage([this](const std::string& message) {
        this->HandleSignalingMessage(message);
    });
    
    signaling_client_->SetOnError([this](const std::string& error) {
        this->OnSignalingError(error);
    });

    is_initialized_ = true;
    std::cout << "WebRTCClient initialized successfully" << std::endl;
    return true;
}

bool WebRTCClient::CreatePeerConnection() {
    if (!peer_connection_factory_) {
        std::cerr << "PeerConnectionFactory not created" << std::endl;
        return false;
    }

    // 创建PeerConnection配置
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    
    // 添加STUN/TURN服务器
    webrtc::PeerConnectionInterface::IceServer ice_server;
    ice_server.uri = "stun:stun.l.google.com:19302";
    config.servers.push_back(ice_server);

    // 创建PeerConnectionObserver
    pc_observer_ = std::make_unique<PeerConnectionObserverImpl>(this);

    // 创建PeerConnection
    peer_connection_ = peer_connection_factory_->CreatePeerConnection(
        config, nullptr, nullptr, pc_observer_.get());

    if (!peer_connection_) {
        std::cerr << "Failed to create PeerConnection" << std::endl;
        return false;
    }

    std::cout << "PeerConnection created successfully" << std::endl;
    return true;
}

void WebRTCClient::ConnectToSignalingServer(const std::string& url) {
    if (!is_initialized_) {
        std::cerr << "WebRTCClient not initialized" << std::endl;
        return;
    }

    if (!signaling_client_) {
        std::cerr << "SignalingClient not created" << std::endl;
        return;
    }

    std::cout << "Connecting to signaling server: " << url << std::endl;
    signaling_client_->Connect(url);
}

void WebRTCClient::HandleSignalingMessage(const std::string& message) {
    std::cout << "Received signaling message: " << message << std::endl;

    // 解析JSON消息
    Json::Value root;
    if (!json_reader_.parse(message, root)) {
        std::cerr << "Failed to parse signaling message as JSON" << std::endl;
        return;
    }

    // 检查消息类型
    if (!root.isMember("type")) {
        std::cerr << "Signaling message missing 'type' field" << std::endl;
        return;
    }

    std::string type = root["type"].asString();

    if (type == "offer") {
        // 处理SDP Offer
        if (!root.isMember("sdp")) {
            std::cerr << "Offer message missing 'sdp' field" << std::endl;
            return;
        }
        OnOfferReceived(root["sdp"].asString());
    } 
    else if (type == "candidate") {
        // 处理ICE Candidate
        if (!root.isMember("candidate") || !root.isMember("sdpMid") || !root.isMember("sdpMLineIndex")) {
            std::cerr << "Candidate message missing required fields" << std::endl;
            return;
        }
        OnRemoteIceCandidateReceived(
            root["sdpMid"].asString(),
            root["sdpMLineIndex"].asInt(),
            root["candidate"].asString()
        );
    }
    else {
        std::cout << "Ignoring unknown signaling message type: " << type << std::endl;
    }
}

void WebRTCClient::OnOfferReceived(const std::string& sdp_offer) {
    if (!peer_connection_) {
        std::cerr << "PeerConnection not created" << std::endl;
        return;
    }

    std::cout << "Processing received offer SDP" << std::endl;

    // 创建远程SDP对象
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp_offer, &error);

    if (!session_description) {
        std::cerr << "Failed to parse offer SDP: " << error.description << std::endl;
        return;
    }

    // 设置远程描述
    peer_connection_->SetRemoteDescription(
        SetSessionDescriptionObserver::Create(
            [this]() {
                std::cout << "SetRemoteDescription success, creating answer..." << std::endl;
                
                // 创建Answer
                webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
                peer_connection_->CreateAnswer(
                    CreateSessionDescriptionObserver::Create(this),
                    options);
            },
            [](const std::string& error) {
                std::cerr << "SetRemoteDescription failed: " << error << std::endl;
            }
        ),
        session_description.release()
    );
}

void WebRTCClient::OnRemoteIceCandidateReceived(
    const std::string& sdp_mid, 
    int sdp_mline_index, 
    const std::string& candidate_sdp) {
    
    if (!peer_connection_) {
        std::cerr << "PeerConnection not created" << std::endl;
        return;
    }

    // 创建ICE候选对象
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidateInterface> candidate(
        webrtc::CreateIceCandidate(sdp_mid, sdp_mline_index, candidate_sdp, &error));

    if (!candidate) {
        std::cerr << "Failed to parse ICE candidate: " << error.description << std::endl;
        return;
    }

    // 添加ICE候选
    if (!peer_connection_->AddIceCandidate(candidate.get())) {
        std::cerr << "Failed to add ICE candidate" << std::endl;
        return;
    }

    std::cout << "Added remote ICE candidate: " << candidate_sdp << std::endl;
}

void WebRTCClient::SendSdpAnswer(const std::string& sdp_answer) {
    if (!signaling_client_ || !signaling_client_->IsConnected()) {
        std::cerr << "SignalingClient not connected" << std::endl;
        return;
    }

    // 创建Answer消息
    signaling_client_->SendMessage(SignalingClient::MessageType::ANSWER, sdp_answer);
    std::cout << "Sent SDP answer to peer" << std::endl;
}

void WebRTCClient::SendIceCandidateToPeer(
    const std::string& sdp_mid, 
    int sdp_mline_index, 
    const std::string& candidate_sdp) {
    
    if (!signaling_client_ || !signaling_client_->IsConnected()) {
        std::cerr << "SignalingClient not connected" << std::endl;
        return;
    }

    // 创建Candidate消息
    Json::Value payload;
    payload["sdpMid"] = sdp_mid;
    payload["sdpMLineIndex"] = sdp_mline_index;
    payload["candidate"] = candidate_sdp;
    
    signaling_client_->SendMessage(SignalingClient::MessageType::ICE_CANDIDATE, 
                                  json_writer_.write(payload));
    std::cout << "Sent ICE candidate to peer" << std::endl;
}

void WebRTCClient::OnSignalingConnected() {
    std::cout << "Connected to signaling server" << std::endl;
    is_connected_to_signaling_ = true;
    
    // 注册到信令服务器
    signaling_client_->Register();
    
    NotifyStateChange("signaling_connected", "Connected to signaling server");
}

void WebRTCClient::OnSignalingDisconnected(int code, const std::string& reason) {
    std::cout << "Disconnected from signaling server: " << code << " - " << reason << std::endl;
    is_connected_to_signaling_ = false;
    NotifyStateChange("signaling_disconnected", "Disconnected from signaling server: " + reason);
}

void WebRTCClient::OnSignalingError(const std::string& error) {
    std::cerr << "Signaling error: " << error << std::endl;
    NotifyStateChange("signaling_error", "Signaling error: " + error);
}

void WebRTCClient::NotifyStateChange(const std::string& state, const std::string& description) {
    if (state_change_callback_) {
        state_change_callback_(state, description);
    }
}

void WebRTCClient::Cleanup() {
    std::cout << "Cleaning up WebRTCClient..." << std::endl;

    // 关闭信令连接
    if (signaling_client_) {
        signaling_client_->Close();
        signaling_client_.reset();
    }

    // 关闭PeerConnection
    if (peer_connection_) {
        peer_connection_->Close();
        peer_connection_ = nullptr;
    }

    // 释放PeerConnectionFactory
    peer_connection_factory_ = nullptr;

    // 停止并释放线程
    if (network_thread_) {
        network_thread_->Stop();
        network_thread_.reset();
    }

    if (worker_thread_) {
        worker_thread_->Stop();
        worker_thread_.reset();
    }

    if (signaling_thread_) {
        signaling_thread_->Stop();
        signaling_thread_.reset();
    }

    pc_observer_.reset();
    is_initialized_ = false;
    is_connected_to_signaling_ = false;

    std::cout << "WebRTCClient cleanup completed" << std::endl;
}
