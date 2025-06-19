#include "peer_connection_observer_impl.h"
#include "webrtc_client.h"
#include <iostream>

PeerConnectionObserverImpl::PeerConnectionObserverImpl(WebRTCClient* client)
    : client_(client) {
}

void PeerConnectionObserverImpl::OnSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState new_state) {
    std::cout << "PeerConnection SignalingState changed to: " << new_state << std::endl;
}

void PeerConnectionObserverImpl::OnAddTrack(
    rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) {
    
    auto track = receiver->track();
    if (!track) {
        std::cerr << "OnAddTrack: track is null" << std::endl;
        return;
    }
    
    std::cout << "OnAddTrack: " << track->kind() << " track added" << std::endl;
    
    if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
        // 处理音频轨道
        ProcessAudioTrack(static_cast<webrtc::AudioTrackInterface*>(track.get()));
    } else if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        // 处理视频轨道
        ProcessVideoTrack(static_cast<webrtc::VideoTrackInterface*>(track.get()));
    }
}

void PeerConnectionObserverImpl::OnRemoveTrack(
    rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    std::cout << "Track removed" << std::endl;
    
    auto track = receiver->track();
    if (!track) {
        return;
    }
    
    if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
        // 停止音频处理
        if (audio_receiver_) {
            audio_receiver_->Stop();
        }
        audio_track_ = nullptr;
    } else if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        // 停止视频处理
        if (encoded_video_handler_) {
            encoded_video_handler_->Stop();
        }
        video_track_ = nullptr;
    }
}

void PeerConnectionObserverImpl::OnDataChannel(
    rtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
    std::cout << "Data channel created, label: " << channel->label() << std::endl;
}

void PeerConnectionObserverImpl::OnRenegotiationNeeded() {
    std::cout << "PeerConnection renegotiation needed" << std::endl;
}

void PeerConnectionObserverImpl::OnIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    std::cout << "ICE connection state changed to: " << new_state << std::endl;
    
    // 通知客户端ICE连接状态变化
    if (client_) {
        std::string state_str;
        switch (new_state) {
            case webrtc::PeerConnectionInterface::kIceConnectionNew:
                state_str = "new";
                break;
            case webrtc::PeerConnectionInterface::kIceConnectionChecking:
                state_str = "checking";
                break;
            case webrtc::PeerConnectionInterface::kIceConnectionConnected:
                state_str = "connected";
                break;
            case webrtc::PeerConnectionInterface::kIceConnectionCompleted:
                state_str = "completed";
                break;
            case webrtc::PeerConnectionInterface::kIceConnectionFailed:
                state_str = "failed";
                break;
            case webrtc::PeerConnectionInterface::kIceConnectionDisconnected:
                state_str = "disconnected";
                break;
            case webrtc::PeerConnectionInterface::kIceConnectionClosed:
                state_str = "closed";
                break;
            default:
                state_str = "unknown";
                break;
        }
        
        client_->NotifyStateChange("ice_" + state_str, "ICE connection state: " + state_str);
        
        // 如果连接断开或失败，重置音视频同步
        if (new_state == webrtc::PeerConnectionInterface::kIceConnectionDisconnected ||
            new_state == webrtc::PeerConnectionInterface::kIceConnectionFailed) {
            if (audio_receiver_) {
                audio_receiver_->Reset();
            }
            if (encoded_video_handler_) {
                encoded_video_handler_->Reset();
            }
        }
    }
}

void PeerConnectionObserverImpl::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
    std::cout << "ICE gathering state changed to: " << new_state << std::endl;
}

void PeerConnectionObserverImpl::OnIceCandidate(
    const webrtc::IceCandidateInterface* candidate) {
    // 获取候选信息
    std::string sdp_mid = candidate->sdp_mid();
    int sdp_mline_index = candidate->sdp_mline_index();
    std::string sdp;
    candidate->ToString(&sdp);
    
    // 发送ICE候选到对端
    if (client_) {
        client_->SendIceCandidateToPeer(sdp_mid, sdp_mline_index, sdp);
    }
}

void PeerConnectionObserverImpl::OnIceConnectionReceivingChange(bool receiving) {
    std::cout << "ICE connection receiving changed to: " << (receiving ? "true" : "false") << std::endl;
}

void PeerConnectionObserverImpl::ProcessAudioTrack(webrtc::AudioTrackInterface* track) {
    // 保存音频轨道
    audio_track_ = track;
    
    // 检查音频接收器
    if (!audio_receiver_) {
        std::cerr << "Audio receiver not set" << std::endl;
        return;
    }
    
    // 初始化音频接收器（使用默认参数：48kHz, 立体声, 16位）
    if (!audio_receiver_->Initialize()) {
        std::cerr << "Failed to initialize audio receiver" << std::endl;
        return;
    }
    
    // 添加音频接收器作为音频轨道的接收器
    track->AddSink(audio_receiver_.get());
    
    // 启动音频接收器
    if (!audio_receiver_->Start()) {
        std::cerr << "Failed to start audio receiver" << std::endl;
        track->RemoveSink(audio_receiver_.get());
        return;
    }
    
    std::cout << "Audio track processing started" << std::endl;
}

void PeerConnectionObserverImpl::ProcessVideoTrack(webrtc::VideoTrackInterface* track) {
    // 保存视频轨道
    video_track_ = track;
    
    // 检查编码视频帧处理器
    if (!encoded_video_handler_) {
        std::cerr << "Encoded video frame handler not set" << std::endl;
        return;
    }
    
    // 启动编码视频帧处理器
    if (!encoded_video_handler_->Start()) {
        std::cerr << "Failed to start encoded video frame handler" << std::endl;
        return;
    }
    
    // 设置音视频同步
    if (audio_receiver_ && encoded_video_handler_) {
        // 设置视频帧处理器的音频同步回调
        encoded_video_handler_->SetAudioSyncCallback(
            [this](int64_t video_pts, int64_t system_time) {
                if (audio_receiver_) {
                    audio_receiver_->SetVideoReference(video_pts, system_time);
                }
            }
        );
    }
    
    std::cout << "Video track processing started" << std::endl;
}
