#include "encoded_video_frame_handler_rockit.h"
#include <iostream>
#include <chrono>

// 包含Rockit相关头文件
extern "C" {
#include "rk_debug.h"
#include "rk_common.h"
#include "rk_comm_video.h"
#include "rk_comm_vdec.h"
#include "rk_comm_vo.h"
#include "rk_comm_sys.h" // [新增] 为SYS_Bind提供结构体
#include "rk_comm_mb.h"   // [新增] 为MB_BLK提供结构体

#include "rk_mpi_vdec.h" // [新增] VDEC模块API
#include "rk_mpi_vo.h"   // [新增] VO模块API
#include "rk_mpi_sys.h"  // [新增] 系统控制模块API（Bind, CreateMB等）
#include "rk_mpi_mb.h"   // [新增] 内存块模块API
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
    

    is_initialized_ = true;
    is_decoder_ready_ = false; // 确保解码器和显示初始为未就绪
    is_display_ready_ = false;

    NotifyVideoState(VIDEO_STATE_INITIALIZED, "Video handler initialized, waiting for first frame.");
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

webrtc::EncodedImageCallback::Result EncodedVideoFrameHandler::OnEncodedImage(
    const webrtc::EncodedImage& encoded_image,
    const webrtc::CodecSpecificInfo* codec_specific_info) {
    
    if (!is_running_) {
        // 成功，但告知WebRTC我们不处理（虽然这里实际上不会发生）
        return webrtc::EncodedImageCallback::Result(webrtc::EncodedImageCallback::Result::OK, encoded_image.RtpTimestamp());
    }
    
    // 【核心修改】检查解码器和显示是否已就绪
    if (!is_decoder_ready_ || !is_display_ready_) {
        // 从视频帧中获取真实的分辨率
        width_ = encoded_image._encodedWidth;
        height_ = encoded_image._encodedHeight;
        std::cout << "First frame received. Dyanmic resolution: " 
                  << width_ << "x" << height_ << std::endl;

        // 使用真实分辨率初始化解码器和显示
        if (!InitializeDecoder()) {
            std::cerr << "Failed to initialize decoder with dynamic resolution" << std::endl;
            return webrtc::EncodedImageCallback::Result(webrtc::EncodedImageCallback::Result::ERROR_SEND_FAILED, encoded_image.RtpTimestamp());
        }
        if (!InitializeDisplay()) {
            std::cerr << "Failed to initialize display with dynamic resolution" << std::endl;
            return webrtc::EncodedImageCallback::Result(webrtc::EncodedImageCallback::Result::ERROR_SEND_FAILED, encoded_image.RtpTimestamp());
        }
    }
    // 获取编码数据
    const uint8_t* data = encoded_image.data();
    size_t size = encoded_image.size();
    // [修正3] 在新版API中，ntp_time_ms_ 和 capture_time_ms_ 都需要从 presentation_timestamp 中获取
    int64_t capture_time_ms = encoded_image.PresentationTimestamp().value_or(webrtc::Timestamp::MinusInfinity()).ms();
    bool is_key_frame = encoded_image._frameType == webrtc::VideoFrameType::kVideoFrameKey;
    
    // 解码并显示帧
    if (!DecodeAndDisplayFrame(data, size, capture_time_ms, is_key_frame)) {
        std::cerr << "Failed to decode and display frame" << std::endl;
        return webrtc::EncodedImageCallback::Result(webrtc::EncodedImageCallback::Result::ERROR_SEND_FAILED, encoded_image.RtpTimestamp());
    }
    
    return webrtc::EncodedImageCallback::Result(webrtc::EncodedImageCallback::Result::OK, encoded_image.RtpTimestamp());
}

bool EncodedVideoFrameHandler::InitializeDecoder() {
    // 配置解码器参数
    VDEC_CHN_ATTR_S vdec_attr;
    memset(&vdec_attr, 0, sizeof(VDEC_CHN_ATTR_S));
    
    // 设置解码器类型
    if (codec_type_ == "H264") {
        vdec_attr.enType = RK_VIDEO_ID_AVC;
    } else if (codec_type_ == "H265") {
        vdec_attr.enType = RK_VIDEO_ID_HEVC;
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
    // 定义要使用的VO设备和图层。对于RK356x，通常使用设备0（如HDMI）和图层0（主视频层）
    VO_DEV VoDev = 0;
    VO_LAYER VoLayer = 0; 
    // 在绑定模式下，VO通道通常也用0
    vo_chn_ = 0; 

    // 1. 配置并启用显示设备 (Device)
    VO_PUB_ATTR_S stVoPubAttr;
    memset(&stVoPubAttr, 0, sizeof(stVoPubAttr));
    // 设置接口类型，例如HDMI
    stVoPubAttr.enIntfType = VO_INTF_HDMI;
    // 设置时序/分辨率，例如1080P@60Hz
    stVoPubAttr.enIntfSync = VO_OUTPUT_1080P60; 

    int ret = RK_MPI_VO_SetPubAttr(VoDev, &stVoPubAttr);
    if (ret != RK_SUCCESS) {
        RK_LOGE("Failed to set VO public attributes, error code: %#x", ret);
        return false;
    }

    ret = RK_MPI_VO_Enable(VoDev);
    if (ret != RK_SUCCESS) {
        RK_LOGE("Failed to enable VO device, error code: %#x", ret);
        return false;
    }

    // 2. 配置并启用视频图层 (Layer)
    VO_VIDEO_LAYER_ATTR_S stLayerAttr;
    memset(&stLayerAttr, 0, sizeof(stLayerAttr));
    // 设置图层的显示区域和画布大小，应与视频分辨率一致
    stLayerAttr.stDispRect = {0, 0, static_cast<RK_U32>(width_), static_cast<RK_U32>(height_)};
    stLayerAttr.stImageSize = {static_cast<RK_U32>(width_), static_cast<RK_U32>(height_)};
    // 设置图层期望接收的像素格式，应与VDEC解码输出的格式一致
    stLayerAttr.enPixFormat = RK_FMT_YUV420SP; 
    stLayerAttr.u32DispFrmRt = 60; // 设置显示帧率

    ret = RK_MPI_VO_SetLayerAttr(VoLayer, &stLayerAttr);
    if (ret != RK_SUCCESS) {
        RK_LOGE("Failed to set VO layer attributes, error code: %#x", ret);
        RK_MPI_VO_Disable(VoDev); // 清理已启用的设备
        return false;
    }

    ret = RK_MPI_VO_EnableLayer(VoLayer);
    if (ret != RK_SUCCESS) {
        RK_LOGE("Failed to enable VO layer, error code: %#x", ret);
        RK_MPI_VO_Disable(VoDev); // 清理已启用的设备
        return false;
    }

    // 3. 将VDEC通道绑定到VO图层上，实现零拷贝
    MPP_CHN_S stSrcChn; // 数据源：VDEC
    stSrcChn.enModId = RK_ID_VDEC;
    stSrcChn.s32DevId = 0; // VDEC设备ID通常为0
    stSrcChn.s32ChnId = vdec_chn_;

    MPP_CHN_S stDestChn; // 目标：VO
    stDestChn.enModId = RK_ID_VO;
    stDestChn.s32DevId = VoLayer; // 在VO模块中，设备ID常被用来指代图层ID
    stDestChn.s32ChnId = vo_chn_;

    ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
    if (ret != RK_SUCCESS) {
        RK_LOGE("Failed to bind VDEC and VO, error code: %#x", ret);
        // 清理已启用的图层和设备
        RK_MPI_VO_DisableLayer(VoLayer);
        RK_MPI_VO_Enable(VoDev);
        return false;
    }
    
    is_display_ready_ = true;
    std::cout << "Display initialized and bound to VDEC successfully." << std::endl;
    return true;


}

bool EncodedVideoFrameHandler::DecodeAndDisplayFrame(
    const uint8_t* encoded_data, size_t encoded_size, int64_t pts, bool is_key_frame) {
    
    if (!is_decoder_ready_ || !is_display_ready_) {
        std::cerr << "Decoder or display not ready" << std::endl;
        return false;
    }

    // 1. 使用 C 标准库的 malloc 申请内存
    void* buffer_data = malloc(encoded_size);
    if (!buffer_data) {
        std::cerr << "Failed to malloc buffer for encoded data" << std::endl;
        return false;
    }
    memcpy(buffer_data, encoded_data, encoded_size);

    // 2. 准备用户数据和回调函数，用于内存的自动释放
    UserData* userData = new UserData();
    userData->buffer = buffer_data;

    MB_EXT_CONFIG_S stMbExtConfig;
    memset(&stMbExtConfig, 0, sizeof(MB_EXT_CONFIG_S));
    stMbExtConfig.pFreeCB = FreeCallback;    // 设置释放回调函数
    stMbExtConfig.pOpaque = userData;        // 传递用户数据指针
    stMbExtConfig.pu8VirAddr = (RK_U8*)buffer_data;
    stMbExtConfig.u64Size = encoded_size;

    // 3. 使用 RK_MPI_SYS_CreateMB 将我们自己的内存“包装”成一个 MB_BLK 句柄
    MB_BLK mb_handle = RK_NULL;
    int ret = RK_MPI_SYS_CreateMB(&mb_handle, &stMbExtConfig);
    if (ret != RK_SUCCESS) {
        std::cerr << "Failed to create MB from external buffer, error: " << ret << std::endl;
        free(buffer_data);
        delete userData;
        return false;
    }

    // 4. 配置 VDEC_STREAM_S
    VDEC_STREAM_S stStream;
    memset(&stStream, 0, sizeof(VDEC_STREAM_S));
    stStream.pMbBlk = mb_handle;
    stStream.u32Len = encoded_size;
    stStream.u64PTS = pts;
    stStream.bEndOfStream = RK_FALSE;
    stStream.bEndOfFrame = RK_TRUE;
    stStream.bBypassMbBlk = RK_TRUE; // 【核心】设置为 TRUE，启用直通模式

    // 5. 发送码流给解码器
    ret = RK_MPI_VDEC_SendStream(vdec_chn_, &stStream, -1);
    if (ret != RK_SUCCESS) {
        std::cerr << "Failed to send stream to decoder in bypass mode: " << ret << std::endl;
        // 如果发送失败，MPI不会接管内存，我们需要自己释放
        RK_MPI_MB_ReleaseMB(mb_handle); // ReleaseMB会触发回调
        return false;
    }

    // 6. 释放 MB_BLK 句柄
    //    注意：这里只释放句柄，真正的内存(buffer_data)所有权已经移交给MPI，
    //    将在MPI内部使用完毕后，通过我们设置的 FreeCallback 函数来释放。
    RK_MPI_MB_ReleaseMB(mb_handle);
    
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

void EncodedVideoFrameHandler::OnDroppedFrame(DropReason reason) {
    // 暂时只打印日志，表示我们知道有帧被丢弃了
    // 注意：DropReason 是一个枚举，不能直接用 << 打印，需要转成整数
    std::cerr << "A video frame has been dropped, reason code: " << static_cast<int>(reason) << std::endl;
}
