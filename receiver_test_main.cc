#include <iostream>
#include <string>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>
#include <json/json.h>

// 包含您的信令客户端实现
#include "signaling/signaling_client_ws.h"

// 全局运行状态标志
std::atomic<bool> g_running(true);

// 信号处理函数，用于优雅地退出程序
void SignalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\nCaught signal " << signal << ", shutting down..." << std::endl;
        g_running = false;
    }
}

int main(int argc, char* argv[]) {
    // 检查命令行参数
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <signaling_url> <room_id> <client_id>" << std::endl;
        std::cerr << "Example: " << argv[0] << " ws://127.0.0.1:8080 101 rk3566_receiver" << std::endl;
        return 1;
    }

    // 设置信号处理
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // 解析命令行参数
    std::string signaling_url = argv[1];
    std::string room_id = argv[2];
    std::string client_id = argv[3];

    std::cout << "--- C++ Signaling Receiver Test ---" << std::endl;

    // 1. 创建信令客户端实例
    auto signaling_client = std::make_unique<WebSocketSignalingClient>();

    // 2. 设置状态回调，用于打印连接状态
    signaling_client->SetStateCallback([](bool connected, const std::string& message) {
        if (connected) {
            std::cout << "✅ State changed: Connected to server." << std::endl;
        } else {
            std::cout << "❌ State changed: Disconnected. Reason: " << message << std::endl;
        }
    });

    // 3. 设置消息回调，这是测试的核心逻辑
    signaling_client->SetMessageCallback(
        // 使用[&]捕获外部变量，以便在lambda中使用signaling_client
        [&signaling_client](SignalingClient::MessageType type, const std::string& message_str) {
            std::cout << "<- Received message. Type: " << static_cast<int>(type) << std::endl;

            // 解析收到的JSON，以获取发送方ID
            Json::CharReaderBuilder builder;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            Json::Value root;
            if (!reader->parse(message_str.c_str(), message_str.c_str() + message_str.length(), &root, nullptr)) {
                std::cerr << "Failed to parse message: " << message_str << std::endl;
                return;
            }
            std::cout << "   Content: " << message_str << std::endl;
            
            // 如果收到Offer，就模拟回复一个Answer
            if (type == SignalingClient::MessageType::OFFER) {
                if (root.isMember("from") && root["from"].isString()) {
                    std::string remote_id = root["from"].asString();
                    std::cout << "   It's an offer from " << remote_id << ". Sending answer back..." << std::endl;
                    
                    // 模拟一个SDP Answer
                    std::string fake_answer_sdp = "v=0 o=- 98765 54321 IN IP4 receiver.example.com ... this is a fake answer";
                    
                    // 发送Answer给对方
                    signaling_client->SendAnswer(fake_answer_sdp, remote_id);
                    std::cout << "-> Sent: answer (to: " << remote_id << ")" << std::endl;
                }
            } else if (type == SignalingClient::MessageType::CANDIDATE) {
                 std::cout << "   It's a candidate. Test OK." << std::endl;
            }
        });

    //在连接之前，先调用Register来“配置”注册信息。    
    signaling_client->Register(room_id, client_id);

    // 然后再发起连接。
    // 当连接成功后，它内部的回调会自动再次调用Register，此时才会真正发送消息。
    std::cout << "Connecting to " << signaling_url << "..." << std::endl;
    signaling_client->Connect(signaling_url);
    

    // 主循环，保持程序运行
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 清理资源
    signaling_client->Close();

    std::cout << "Test application exited gracefully." << std::endl;
    return 0;
}