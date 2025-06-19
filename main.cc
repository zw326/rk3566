#include <iostream>
#include <string>
#include <signal.h>
#include <thread>
#include <chrono>
#include <atomic>
#include "webrtc/webrtc_client.h"

// 全局变量
std::atomic<bool> g_running(true);

// 信号处理函数
void SignalHandler(int signal) {
    std::cout << "Received signal: " << signal << std::endl;
    g_running = false;
}

int main(int argc, char* argv[]) {
    // 设置信号处理
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // 默认信令服务器URL
    std::string signaling_url = "ws://127.0.0.1:8080";

    // 解析命令行参数
    if (argc > 1) {
        signaling_url = argv[1];
    }

    std::cout << "RV1126 WebRTC Receiver" << std::endl;
    std::cout << "Signaling server: " << signaling_url << std::endl;

    // 创建WebRTC客户端
    WebRTCClient client;

    // 设置状态回调
    client.SetStateChangeCallback([](const std::string& state, const std::string& description) {
        std::cout << "State changed: " << state << " - " << description << std::endl;
    });

    // 初始化WebRTC客户端
    if (!client.Initialize()) {
        std::cerr << "Failed to initialize WebRTC client" << std::endl;
        return 1;
    }

    // 连接到信令服务器
    if (!client.ConnectToSignalingServer(signaling_url)) {
        std::cerr << "Failed to connect to signaling server" << std::endl;
        return 1;
    }

    // 主循环
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 清理资源
    client.Close();

    std::cout << "Application exiting..." << std::endl;
    return 0;
}
