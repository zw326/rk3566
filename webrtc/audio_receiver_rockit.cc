#include "audio_receiver_rockit.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <cstring>
#include <algorithm>

// 包含Rockit相关头文件
extern "C" {
#include "rk_common.h"
#include "rk_comm_aio.h"
#include "rk_comm_ao.h"
#include "rk_mpi.h"
}

// 音频状态码定义
enum AudioStateCode {
    AUDIO_STATE_INITIALIZED = 0,
    AUDIO_STATE_STARTED = 1,
    AUDIO_STATE_STOPPED = 2,
    AUDIO_STATE_DEVICE_ERROR = -1,
    AUDIO_STATE_BUFFER_OVERFLOW = -2,
    AUDIO_STATE_BUFFER_UNDERFLOW = -3,
    AUDIO_STATE_SYNC_RESET = 10,
};

// 辅助函数：获取当前系统时间（毫秒）
static int64_t GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

AudioReceiver::AudioReceiver()
    : sample_rate_(48000)
    , channels_(2)
    , bits_per_sample_(16)
    , bytes_per_sample_(2)
    , frame_size_bytes_(0)
    , max_buffer_size_(100)  // 默认最大缓冲100帧
    , audio_device_id_(0)
    , is_device_working_(false)
    , is_running_(false)
    , is_paused_(false)
    , video_reference_pts_(0)
    , video_reference_time_(0)
    , target_delay_ms_(40)  // 默认目标延迟40ms
    , first_audio_pts_(0)
    , first_audio_time_(0)
    , first_frame_received_(false) {
}

AudioReceiver::~AudioReceiver() {
    Stop();
}

bool AudioReceiver::Initialize(int sample_rate, int channels, int bits_per_sample) {
    // 保存音频参数
    sample_rate_ = sample_rate;
    channels_ = channels;
    bits_per_sample_ = bits_per_sample;
    bytes_per_sample_ = bits_per_sample / 8;
    
    // 初始化Rockit音频设备
    if (!InitializeAudioDevice()) {
        std::cerr << "Failed to initialize audio device" << std::endl;
        return false;
    }
    
    NotifyAudioState(AUDIO_STATE_INITIALIZED, "Audio receiver initialized");
    return true;
}

bool AudioReceiver::Start() {
    if (is_running_) {
        std::cout << "Audio receiver already running" << std::endl;
        return true;
    }
    
    is_running_ = true;
    is_paused_ = false;
    
    // 启动音频处理线程
    audio_thread_ = std::make_unique<std::thread>(&AudioReceiver::AudioProcessingThread, this);
    
    NotifyAudioState(AUDIO_STATE_STARTED, "Audio receiver started");
    return true;
}

void AudioReceiver::Stop() {
    if (!is_running_) {
        return;
    }
    
    // 停止处理线程
    is_running_ = false;
    if (audio_thread_ && audio_thread_->joinable()) {
        audio_thread_->join();
    }
    
    // 清空缓冲区
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        std::queue<AudioFrame> empty;
        std::swap(audio_buffer_, empty);
    }
    
    // 停止Rockit音频设备
    if (is_device_working_) {
        RK_MPI_AO_DisableChn(audio_device_id_, 0);
        is_device_working_ = false;
    }
    
    NotifyAudioState(AUDIO_STATE_STOPPED, "Audio receiver stopped");
}

void AudioReceiver::Reset() {
    // 重置音频同步状态
    {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        first_frame_received_ = false;
        first_audio_pts_ = 0;
        first_audio_time_ = 0;
    }
    
    // 清空缓冲区
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        std::queue<AudioFrame> empty;
        std::swap(audio_buffer_, empty);
    }
    
    NotifyAudioState(AUDIO_STATE_SYNC_RESET, "Audio sync reset");
}

void AudioReceiver::SetTargetDelayMs(int delay_ms) {
    target_delay_ms_ = delay_ms;
}

int AudioReceiver::GetCurrentDelayMs() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    // 估算当前缓冲区的延迟（毫秒）
    // 每帧的时长 = 1000ms / (采样率 / 每帧采样点数)
    if (audio_buffer_.empty()) {
        return 0;
    }
    
    // 假设每帧10ms（常见的WebRTC音频帧大小）
    return static_cast<int>(audio_buffer_.size() * 10);
}

void AudioReceiver::SetVideoReference(int64_t video_pts, int64_t system_time) {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    video_reference_pts_ = video_pts;
    video_reference_time_ = system_time;
}

size_t AudioReceiver::GetBufferSize() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return audio_buffer_.size();
}

void AudioReceiver::OnData(const void* audio_data,
                          int bits_per_sample,
                          int sample_rate,
                          size_t number_of_channels,
                          size_t number_of_frames,
                          absl::optional<int64_t> absolute_capture_timestamp_ms) {
    if (!is_running_ || is_paused_) {
        return;
    }
    
    // 检查缓冲区大小
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (audio_buffer_.size() >= max_buffer_size_) {
            // 缓冲区溢出，丢弃最旧的帧
            audio_buffer_.pop();
            NotifyAudioState(AUDIO_STATE_BUFFER_OVERFLOW, "Audio buffer overflow, dropping frame");
        }
    }
    
    // 计算帧大小
    size_t frame_size = number_of_frames * number_of_channels * (bits_per_sample / 8);
    
    // 创建音频帧
    AudioFrame frame;
    frame.data = std::make_unique<uint8_t[]>(frame_size);
    frame.size = frame_size;
    frame.sample_rate = sample_rate;
    frame.channels = number_of_channels;
    frame.bits_per_sample = bits_per_sample;
    frame.number_of_frames = number_of_frames;
    
    // 复制音频数据
    memcpy(frame.data.get(), audio_data, frame_size);
    
    // 计算PTS
    frame.pts = CalculateAudioPts();
    
    // 将帧加入缓冲区
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        audio_buffer_.push(std::move(frame));
    }
}

bool AudioReceiver::InitializeAudioDevice() {
    // 配置音频输出参数
    AUDIO_DEV AoDev = 0;
    AO_CHN AoChn = 0;
    AO_CHN_ATTR_S stAoAttr;
    
    memset(&stAoAttr, 0, sizeof(AO_CHN_ATTR_S));
    
    // 设置音频参数
    stAoAttr.enSampleFormat = (bits_per_sample_ == 16) ? RK_SAMPLE_FMT_S16 : RK_SAMPLE_FMT_S32;
    stAoAttr.u32Channels = channels_;
    stAoAttr.u32SampleRate = sample_rate_;
    stAoAttr.u32NbSamples = 1024;  // 每帧采样点数
    
    // 创建音频输出设备
    int ret = RK_MPI_AO_SetChnAttr(AoDev, AoChn, &stAoAttr);
    if (ret != RK_SUCCESS) {
        std::cerr << "Failed to set audio output attributes: " << ret << std::endl;
        return false;
    }
    
    // 启用音频输出通道
    ret = RK_MPI_AO_EnableChn(AoDev, AoChn);
    if (ret != RK_SUCCESS) {
        std::cerr << "Failed to enable audio output channel: " << ret << std::endl;
        return false;
    }
    
    audio_device_id_ = AoDev;
    is_device_working_ = true;
    std::cout << "Audio device initialized successfully" << std::endl;
    return true;
}

void AudioReceiver::AudioProcessingThread() {
    std::cout << "Audio processing thread started" << std::endl;
    
    while (is_running_) {
        if (is_paused_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        // 检查缓冲区
        AudioFrame frame;
        bool has_frame = false;
        
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            if (!audio_buffer_.empty()) {
                frame = std::move(audio_buffer_.front());
                audio_buffer_.pop();
                has_frame = true;
            }
        }
        
        if (has_frame) {
            // 发送音频帧到设备
            if (!SendAudioFrameToDevice(frame)) {
                std::cerr << "Failed to send audio frame to device" << std::endl;
                NotifyAudioState(AUDIO_STATE_DEVICE_ERROR, "Failed to send audio frame to device");
            }
        } else {
            // 缓冲区为空，等待一段时间
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            
            // 检查是否长时间没有数据
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty()) {
                    NotifyAudioState(AUDIO_STATE_BUFFER_UNDERFLOW, "Audio buffer underflow");
                }
            }
        }
    }
    
    std::cout << "Audio processing thread stopped" << std::endl;
}

bool AudioReceiver::SendAudioFrameToDevice(const AudioFrame& frame) {
    if (!is_device_working_) {
        return false;
    }
    
    // 创建Rockit音频帧
    AUDIO_FRAME_S audio_frame;
    memset(&audio_frame, 0, sizeof(AUDIO_FRAME_S));
    
    // 设置音频参数
    audio_frame.u32Len = frame.size;
    audio_frame.u64TimeStamp = frame.pts;
    audio_frame.enSampleFormat = (frame.bits_per_sample == 16) ? RK_SAMPLE_FMT_S16 : RK_SAMPLE_FMT_S32;
    audio_frame.u32Channels = frame.channels;
    audio_frame.u32SampleRate = frame.sample_rate;
    audio_frame.u32Samples = frame.number_of_frames;
    
    // 分配内存块
    MB_BLK mb = RK_MPI_MB_CreateBuffer(frame.size, false);
    if (!mb) {
        std::cerr << "Failed to create memory block for audio frame" << std::endl;
        return false;
    }
    
    // 复制音频数据
    void* mb_data = RK_MPI_MB_GetPtr(mb);
    memcpy(mb_data, frame.data.get(), frame.size);
    audio_frame.pMbBlk = mb;
    
    // 发送音频帧到设备
    int ret = RK_MPI_AO_SendFrame(audio_device_id_, 0, &audio_frame, -1);
    if (ret != RK_SUCCESS) {
        RK_MPI_MB_ReleaseBuffer(mb);
        return false;
    }
    
    return true;
}

int64_t AudioReceiver::CalculateAudioPts() {
    int64_t current_time = GetCurrentTimeMs();
    
    // 第一帧音频
    if (!first_frame_received_) {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        first_frame_received_ = true;
        first_audio_time_ = current_time;
        
        // 如果有视频参考，使用视频PTS作为基准
        if (video_reference_time_ > 0) {
            first_audio_pts_ = video_reference_pts_;
            return first_audio_pts_;
        } else {
            // 否则从0开始
            first_audio_pts_ = 0;
            return 0;
        }
    }
    
    // 计算相对于第一帧的时间差
    int64_t elapsed = current_time - first_audio_time_;
    
    // 计算PTS
    int64_t pts = first_audio_pts_ + elapsed;
    
    // 音视频同步调整
    std::lock_guard<std::mutex> lock(sync_mutex_);
    if (video_reference_time_ > 0) {
        // 计算音频和视频的时间差
        int64_t video_elapsed = current_time - video_reference_time_;
        int64_t expected_audio_pts = video_reference_pts_ + video_elapsed;
        
        // 计算音频和视频的PTS差异
        int64_t pts_diff = pts - expected_audio_pts;
        
        // 如果差异超过阈值，进行调整
        if (std::abs(pts_diff) > target_delay_ms_) {
            // 平滑调整，不要一次性调整太多
            int64_t adjustment = pts_diff / 4;
            pts -= adjustment;
            
            // 更新基准
            first_audio_pts_ = pts - elapsed;
        }
    }
    
    return pts;
}

void AudioReceiver::NotifyAudioState(int state, const std::string& message) {
    if (audio_state_callback_) {
        audio_state_callback_(state, message);
    }
}
