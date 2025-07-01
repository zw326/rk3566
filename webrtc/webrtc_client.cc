#include "webrtc_client.h"
#include "../signaling/signaling_client_ws.h"
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "rtc_base/ref_counted_object.h"
#include <iostream>
#include <sstream>

// 辅助的 Observer 类 (和之前保持一致，是正确的)
class SetSessionDescriptionObserver : public webrtc::SetSessionDescriptionObserver {
public:
    static webrtc::scoped_refptr<SetSessionDescriptionObserver> Create(
            std::function<void()> on_success,
            std::function<void(webrtc::RTCError)> on_failure) {
        return webrtc::make_ref_counted<SetSessionDescriptionObserver>(std::move(on_success), std::move(on_failure));
    }
    void OnSuccess() override { if (on_success_) on_success_(); }
    void OnFailure(webrtc::RTCError error) override { if (on_failure_) on_failure_(std::move(error)); }
protected:
    SetSessionDescriptionObserver(std::function<void()> on_success, std::function<void(webrtc::RTCError)> on_failure)
        : on_success_(std::move(on_success)), on_failure_(std::move(on_failure)) {}
private:
    std::function<void()> on_success_;
    std::function<void(webrtc::RTCError)> on_failure_;
};

class CreateSessionDescriptionObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    using SuccessCallback = std::function<void(webrtc::SessionDescriptionInterface*)>;
    static webrtc::scoped_refptr<CreateSessionDescriptionObserver> Create(SuccessCallback on_success, std::function<void(webrtc::RTCError)> on_failure) {
        return webrtc::make_ref_counted<CreateSessionDescriptionObserver>(std::move(on_success), std::move(on_failure));
    }
    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override { if (on_success_) on_success_(desc); }
    void OnFailure(webrtc::RTCError error) override { if (on_failure_) on_failure_(std::move(error)); }
protected:
    CreateSessionDescriptionObserver(SuccessCallback on_success, std::function<void(webrtc::RTCError)> on_failure)
        : on_success_(std::move(on_success)), on_failure_(std::move(on_failure)) {}
private:
    SuccessCallback on_success_;
    std::function<void(webrtc::RTCError)> on_failure_;
};


WebRTCClient::WebRTCClient() : is_initialized_(false), is_connected_to_signaling_(false) {}

WebRTCClient::~WebRTCClient() {
    Cleanup();
}

void WebRTCClient::SetMediaHandlers(std::shared_ptr<EncodedVideoFrameHandler> video_handler, std::shared_ptr<AudioReceiver> audio_handler) {
    video_handler_ = std::move(video_handler);
    audio_handler_ = std::move(audio_handler);
}

bool WebRTCClient::Initialize() {
    if (is_initialized_) return true;

    network_thread_ = webrtc::Thread::CreateWithSocketServer();
    worker_thread_ = webrtc::Thread::Create();
    signaling_thread_ = webrtc::Thread::Create();
    if (!network_thread_->Start() || !worker_thread_->Start() || !signaling_thread_->Start()) {
        std::cerr << "Failed to start threads" << std::endl;
        return false;
    }

    peer_connection_factory_ = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(), worker_thread_.get(), signaling_thread_.get(),
        nullptr,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(),
        nullptr, nullptr);

    if (!peer_connection_factory_) {
        std::cerr << "Failed to create PeerConnectionFactory" << std::endl;
        return false;
    }

    if (!CreatePeerConnection()) {
        return false;
    }

    signaling_client_ = std::make_unique<WebSocketSignalingClient>();
    
    // [FIX] 使用正确的信令回调接口
    signaling_client_->SetStateCallback([this](bool connected, const std::string& message) {
        if (connected) {
            is_connected_to_signaling_ = true;
            NotifyStateChange("signaling_connected", message);
        } else {
            is_connected_to_signaling_ = false;
            NotifyStateChange("signaling_disconnected", message);
        }
    });
    
    signaling_client_->SetMessageCallback([this](SignalingClient::MessageType type, const std::string& message) {
        this->HandleSignalingMessage(type, message);
    });

    is_initialized_ = true;
    std::cout << "WebRTCClient initialized successfully" << std::endl;
    return true;
}

bool WebRTCClient::CreatePeerConnection() {
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    webrtc::PeerConnectionInterface::IceServer ice_server;
    ice_server.uri = "stun:stun.l.google.com:19302";
    config.servers.push_back(ice_server);

    pc_observer_ = std::make_unique<PeerConnectionObserverImpl>(this);
    if(video_handler_ && audio_handler_){
        pc_observer_->SetMediaHandlers(video_handler_, audio_handler_);
    }

    // peer_connection_ = peer_connection_factory_->CreatePeerConnectionOrError(config, nullptr, nullptr, pc_observer_.get());

    // if (!peer_connection_) {
    //     std::cerr << "Failed to create PeerConnection" << std::endl;
    //     return false;
    // }
    // return true;

    // [修改] 调用 CreatePeerConnectionOrError
    webrtc::PeerConnectionDependencies pc_deps(pc_observer_.get());
    auto result = peer_connection_factory_->CreatePeerConnectionOrError(config, std::move(pc_deps));
    
    if (!result.ok()) {
        std::cerr << "Failed to create PeerConnection: " << result.error().message() << std::endl;
        peer_connection_ = nullptr;
        return false;
    }
    peer_connection_ = result.MoveValue();
    return peer_connection_ != nullptr;
}

void WebRTCClient::ConnectToSignalingServer(const std::string& url, const std::string& room_id, const std::string& client_id) {
    if (!is_initialized_) return;
    signaling_client_->Connect(url);
    // 等待连接成功后自动注册
    signaling_client_->Register(room_id, client_id);
}

// [FIX] 将消息处理逻辑与信令回调对接
void WebRTCClient::HandleSignalingMessage(SignalingClient::MessageType type, const std::string& message) {
    // [FIX] 使用现代的JSON库API
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;

    if (!reader->parse(message.c_str(), message.c_str() + message.length(), &root, &errors)) {
        std::cerr << "Failed to parse signaling message: " << errors << std::endl;
        return;
    }

    switch (type) {
        case SignalingClient::MessageType::OFFER:
            OnOfferReceived(root);
            break;
        case SignalingClient::MessageType::CANDIDATE:
            OnCandidateReceived(root);
            break;
        default:
            std::cout << "Ignoring message of type: " << static_cast<int>(type) << std::endl;
            break;
    }
}
#if 0
void WebRTCClient::OnOfferReceived(const Json::Value& message_json) {
    if (!message_json.isMember("sdp") || !message_json.isMember("from")) {
        std::cerr << "Offer message missing required fields" << std::endl;
        return;
    }
    
    std::string sdp = message_json["sdp"].asString();
    remote_client_id_ = message_json["from"].asString(); // 存储对端ID

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error);

    if (!session_description) {
        std::cerr << "Failed to parse offer SDP: " << error.description << std::endl;
        return;
    }

    peer_connection_->SetRemoteDescription(
        SetSessionDescriptionObserver::Create(
            [this]() {
                std::cout << "SetRemoteDescription success, creating answer..." << std::endl;
                webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
                peer_connection_->CreateAnswer(
                    CreateSessionDescriptionObserver::Create(
                        [this](webrtc::SessionDescriptionInterface* desc) {
                            peer_connection_->SetLocalDescription(
                                SetSessionDescriptionObserver::Create(
                                    [this, desc]() {
                                        std::string sdp;
                                        desc->ToString(&sdp);
                                        this->SendSdpAnswer(sdp);
                                    },
                                    [](webrtc::RTCError error){ std::cerr << "SetLocalDescription failed: " << error.message() << std::endl; }
                                ),
                                desc
                            );
                        },
                        [](webrtc::RTCError error){ std::cerr << "CreateAnswer failed: " << error.message() << std::endl; }
                    ),
                    options);
            },
            [](webrtc::RTCError error) {
                std::cerr << "SetRemoteDescription failed: " << error.message() << std::endl;
            }
        ),
        session_description.release()
    );
}
#endif

void WebRTCClient::OnOfferReceived(const Json::Value& message_json) {
    if (!message_json.isMember("sdp") || !message_json.isMember("from")) {
        std::cerr << "Offer message missing required fields" << std::endl;
        return;
    }

    std::string sdp = message_json["sdp"].asString();
    remote_client_id_ = message_json["from"].asString();

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error);

    if (!session_description) {
        std::cerr << "Failed to parse offer SDP: " << error.description << std::endl;
        return;
    }

        // [修改] 调用接收裸指针的旧版API
    peer_connection_->SetRemoteDescription(
        // 使用 .get() 传递裸指针
        SetSessionDescriptionObserver::Create(
            [this]() {
                std::cout << "SetRemoteDescription success, creating answer..." << std::endl;
                webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
                peer_connection_->CreateAnswer(
                    // 使用 .get() 传递裸指针
                    CreateSessionDescriptionObserver::Create(
                        [this](webrtc::SessionDescriptionInterface* desc) {
                            peer_connection_->SetLocalDescription(
                                // 使用 .get() 传递裸指针
                                SetSessionDescriptionObserver::Create(
                                    [this, desc]() {
                                        std::string sdp;
                                        desc->ToString(&sdp);
                                        this->SendSdpAnswer(sdp);
                                    },
                                    [](webrtc::RTCError error){ std::cerr << "SetLocalDescription failed: " << error.message() << std::endl; }
                                ).get(),
                                desc
                            );
                        },
                        [](webrtc::RTCError error){ std::cerr << "CreateAnswer failed: " << error.message() << std::endl; }
                    ).get(),
                    options);
            },
            [](webrtc::RTCError error) {
                std::cerr << "SetRemoteDescription failed: " << error.message() << std::endl;
            }
        ).get(),
        session_description.release() // release() 会交出所有权并返回裸指针
    );
}

void WebRTCClient::OnCandidateReceived(const Json::Value& message_json) {
    if (!message_json.isMember("candidate") || !message_json.isMember("sdpMid") || !message_json.isMember("sdpMLineIndex")) {
        std::cerr << "Candidate message missing required fields" << std::endl;
        return;
    }
    
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidateInterface> candidate(webrtc::CreateIceCandidate(
        message_json["sdpMid"].asString(),
        message_json["sdpMLineIndex"].asInt(),
        message_json["candidate"].asString(),
        &error
    ));

    if (!candidate) {
        std::cerr << "Failed to parse ICE candidate: " << error.description << std::endl;
        return;
    }

    if (!peer_connection_->AddIceCandidate(candidate.get())) {
        std::cerr << "Failed to add ICE candidate" << std::endl;
    }
}

// [FIX] 使用正确的信令接口发送Answer
void WebRTCClient::SendSdpAnswer(const std::string& sdp) {
    if (!signaling_client_ || !is_connected_to_signaling_) {
        std::cerr << "Cannot send answer: SignalingClient not connected" << std::endl;
        return;
    }
    signaling_client_->SendAnswer(sdp, remote_client_id_);
}

// [FIX] 使用正确的信令接口发送Candidate
void WebRTCClient::SendIceCandidateToPeer(const webrtc::IceCandidateInterface* candidate) {
    if (!signaling_client_ || !is_connected_to_signaling_) {
        std::cerr << "Cannot send candidate: SignalingClient not connected" << std::endl;
        return;
    }
    std::string sdp;
    candidate->ToString(&sdp);
    signaling_client_->SendCandidate(candidate->sdp_mid(), candidate->sdp_mline_index(), sdp, remote_client_id_);
}

void WebRTCClient::NotifyStateChange(const std::string& state, const std::string& description) {
    if (state_change_callback_) {
        state_change_callback_(state, description);
    }
}

void WebRTCClient::Cleanup() {
    is_initialized_ = false;
    is_connected_to_signaling_ = false;
    if (peer_connection_) {
        peer_connection_->Close();
        peer_connection_ = nullptr;
    }
    if (signaling_client_) {
        signaling_client_->Close();
        signaling_client_.reset();
    }
    peer_connection_factory_ = nullptr;
    network_thread_->Stop();
    worker_thread_->Stop();
    signaling_thread_->Stop();
    pc_observer_.reset();
    video_handler_.reset();
    audio_handler_.reset();
}