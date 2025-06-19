#include "encoded_video_frame_handler_rockit.h"
#include <iostream>
#include <chrono>

// 包含Rockit相关头文件
extern "C" {
#include "rk_common.h"
#include "rk_comm_video.h"
#include "rk_comm_vdec.h"
#include "rk_comm_vo.h"
#include "rk_mpi.h"
}

// 视频状态码定义
enum VideoStateCode {
    VIDEO_STATE_INITIALIZED = 0,
    VIDEO_STATE_STARTED = 1,
    VIDEO_STATE_STOPPED = 2,
    VIDEO_STATE_FIRST_FRAME = 3,
    VIDEO_STATE_KEY_FRAME = 4,
    VIDEO_STATE_DECODER_ERROR = -1,
    VIDEO_STATE_DISPLAY_ERROR = -2,
    VIDEO_STATE_SYNC_RESET = 10,
};

// 辅助函数：获取当前系统时间（毫秒）
static int64_t GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

EncodedVideoFrameHandler::EncodedVideoFrameHandler()
    : width_(1920)
    , height_(1080)
    , codec_type_("H264")
    , vdec_chn_(0)
    , vo_chn_(0)
    , is_initialized_(false)
    , is_running_(false)
    , is_decoder_ready_(false)
    , is_display_ready_(false)
    , first_frame_pts_(0)
    , first_frame_time_(0)
    , first_frame_received_(false) {
}

EncodedVideoFrameHandler::~EncodedVideoFrameHandler() {
    Stop();
}

bool EncodedVideoFrameHandler::Initialize(int width, int height, const std::string& codec_type) {
    if (is_initialized_) {
        std::cout << "EncodedVideoFrameHandler already initialized" << std::endl;
        return true;
    }
    
    // 保存视频参数
    width_ = width;
    height_ = height;
    codec_type_ = codec_type;
    
    // 初始化Rockit解码器
    if (!InitializeDecoder()) {
        std::cerr << "Failed to initialize decoder" << std::endl;
        return false;
    }
    
    // 初始化Rockit显示输出
    if (!InitializeDisplay()) {
        std::cerr << "Failed to initialize display" << std::endl;
        return false;
    }
    
    is_initialized_ = true;
    NotifyVideoState(VIDEO_STATE_INITIALIZED, "Video handler initialized");
    return true;
}

bool EncodedVideoFrameHandler::Start() {
    if (!is_initialized_) {
        std::cerr << "EncodedVideoFrameHandler not initialized" << std::endl;
        return false;
    }
    
    if (is_running_) {
        std::cout << "EncodedVideoFrameHandler already running" << std::endl;
        return true;
    }
    
    is_running_ = true;
    NotifyVideoState(VIDEO_STATE_STARTED, "Video handler started");
    return true;
}

void EncodedVideoFrameHandler::Stop() {
    if (!is_running_) {
        return;
    }
    
    is_running_ = false;
    
    // 停止Rockit解码器
    if (is_decoder_ready_) {
        RK_MPI_VDEC_StopRecvStream(vdec_chn_);
        RK_MPI_VDEC_DestroyChn(vdec_chn_);
        is_decoder_ready_ = false;
    }
    
    // 停止Rockit显示输出
    if (is_display_ready_) {
        RK_MPI_VO_DisableChn(0, vo_chn_);
        is_display_ready_ = false;
    }
    
    NotifyVideoState(VIDEO_STATE_STOPPED, "Video handler stopped");
}

void EncodedVideoFrameHandler::Reset() {
    // 重置同步状态
    {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        first_frame_received_ = false;
        first_frame_pts_ = 0;
        first_frame_time_ = 0;
    }
    
    NotifyVideoState(VIDEO_STATE_SYNC_RESET, "Video sync reset");
}

webrtc::Result EncodedVideoFrameHandler::OnEncodedImage(
    const webrtc::EncodedImage& encoded_image,
    const webrtc::CodecSpecificInfo* codec_specific_info) {
    
    if (!is_running_) {
        return webrtc::Result(webrtc::Result::OK, "Handler not running");
    }
    
    // 获取编码数据
    const uint8_t* data = encoded_image.data();
    size_t size = encoded_image.size();
    int64_t capture_time_ms = encoded_image.capture_time_ms_;
    bool is_key_frame = encoded_image._frameType == webrtc::VideoFrameType::kVideoFrameKey;
    
    // 解码并显示帧
    if (!DecodeAndDisplayFrame(data, size, capture_time_ms, is_key_frame)) {
        std::cerr << "Failed to decode and display frame" << std::endl;
        return webrtc::Result(webrtc::Result::ERROR, "Failed to decode frame");
    }
    
    return webrtc::Result(webrtc::Result::OK);
}

bool EncodedVideoFrameHandler::InitializeDecoder() {
    // 配置解码器参数
    VDEC_CHN_ATTR_S vdec_attr;
    memset(&vdec_attr, 0, sizeof(VDEC_CHN_ATTR_S));
    
    // 设置解码器类型
    if (codec_type_ == "H264") {
        vdec_attr.enCodecType = RK_CODEC_TYPE_H264;
    } else if (codec_type_ == "H265") {
        vdec_attr.enCodecType = RK_CODEC_TYPE_H265;
    } else {
        std::cerr << "Unsupported codec type: " << codec_type_ << std::endl;
        return false;
    }
    
    // 设置解码模式
    vdec_attr.enMode = VIDEO_MODE_FRAME;
    
    // 设置图像信息
    vdec_attr.u32PicWidth = width_;
    vdec_attr.u32PicHeight = height_;
    vdec_attr.u32FrameBufCnt = 8;  // 帧缓冲数量
    
    // 创建解码通道
    int ret = RK_MPI_VDEC_CreateChn(vdec_chn_, &vdec_attr);
    if (ret != RK_SUCCESS) {
        std::cerr << "Failed to create VDEC channel: " << ret << std::endl;
        return false;
    }
    
    // 启动接收流
    ret = RK_MPI_VDEC_StartRecvStream(vdec_chn_);
    if (ret != RK_SUCCESS) {
        std::cerr << "Failed to start receiving stream: " << ret << std::endl;
        RK_MPI_VDEC_DestroyChn(vdec_chn_);
        return false;
    }
    
    is_decoder_ready_ = true;
    std::cout << "Decoder initialized successfully" << std::endl;
    return true;
}

bool EncodedVideoFrameHandler::InitializeDisplay() {
    // 配置显示输出参数
    VO_CHN_ATTR_S vo_attr;
    memset(&vo_attr, 0, sizeof(VO_CHN_ATTR_S));
    
    // 设置显示区域
    vo_attr.pcDevNode = "/dev/dri/card0";  // DRM设备节点
    vo_attr.u32Width = width_;
    vo_attr.u32Height = height_;
    vo_attr.stImgRect.s32X = 0;
    vo_attr.stImgRect.s32Y = 0;
    vo_attr.stImgRect.u32Width = width_;
    vo_attr.stImgRect.u32Height = height_;
    vo_attr.stDispRect.s32X = 0;
    vo_attr.stDispRect.s32Y = 0;
    vo_attr.stDispRect.u32Width = width_;
    vo_attr.stDispRect.u32Height = height_;
    
    // 创建显示通道
    int ret = RK_MPI_VO_CreateChn(0, vo_chn_, &vo_attr);
    if (ret != RK_SUCCESS) {
        std::cerr << "Failed to create VO channel: " << ret << std::endl;
        return false;
    }
    
    // 绑定解码器和显示输出
    MPP_CHN_S src_chn;
    src_chn.enModId = RK_ID_VDEC;
    src_chn.s32DevId = 0;
    src_chn.s32ChnId = vdec_chn_;
    
    MPP_CHN_S dst_chn;
    dst_chn.enModId = RK_ID_VO;
    dst_chn.s32DevId = 0;
    dst_chn.s32ChnId = vo_chn_;
    
    ret = RK_MPI_SYS_Bind(&src_chn, &dst_chn);
    if (ret != RK_SUCCESS) {
        std::cerr << "Failed to bind VDEC and VO: " << ret << std::endl;
        RK_MPI_VO_DestroyChn(0, vo_chn_);
        return false;
    }
    
    is_display_ready_ = true;
    std::cout << "Display initialized successfully" << std::endl;
    return true;
}

bool EncodedVideoFrameHandler::DecodeAndDisplayFrame(
    const uint8_t* encoded_data, size_t encoded_size, int64_t pts, bool is_key_frame) {
    
    if (!is_decoder_ready_ || !is_display_ready_) {
        std::cerr << "Decoder or display not ready" << std::endl;
        return false;
    }
    
    // 创建视频帧
    VIDEO_FRAME_INFO_S video_frame;
    memset(&video_frame, 0, sizeof(VIDEO_FRAME_INFO_S));
    
    // 分配内存块
    MB_BLK mb = RK_MPI_MB_CreateBuffer(encoded_size, false);
    if (!mb) {
        std::cerr << "Failed to create memory block" << std::endl;
        return false;
    }
    
    // 复制编码数据
    void* mb_data = RK_MPI_MB_GetPtr(mb);
    memcpy(mb_data, encoded_data, encoded_size);
    
    // 设置视频帧参数
    video_frame.stVFrame.pMbBlk = mb;
    video_frame.stVFrame.u32Width = width_;
    video_frame.stVFrame.u32Height = height_;
    video_frame.stVFrame.u32VirWidth = width_;
    video_frame.stVFrame.u32VirHeight = height_;
    video_frame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
    video_frame.stVFrame.u64PTS = pts;
    
    // 发送帧到解码器
    int ret = RK_MPI_VDEC_SendFrame(vdec_chn_, &video_frame, -1);
    RK_MPI_MB_ReleaseBuffer(mb);
    
    if (ret != RK_SUCCESS) {
        std::cerr << "Failed to send frame to decoder: " << ret << std::endl;
        NotifyVideoState(VIDEO_STATE_DECODER_ERROR, "Failed to send frame to decoder");
        return false;
    }
    
    // 处理同步
    int64_t current_time = GetCurrentTimeMs();
    
    // 第一帧
    if (!first_frame_received_) {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        first_frame_received_ = true;
        first_frame_pts_ = pts;
        first_frame_time_ = current_time;
        
        NotifyVideoState(VIDEO_STATE_FIRST_FRAME, "First video frame received");
        
        // 通知音频同步
        if (audio_sync_callback_) {
            audio_sync_callback_(pts, current_time);
        }
    }
    
    // 关键帧
    if (is_key_frame) {
        NotifyVideoState(VIDEO_STATE_KEY_FRAME, "Key frame received");
        
        // 更新同步参考
        std::lock_guard<std::mutex> lock(sync_mutex_);
        
        // 通知音频同步
        if (audio_sync_callback_) {
            audio_sync_callback_(pts, current_time);
        }
    }
    
    return true;
}

void EncodedVideoFrameHandler::NotifyVideoState(int state, const std::string& message) {
    if (video_state_callback_) {
        video_state_callback_(state, message);
    }
}
