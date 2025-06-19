#pragma once

#include "api/media_stream_interface.h"
#include "api/audio/audio_frame.h"
#include "rtc_base/thread.h"
#include <mutex>
#include <queue>
#include <memory>
#include <atomic>
#include <functional>

// 前向声明Rockit相关结构体，避免直接包含Rockit头文件
struct RK_AUDIO_FRAME_INFO_S;

/**
 * @brief 音频接收器类 - Rockit版本
 * 
 * 该类实现了AudioTrackSinkInterface接口，可以直接从WebRTC音频轨道接收PCM数据
 * 同时管理音频缓冲、时间戳同步和Rockit音频设备输出
 */
class AudioReceiver : public webrtc::AudioTrackSinkInterface {
public:
    /**
     * @brief 音频状态回调函数类型
     * @param state 状态码
     * @param message 状态描述
     */
    using AudioStateCallback = std::function<void(int state, const std::string& message)>;

    /**
     * @brief 构造函数
     */
    AudioReceiver();

    /**
     * @brief 析构函数
     */
    ~AudioReceiver() override;

    /**
     * @brief 初始化音频接收器
     * @param sample_rate 采样率，默认48000Hz
     * @param channels 声道数，默认2（立体声）
     * @param bits_per_sample 位宽，默认16位
     * @return 是否初始化成功
     */
    bool Initialize(int sample_rate = 48000, int channels = 2, int bits_per_sample = 16);

    /**
     * @brief 启动音频处理
     * @return 是否启动成功
     */
    bool Start();

    /**
     * @brief 停止音频处理
     */
    void Stop();

    /**
     * @brief 重置音频处理
     */
    void Reset();

    /**
     * @brief 设置音频状态回调
     * @param callback 回调函数
     */
    void SetAudioStateCallback(AudioStateCallback callback) { audio_state_callback_ = std::move(callback); }

    /**
     * @brief 设置音频延迟目标（用于音视频同步）
     * @param delay_ms 目标延迟（毫秒）
     */
    void SetTargetDelayMs(int delay_ms);

    /**
     * @brief 获取当前音频延迟
     * @return 当前延迟（毫秒）
     */
    int GetCurrentDelayMs() const;

    /**
     * @brief 设置视频参考时间戳（用于音视频同步）
     * @param video_pts 视频PTS
     * @param system_time 系统时间
     */
    void SetVideoReference(int64_t video_pts, int64_t system_time);

    /**
     * @brief 获取音频设备状态
     * @return 是否正常工作
     */
    bool IsDeviceWorking() const { return is_device_working_; }

    /**
     * @brief 获取音频缓冲区大小
     * @return 缓冲区中的帧数
     */
    size_t GetBufferSize() const;

    // 实现AudioTrackSinkInterface接口
    void OnData(const void* audio_data,
                int bits_per_sample,
                int sample_rate,
                size_t number_of_channels,
                size_t number_of_frames,
                absl::optional<int64_t> absolute_capture_timestamp_ms) override;

private:
    /**
     * @brief 音频帧结构体
     */
    struct AudioFrame {
        std::unique_ptr<uint8_t[]> data;  // 音频数据
        size_t size;                      // 数据大小（字节）
        int64_t pts;                      // 时间戳
        int sample_rate;                  // 采样率
        int channels;                     // 声道数
        int bits_per_sample;              // 位宽
        size_t number_of_frames;          // 帧数
    };

    /**
     * @brief 初始化Rockit音频设备
     * @return 是否初始化成功
     */
    bool InitializeAudioDevice();

    /**
     * @brief 音频处理线程函数
     */
    void AudioProcessingThread();

    /**
     * @brief 将PCM数据发送到Rockit音频设备
     * @param frame 音频帧
     * @return 是否发送成功
     */
    bool SendAudioFrameToDevice(const AudioFrame& frame);

    /**
     * @brief 计算音频PTS（用于音视频同步）
     * @return 当前PTS
     */
    int64_t CalculateAudioPts();

    /**
     * @brief 通知音频状态变化
     * @param state 状态码
     * @param message 状态描述
     */
    void NotifyAudioState(int state, const std::string& message);

    // 音频参数
    int sample_rate_;
    int channels_;
    int bits_per_sample_;
    int bytes_per_sample_;
    int frame_size_bytes_;

    // 音频缓冲队列
    std::queue<AudioFrame> audio_buffer_;
    std::mutex buffer_mutex_;
    size_t max_buffer_size_;  // 最大缓冲帧数

    // 音频设备
    int audio_device_id_;
    std::atomic<bool> is_device_working_;

    // 音频处理线程
    std::unique_ptr<std::thread> audio_thread_;
    std::atomic<bool> is_running_;
    std::atomic<bool> is_paused_;

    // 音视频同步
    std::mutex sync_mutex_;
    int64_t video_reference_pts_;
    int64_t video_reference_time_;
    int target_delay_ms_;
    int64_t first_audio_pts_;
    int64_t first_audio_time_;
    bool first_frame_received_;

    // 状态回调
    AudioStateCallback audio_state_callback_;
};
