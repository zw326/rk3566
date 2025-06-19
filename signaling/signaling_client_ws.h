#pragma once
#include "signaling_client.h"
#include <libwebsockets.h>
#include <json/json.h>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <unordered_map>

/**
 * @brief 基于WebSocket的信令客户端实现
 * 
 * 该类实现了SignalingClient接口，使用libwebsockets库
 * 与WebSocket信令服务器通信
 */
class WebSocketSignalingClient : public SignalingClient {
public:
    //构造函数
    WebSocketSignalingClient();

    //析构函数
    ~WebSocketSignalingClient() override;

    // SignalingClient接口实现
    bool Connect(const std::string& url) override;
    void Close() override;
    bool Register(const std::string& room_id, const std::string& client_id = "") override;
    bool SendOffer(const std::string& sdp, const std::string& target_id = "") override;
    bool SendAnswer(const std::string& sdp, const std::string& target_id = "") override;
    bool SendCandidate(const std::string& sdp_mid, int sdp_mline_index,
                      const std::string& candidate, const std::string& target_id = "") override;
    bool SendLeave() override;
    void SetStateCallback(StateCallback callback) override;
    void SetMessageCallback(MessageCallback callback) override;
    bool IsConnected() const override;
    std::string GetRoomId() const override;
    std::string GetClientId() const override;

private:
    /**
     * @brief 消息结构体
     */
    struct Message {
        MessageType type;
        std::string content;
        std::string target_id;
    };

    /**
     * @brief 启动WebSocket客户端线程
     * @return 是否成功启动
     */
    bool StartWebSocketThread();

    /**
     * @brief 停止WebSocket客户端线程
     */
    void StopWebSocketThread();

    /**
     * @brief 解析服务器URL
     * @param url 服务器URL
     * @return 是否解析成功
     */
    bool ParseServerUrl(const std::string& url);

    /**
     * @brief 创建WebSocket连接
     * @return 是否成功创建
     */
    bool CreateWebSocketConnection();

    /**
     * @brief 发送消息
     * @param type 消息类型
     * @param content 消息内容
     * @param target_id 目标客户端ID（可选）
     * @return 是否成功发送
     */
    bool SendMessage(MessageType type, const std::string& content, const std::string& target_id = "");

    /**
     * @brief 处理接收到的消息
     * @param message 消息内容
     */
    void HandleReceivedMessage(const std::string& message);

    /**
     * @brief 尝试重新连接
     */
    void TryReconnect();

    /**
     * @brief WebSocket回调函数
     * @param wsi WebSocket实例
     * @param reason 回调原因
     * @param user 用户数据
     * @param in 输入数据
     * @param len 数据长度
     * @return 回调结果
     */
    static int WebSocketCallback(struct lws* wsi, enum lws_callback_reasons reason,
                                void* user, void* in, size_t len);

    /**
     * @brief 将消息类型转换为字符串
     * @param type 消息类型
     * @return 类型字符串
     */
    static std::string MessageTypeToString(MessageType type);

    /**
     * @brief 将字符串转换为消息类型
     * @param type_str 类型字符串
     * @return 消息类型
     */
    static MessageType StringToMessageType(const std::string& type_str);

    // WebSocket相关
    std::string scheme_;
    std::string host_;
    int port_;
    std::string path_;
    struct lws_context* context_;
    struct lws* websocket_connection_;
    struct lws_protocols protocols_[2];  // 一个用于WebSocket，一个用于NULL终止
    std::thread websocket_thread_;
    std::atomic<bool> should_exit_;

    // 连接状态
    std::atomic<bool> is_connected_;
    std::atomic<bool> is_connecting_;
    std::atomic<bool> should_reconnect_;
    int reconnect_attempts_;
    static const int max_reconnect_attempts_ = 5;

    // 消息队列
    std::queue<Message> message_queue_;
    std::mutex message_queue_mutex_;

    // 房间和客户端信息
    std::string room_id_;
    std::string client_id_;
    std::mutex info_mutex_;

    // 回调函数
    StateCallback state_callback_;
    MessageCallback message_callback_;
    std::mutex callback_mutex_;

    // 实例映射（用于静态回调）
    static std::unordered_map<struct lws*, WebSocketSignalingClient*> instance_map_;
    static std::mutex instance_map_mutex_;
};
