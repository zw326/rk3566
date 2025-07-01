#pragma once
#include "rk_type.h"
#include "api/video/encoded_image.h"      // 为了使用 EncodedImage 这个“包裹”类
#include "api/video_codecs/video_encoder.h" // 为了使用 EncodedImageCallback 这个“回调”接口
#include "api/video_codecs/video_decoder.h" // 为了使用 VideoDecoder 相关类型
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>

/**
 * @brief 编码视频帧处理器类 - Rockit版本
 * 
 * 该类负责接收WebRTC编码视频帧，通过Rockit硬件解码，
 * 并输出到显示设备，同时支持音视频同步
 */
class EncodedVideoFrameHandler : public webrtc::EncodedImageCallback {
public:
    /**
     * @brief 音视频同步回调函数类型
     * @param video_pts 视频PTS
     * @param system_time 系统时间
     */
    using AudioSyncCallback = std::function<void(int64_t video_pts, int64_t system_time)>;

    /**
     * @brief 视频状态回调函数类型
     * @param state 状态码
     * @param message 状态描述
     */
    using VideoStateCallback = std::function<void(int state, const std::string& message)>;

    /**
     * @brief 构造函数
     */
    EncodedVideoFrameHandler();

    /**
     * @brief 析构函数
     */
    ~EncodedVideoFrameHandler();

    /**
     * @brief 初始化视频处理器
     * @param width 视频宽度
     * @param height 视频高度
     * @param codec_type 编解码器类型
     * @return 是否初始化成功
     */
    bool Initialize(int width = 1920, int height = 1080, const std::string& codec_type = "H264");

    /**
     * @brief 启动视频处理
     * @return 是否启动成功
     */
    bool Start();

    /**
     * @brief 停止视频处理
     */
    void Stop();

    /**
     * @brief 重置视频处理
     */
    void Reset();

    /**
     * @brief 设置音视频同步回调
     * @param callback 回调函数
     */
    void SetAudioSyncCallback(AudioSyncCallback callback) { audio_sync_callback_ = std::move(callback); }

    /**
     * @brief 设置视频状态回调
     * @param callback 回调函数
     */
    void SetVideoStateCallback(VideoStateCallback callback) { video_state_callback_ = std::move(callback); }

    // 实现EncodedImageCallback接口
    webrtc::EncodedImageCallback::Result OnEncodedImage(
        const webrtc::EncodedImage& encoded_image,
        const webrtc::CodecSpecificInfo* codec_specific_info) override;
    void OnDroppedFrame(DropReason reason) override;
private:
    // 用于传递给回调函数的用户数据
    struct UserData {
        void* buffer;
    };

    // C风格的静态回调函数，用于释放内存
    static RK_S32 FreeCallback(void* opaque) {
        if (opaque) {
            UserData* userData = static_cast<UserData*>(opaque);
            free(userData->buffer); // 释放最初malloc的内存
            delete userData;        // 释放结构体本身
            return RK_SUCCESS;
        }
        return RK_FAILURE;
    }
        
    /**
     * @brief 初始化Rockit解码器
     * @return 是否初始化成功
     */
    bool InitializeDecoder();

    /**
     * @brief 初始化Rockit显示输出
     * @return 是否初始化成功
     */
    bool InitializeDisplay();

    /**
     * @brief 解码并显示视频帧
     * @param encoded_data 编码数据
     * @param encoded_size 数据大小
     * @param pts 时间戳
     * @param is_key_frame 是否为关键帧
     * @return 是否处理成功
     */
    bool DecodeAndDisplayFrame(const uint8_t* encoded_data, size_t encoded_size, 
                              int64_t pts, bool is_key_frame);

    /**
     * @brief 通知视频状态变化
     * @param state 状态码
     * @param message 状态描述
     */
    void NotifyVideoState(int state, const std::string& message);

    // 视频参数
    int width_;
    int height_;
    std::string codec_type_;

    // Rockit设备ID
    int vdec_chn_;  // 解码通道
    int vo_chn_;    // 显示通道

    // 状态标志
    std::atomic<bool> is_initialized_;
    std::atomic<bool> is_running_;
    std::atomic<bool> is_decoder_ready_;
    std::atomic<bool> is_display_ready_;

    // 同步相关
    int64_t first_frame_pts_;
    int64_t first_frame_time_;
    bool first_frame_received_;
    std::mutex sync_mutex_;

    // 回调函数
    AudioSyncCallback audio_sync_callback_;
    VideoStateCallback video_state_callback_;
};
