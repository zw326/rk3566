#include <iostream>
#include <string>
#include <csignal> // 用于 signal
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>   // 用于智能指针

// 包含我们所有的核心模块
#include "webrtc/webrtc_client.h"
#include "webrtc/encoded_video_frame_handler_rockit.h"
#include "webrtc/audio_receiver_rockit.h"

// 引入Rockchip MPP系统控制头文件
extern "C" {
#include "rk_mpi_sys.h"
}

// 全局运行状态标志 (来自您的版本)
std::atomic<bool> g_running(true);

// 信号处理函数，用于优雅地退出程序 (来自您的版本，更完整)
void SignalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\nCaught signal " << signal << ", shutting down gracefully..." << std::endl;
        g_running = false;
    }
}

int main(int argc, char* argv[]) {
    // 1. 参数解析 (来自您的版本)
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <signaling_url> <room_id> [client_id]" << std::endl;
        std::cerr << "Example: " << argv[0] << " ws://192.168.1.10:8080 101 rk3566_receiver" << std::endl;
        return 1;
    }
    std::string signaling_url = argv[1];
    std::string room_id = argv[2];
    std::string client_id = (argc > 3) ? argv[3] : "rk3566_receiver";

    // 2. 打印友好的启动日志 (来自您的版本)
    std::cout << "--- RK3566 WebRTC Receiver ---" << std::endl;
    std::cout << "Signaling Server: " << signaling_url << std::endl;
    std::cout << "Room ID: " << room_id << std::endl;
    std::cout << "Client ID: " << client_id << std::endl;
    std::cout << "---------------------------------" << std::endl;
    
    // 3. 初始化Rockchip MPP系统 (来自我的版本，至关重要)
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        std::cerr << "Fatal: Failed to initialize Rockchip MPP system." << std::endl;
        return -1;
    }
    std::cout << "Rockchip MPP system initialized." << std::endl;

    // 4. 设置信号处理 (来自您的版本)
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // 5. 创建核心对象 (使用智能指针)
    auto webRTCClient = std::make_unique<WebRTCClient>();
    auto videoHandler = std::make_shared<EncodedVideoFrameHandler>();
    auto audioHandler = std::make_shared<AudioReceiver>();

    // 6. 设置回调，用于打印状态日志 (通用实践)
    webRTCClient->SetStateChangeCallback([](const std::string& state, const std::string& description) {
        std::cout << "[WebRTC State] " << state << ": " << description << std::endl;
    });
    // (可以为 videoHandler 和 audioHandler 添加类似的回调)
    videoHandler->SetVideoStateCallback([](int state, const std::string& msg){
        std::cout << "[Video State] code " << state << ": " << msg << std::endl;
    });
    audioHandler->SetAudioStateCallback([](int state, const std::string& msg){
        std::cout << "[Audio State] code " << state << ": " << msg << std::endl;
    });

    // 7. 依赖注入与组件初始化 (来自我的版本，顺序很重要)
    webRTCClient->SetMediaHandlers(videoHandler, audioHandler);

    if (!videoHandler->Initialize() || !audioHandler->Initialize() || !webRTCClient->Initialize()) {
        std::cerr << "Fatal: Failed to initialize one or more components." << std::endl;
        RK_MPI_SYS_Exit();
        return -1;
    }

    // 8. 启动处理流程 (来自我的版本，逻辑更清晰)
    videoHandler->Start();
    audioHandler->Start();
    webRTCClient->ConnectToSignalingServer(signaling_url, room_id, client_id);

    // 9. 主循环 (来自您的版本)
    std::cout << "Receiver is running. Press Ctrl+C to exit." << std::endl;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 10. [修改] 优化资源清理顺序，确保健壮性
    std::cout << "Shutting down all components..." << std::endl;
    
    // a. 首先停止WebRTC客户端，这将停止所有网络活动和数据回调
    webRTCClient->Cleanup();
    std::cout << "WebRTC client cleaned up." << std::endl;
    
    // b. 然后停止媒体处理器，它们不再会接收到新数据
    audioHandler->Stop();
    std::cout << "Audio handler stopped." << std::endl;
    videoHandler->Stop();
    std::cout << "Video handler stopped." << std::endl;
    
    // c. 最后释放MPP系统资源
    RK_MPI_SYS_Exit();
    std::cout << "Rockchip MPP system exited." << std::endl;
    std::cout << "Application exited gracefully." << std::endl;
    
    return 0;
}