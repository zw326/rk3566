#include "peer_connection_observer_impl.h"
#include "webrtc_client.h"
#include "encoded_video_frame_handler_rockit.h"
#include "audio_receiver_rockit.h"
#include "api/video/encoded_image.h"         // [新增] 确保包含了 EncodedImage 的完整定义
#include "rtc_base/ref_counted_object.h"   // [新增] 确保包含了 make_ref_counted
#include <iostream>



// --- VideoFrameTransformer 方法实现 ---
VideoFrameTransformer::VideoFrameTransformer(std::shared_ptr<EncodedVideoFrameHandler> handler)
    : handler_(std::move(handler)) {}

void VideoFrameTransformer::RegisterTransformedFrameCallback(
    webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback) {
    // 本项目不修改帧，也无需将帧传回WebRTC流程，所以留空
}

void VideoFrameTransformer::UnregisterTransformedFrameCallback() {
    // 留空
}


void VideoFrameTransformer::Transform(std::unique_ptr<webrtc::TransformableFrameInterface> frame) {
    if (!handler_ || frame->GetDirection() != webrtc::TransformableFrameInterface::Direction::kReceiver) {
        return;
    }

    auto* video_frame = static_cast<webrtc::TransformableVideoFrameInterface*>(frame.get());
    
    webrtc::EncodedImage encoded_image;
    auto data_view = video_frame->GetData();
    encoded_image.SetEncodedData(webrtc::EncodedImageBuffer::Create(data_view.data(), data_view.size()));
    encoded_image.set_size(data_view.size());
    
    // [修正3] 使用正确的API和类型转换
    encoded_image.SetRtpTimestamp(video_frame->GetTimestamp());
    
    // [修正4] 使用新的GetPresentationTimestamp()，并正确处理optional和类型转换
    auto presentation_timestamp = video_frame->GetPresentationTimestamp();
    if (presentation_timestamp.has_value()) {
        encoded_image.capture_time_ms_ = presentation_timestamp->ms();
    } else {
        encoded_image.capture_time_ms_ = -1;
    }

    encoded_image._frameType = video_frame->IsKeyFrame() ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
    
    handler_->OnEncodedImage(encoded_image, nullptr);
}

// 构造函数，初始化客户端指针。
PeerConnectionObserverImpl::PeerConnectionObserverImpl(WebRTCClient* client) : client_(client) {}

// 设置媒体处理器，将外部创建的handler注入到观察者内部。
void PeerConnectionObserverImpl::SetMediaHandlers(
    std::shared_ptr<EncodedVideoFrameHandler> video_handler,
    std::shared_ptr<AudioReceiver> audio_handler) {
    encoded_video_handler_ = std::move(video_handler);
    audio_receiver_ = std::move(audio_handler);
}

// 当信令状态改变时被调用。
void PeerConnectionObserverImpl::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) {
    // 打印日志，方便调试。
    std::cout << "PeerConnection SignalingState changed to: " << webrtc::PeerConnectionInterface::AsString(new_state) << std::endl;
}

// 当接收到新的媒体轨道时被调用，这是媒体处理的起点。
void PeerConnectionObserverImpl::OnAddTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) {
    
    auto track = receiver->track();
    if (!track) return;
    
    std::cout << "OnAddTrack: " << track->kind() << " track added with id: " << track->id() << std::endl;
    
    // 根据轨道的类型（音频或视频），分发给不同的处理函数。
    if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        // 将RtpReceiver一起传递下去，用于注册回调。
        ProcessVideoTrack(static_cast<webrtc::VideoTrackInterface*>(track.get()), receiver);
    } else if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
        ProcessAudioTrack(static_cast<webrtc::AudioTrackInterface*>(track.get()));
    }
}

// 当媒体轨道被移除时调用。
void PeerConnectionObserverImpl::OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    std::cout << "Track removed" << std::endl;
    // 在这里可以添加停止相应媒体处理器的逻辑。
}

// 当数据通道被创建时调用。
void PeerConnectionObserverImpl::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
    std::cout << "Data channel created, label: " << channel->label() << std::endl;
}

// 当需要重新协商SDP时调用。
void PeerConnectionObserverImpl::OnRenegotiationNeeded() {
    std::cout << "PeerConnection renegotiation needed" << std::endl;
}

// 当ICE（网络穿透）连接状态改变时调用，非常重要。
void PeerConnectionObserverImpl::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    std::cout << "ICE connection state changed to: " << webrtc::PeerConnectionInterface::AsString(new_state) << std::endl;
    if (client_) {
        // 当连接断开或失败时，重置音视频处理器。
        // 这是一个非常重要的健壮性设计，可以清空缓冲区，重置同步状态，为下一次连接做准备。
        if (new_state == webrtc::PeerConnectionInterface::kIceConnectionDisconnected ||
            new_state == webrtc::PeerConnectionInterface::kIceConnectionFailed) {
            if (audio_receiver_) audio_receiver_->Reset();
            if (encoded_video_handler_) encoded_video_handler_->Reset();
        }
    }
}

// 当ICE收集状态改变时调用。
void PeerConnectionObserverImpl::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) {
    std::cout << "ICE gathering state changed to: " << webrtc::PeerConnectionInterface::AsString(new_state) << std::endl;
}

// 当WebRTC引擎在本地发现一个可用的网络候选者（IP地址和端口）时调用。
void PeerConnectionObserverImpl::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
    // 将这个候选者信息通过主控类发送给信令服务器，由信令服务器转发给对端。
    if (client_) {
        client_->SendIceCandidateToPeer(candidate);
    }
}



// 视频轨道的具体处理逻辑。
void PeerConnectionObserverImpl::ProcessVideoTrack(webrtc::VideoTrackInterface* track, webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    if (!encoded_video_handler_) {
        std::cerr << "Encoded video frame handler not set!" << std::endl;
        return;
    }

    auto transformer = webrtc::make_ref_counted<VideoFrameTransformer>(encoded_video_handler_);
    receiver->SetFrameTransformer(transformer);

    if (audio_receiver_) {
        encoded_video_handler_->SetAudioSyncCallback(
            [this](int64_t video_pts, int64_t system_time) {
                if (audio_receiver_) {
                    audio_receiver_->SetVideoReference(video_pts, system_time);
                }
            }
        );
    }
    std::cout << "Video track processing started and FrameTransformer registered." << std::endl;


    // // 【核心修复】在这里将我们的视频处理器注册为编码帧的观察者。
    // // 只有注册之后，OnEncodedImage回调才会被WebRTC调用，我们才能收到H.264/H.265数据。
    // receiver->RegisterEncodedFrameObserver(encoded_video_handler_.get());
    
    // // 配置音视频同步回调：让视频处理器在收到关键帧时，通知音频处理器当前的视频时间戳。
    // if (audio_receiver_) {
    //     encoded_video_handler_->SetAudioSyncCallback(
    //         [this](int64_t video_pts, int64_t system_time) {
    //             if (audio_receiver_) {
    //                 audio_receiver_->SetVideoReference(video_pts, system_time);
    //             }
    //         }
    //     );
    // }
    
    // std::cout << "Video track processing started and observer registered." << std::endl;
}

// 音频轨道的具体处理逻辑。
void PeerConnectionObserverImpl::ProcessAudioTrack(webrtc::AudioTrackInterface* track) {
    if (!audio_receiver_) {
        std::cerr << "Audio receiver not set!" << std::endl;
        return;
    }
    
    // 【设计选择】调用AddSink，订阅解码后的PCM音频数据。
    // 这是一种简单有效的策略，由WebRTC负责音频解码。
    // 我们的AudioReceiver类将接收PCM数据并送入Rockit的AO模块播放。
    track->AddSink(audio_receiver_.get());
    std::cout << "Audio track sink added." << std::endl;
}

