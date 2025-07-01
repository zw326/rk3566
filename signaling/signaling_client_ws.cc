#include "signaling_client_ws.h"
#include <iostream>
#include <chrono>
#include <cstring>
#include <regex>
#include <sstream>
#include <random>

// 静态成员初始化
std::unordered_map<struct lws*, WebSocketSignalingClient*> WebSocketSignalingClient::instance_map_;
std::mutex WebSocketSignalingClient::instance_map_mutex_;

// 使用C++11 <random> 生成随机ID
static std::string GenerateRandomId(int length = 8) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    
    thread_local static std::mt19937 gen(std::random_device{}());
    thread_local static std::uniform_int_distribution<int> dist(0, sizeof(alphanum) - 2);
    
    std::string id;
    id.reserve(length);
    for (int i = 0; i < length; ++i) {
        id += alphanum[dist(gen)];
    }
    return id;
}

// 构造函数：初始化成员变量和lws协议
WebSocketSignalingClient::WebSocketSignalingClient()
    : is_connected_(false), is_connecting_(false), should_reconnect_(false),
      should_exit_(false), port_(0), reconnect_attempts_(0),
      context_(nullptr), websocket_connection_(nullptr) {
    memset(&protocols_, 0, sizeof(protocols_));
    protocols_[0].name = "webrtc-signaling";
    protocols_[0].callback = WebSocketCallback;
    protocols_[0].per_session_data_size = 0;
    protocols_[0].rx_buffer_size = 65536;
}

// 析构函数：确保资源被释放
WebSocketSignalingClient::~WebSocketSignalingClient() {
    Close();
}

bool WebSocketSignalingClient::Connect(const std::string& url) {
    if (is_connected_ || is_connecting_) {
        return false;
    }
    if (!ParseServerUrl(url)) {
        std::cerr << "Failed to parse server URL: " << url << std::endl;
        return false;
    }
    return StartWebSocketThread();
}

void WebSocketSignalingClient::Close() {
    StopWebSocketThread();

    // 清空消息队列
    {
        std::lock_guard<std::mutex> lock(message_queue_mutex_);
        std::queue<Message> empty;
        std::swap(message_queue_, empty);
    }
    
    is_connected_ = false;
    is_connecting_ = false;
    should_reconnect_ = false;
    reconnect_attempts_ = 0;
}

// 使用正则表达式解析URL
bool WebSocketSignalingClient::ParseServerUrl(const std::string& url) {
    std::regex url_regex("(wss?)://([^:/]+)(?::([0-9]+))?(/.*)?");
    std::smatch matches;
    if (!std::regex_match(url, matches, url_regex)) {
        std::cerr << "Invalid WebSocket URL: " << url << std::endl;
        return false;
    }
    scheme_ = matches[1].str();
    host_ = matches[2].str();
    port_ = matches[3].matched ? std::stoi(matches[3].str()) : (scheme_ == "wss" ? 443 : 80);
    path_ = matches[4].matched ? matches[4].str() : "/";
    return true;
}

// 启动后台线程以运行lws的事件循环，避免阻塞主线程
bool WebSocketSignalingClient::StartWebSocketThread() {
    if (websocket_thread_.joinable()) {
        return false;
    }
    
    should_exit_ = false;
    is_connecting_ = true;
    
    websocket_thread_ = std::thread([this]() {
        if (!CreateWebSocketConnection()) {
            is_connecting_ = false;
            // ... 省略回调通知 ...
            return;
        }
        
        // WebSocket事件主循环
        while (!should_exit_) {
            lws_service(context_, 100);
            
            // 处理待发送的消息队列 (优化后的逻辑)
            if (is_connected_ && websocket_connection_) {
                std::queue<Message> messages_to_send;
                {
                    std::lock_guard<std::mutex> lock(message_queue_mutex_);
                    if (!message_queue_.empty()) {
                        std::swap(messages_to_send, message_queue_);
                    }
                }
                
                while (!messages_to_send.empty() && !should_exit_) {
                    const auto& msg = messages_to_send.front();
                    
                    // 1. 构造最终的JSON对象
                    Json::Value final_json = msg.content;
                    final_json["type"] = MessageTypeToString(msg.type);
                    {
                        std::lock_guard<std::mutex> lock(info_mutex_);
                        final_json["roomId"] = room_id_;
                        if (!msg.target_id.empty()) {
                            final_json["to"] = msg.target_id;
                        }
                    }
                    
                    // 2. 序列化并发送
                    Json::StreamWriterBuilder writer;
                    writer["indentation"] = "";
                    std::string json_str = Json::writeString(writer, final_json);
                    
                    size_t len = json_str.length();
                    unsigned char* buf = new unsigned char[LWS_PRE + len];
                    memcpy(buf + LWS_PRE, json_str.c_str(), len);
                    
                    int ret = lws_write(websocket_connection_, buf + LWS_PRE, len, LWS_WRITE_TEXT);
                    delete[] buf;
                    
                    if (ret < 0) { // 发送失败则重新入队
                        std::lock_guard<std::mutex> lock(message_queue_mutex_);
                        message_queue_.push(msg);
                        break;
                    }
                    messages_to_send.pop();
                }
            }
            
            // 检查是否需要重连
            if (should_reconnect_ && !is_connected_ && !is_connecting_) {
                TryReconnect();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        // 清理lws上下文
        if (context_) {
            lws_context_destroy(context_);
            context_ = nullptr;
        }
        is_connected_ = false;
        is_connecting_ = false;
    });
    
    return true;
}

// 停止并等待后台线程结束
void WebSocketSignalingClient::StopWebSocketThread() {
    if (should_exit_) return;

    should_exit_ = true;
    if (websocket_thread_.joinable()) {
        websocket_thread_.join();
    }
}

// 创建lws上下文和连接实例
bool WebSocketSignalingClient::CreateWebSocketConnection() {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols_;
    
    context_ = lws_create_context(&info);
    if (!context_) return false;
    
    struct lws_client_connect_info connect_info;
    memset(&connect_info, 0, sizeof(connect_info));
    connect_info.context = context_;
    connect_info.address = host_.c_str();
    connect_info.port = port_;
    connect_info.path = path_.c_str();
    connect_info.host = host_.c_str();
    connect_info.origin = host_.c_str();
    connect_info.protocol = protocols_[0].name;
    connect_info.ssl_connection = (scheme_ == "wss") ? LCCSCF_USE_SSL : 0;
    connect_info.opaque_user_data = this; // 关键：将this指针传递给lws

    websocket_connection_ = lws_client_connect_via_info(&connect_info);
    if (!websocket_connection_) {
        lws_context_destroy(context_);
        context_ = nullptr;
        return false;
    }
    return true;
}

// 尝试重连，有最大次数限制
void WebSocketSignalingClient::TryReconnect() {
    if (reconnect_attempts_ >= max_reconnect_attempts_) {
        should_reconnect_ = false;
        return;
    }
    reconnect_attempts_++;
    
    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }
    
    std::this_thread::sleep_for(std::chrono::seconds(2));
    is_connecting_ = true;
    if (!CreateWebSocketConnection()) {
        is_connecting_ = false;
    }
}

// 公共接口：注册到房间
bool WebSocketSignalingClient::Register(const std::string& room_id, const std::string& client_id) {
    // {
    //     std::lock_guard<std::mutex> lock(info_mutex_);
    //     room_id_ = room_id;
    //     client_id_ = client_id.empty() ? GenerateRandomId() : client_id;
    // }
    
    // Json::Value content;
    // content["roomId"] = GetRoomId();
    // content["clientId"] = GetClientId();
    
    // return SendMessage(MessageType::REGISTER, content);
    
    // 步骤 1：无论何时调用，都先把注册信息保存下来。
    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        room_id_ = room_id;
        // 如果外部没有提供client_id，我们才生成一个随机的。
        if (!client_id.empty()) {
            client_id_ = client_id;
        } else if (client_id_.empty()) {
            client_id_ = GenerateRandomId();
        }
    }
    
    // 步骤 2：【核心修改】检查当前是否已连接。
    // 如果还没连接，就不发送任何消息，仅仅保存信息并返回。
    if (!IsConnected()) {
        std::cout << "[INFO] Client not connected yet. Registration info saved, will register automatically upon connection." << std::endl;
        return true; // 因为信息已经成功保存，所以操作是“成功”的。
    }
    
    // 步骤 3：如果已经连接（这是在连接成功后的自动回调中执行的路径），则发送注册消息。
    std::cout << "[INFO] Client is connected, sending register message now." << std::endl;
    Json::Value content;
    content["roomId"] = GetRoomId();
    content["clientId"] = GetClientId();
    
    return SendMessage(MessageType::REGISTER, content);
}

// 公共接口：发送SDP Offer
bool WebSocketSignalingClient::SendOffer(const std::string& sdp, const std::string& target_id) {
    Json::Value content;
    content["sdp"] = sdp;
    return SendMessage(MessageType::OFFER, content, target_id);
}

// 公共接口：发送SDP Answer
bool WebSocketSignalingClient::SendAnswer(const std::string& sdp, const std::string& target_id) {
    Json::Value content;
    content["sdp"] = sdp;
    return SendMessage(MessageType::ANSWER, content, target_id);
}

// 公共接口：发送ICE Candidate
bool WebSocketSignalingClient::SendCandidate(const std::string& sdp_mid, int sdp_mline_index,
                                           const std::string& candidate, const std::string& target_id) {
    Json::Value content;
    content["candidate"] = candidate;
    content["sdpMid"] = sdp_mid;
    content["sdpMLineIndex"] = sdp_mline_index;
    return SendMessage(MessageType::CANDIDATE, content, target_id);
}

// 公共接口：发送离开消息
bool WebSocketSignalingClient::SendLeave() {
    Json::Value content; // 内容为空
    return SendMessage(MessageType::LEAVE, content);
}

// 将消息放入队列，并请求lws进行一次写操作
bool WebSocketSignalingClient::SendMessage(MessageType type, const Json::Value& content, const std::string& target_id) {
    Message msg;
    msg.type = type;
    msg.content = content;
    msg.target_id = target_id;
    
    {
        std::lock_guard<std::mutex> lock(message_queue_mutex_);
        message_queue_.push(std::move(msg));
    }
    
    if (is_connected_ && websocket_connection_) {
        lws_callback_on_writable(websocket_connection_);
    }
    return true;
}

// 处理从服务器接收到的消息
void WebSocketSignalingClient::HandleReceivedMessage(const std::string& message) {
    Json::CharReaderBuilder reader;
    Json::Value json;
    std::string errors;
    std::istringstream message_stream(message);
    
    if (!Json::parseFromStream(reader, message_stream, &json, &errors)) return;
    if (!json.isMember("type") || !json["type"].isString()) return;
    
    std::string type_str = json["type"].asString();
    
    // 如果是注册成功消息，更新服务器分配的ID
    if (type_str == "register_success" && json.isMember("clientId")) {
        std::lock_guard<std::mutex> lock(info_mutex_);
        client_id_ = json["clientId"].asString();
    }

    MessageType type = StringToMessageType(type_str);
    
    // 通过回调将消息通知给上层业务逻辑
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (message_callback_) {
        message_callback_(type, message);
    }
}

// --- Getter/Setter ---
void WebSocketSignalingClient::SetStateCallback(StateCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    state_callback_ = std::move(callback);
}

void WebSocketSignalingClient::SetMessageCallback(MessageCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    message_callback_ = std::move(callback);
}

bool WebSocketSignalingClient::IsConnected() const {
    return is_connected_;
}

std::string WebSocketSignalingClient::GetRoomId() const {
    std::lock_guard<std::mutex> lock(info_mutex_);
    return room_id_;
}

std::string WebSocketSignalingClient::GetClientId() const {
    std::lock_guard<std::mutex> lock(info_mutex_);
    return client_id_;
}

// lws的静态回调函数，所有WebSocket事件的入口
int WebSocketSignalingClient::WebSocketCallback(struct lws* wsi, enum lws_callback_reasons reason,
                                              void* user, void* in, size_t len) {
    // 通过lws的用户数据获取对应的类实例指针
    WebSocketSignalingClient* instance = static_cast<WebSocketSignalingClient*>(lws_get_opaque_user_data(wsi));
    
    if (!instance) {
        return lws_callback_http_dummy(wsi, reason, user, in, len);
    }

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            instance->is_connected_ = true;
            instance->is_connecting_ = false;
            instance->reconnect_attempts_ = 0;
            // 通知上层连接成功
            {
                std::lock_guard<std::mutex> lock(instance->callback_mutex_);
                if (instance->state_callback_) instance->state_callback_(true, "Connected");
            }
            // 自动注册
            instance->Register(instance->GetRoomId(), instance->GetClientId());
            lws_callback_on_writable(wsi); // 请求发送
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        case LWS_CALLBACK_CLOSED:
            instance->is_connected_ = false;
            instance->is_connecting_ = false;
            if (!instance->should_exit_) instance->should_reconnect_ = true;
            // 通知上层连接断开
             {
                std::lock_guard<std::mutex> lock(instance->callback_mutex_);
                if (instance->state_callback_) instance->state_callback_(false, "Disconnected");
            }
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            instance->HandleReceivedMessage(std::string(static_cast<const char*>(in), len));
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            // 实际的发送逻辑在主循环中，这里仅用于触发
            break;
            
        default:
            break;
    }
    return 0;
}

// --- 类型转换辅助函数 ---
std::string WebSocketSignalingClient::MessageTypeToString(MessageType type) {
    switch (type) {
        case MessageType::REGISTER:    return "register";
        case MessageType::OFFER:       return "offer";
        case MessageType::ANSWER:      return "answer";
        case MessageType::CANDIDATE:   return "candidate";
        case MessageType::LEAVE:       return "leave";
        default:                       return "unknown";
    }
}

SignalingClient::MessageType WebSocketSignalingClient::StringToMessageType(const std::string& type_str) {
        // [新逻辑] 把所有与注册、客户端状态相关的消息都暂时归为REGISTER类型
    if (type_str == "register_success" || type_str == "client_exists" || type_str == "client_joined") {
        return MessageType::REGISTER;
    }
    if (type_str == "offer") return MessageType::OFFER;
    if (type_str == "answer") return MessageType::ANSWER;
    if (type_str == "candidate") return MessageType::CANDIDATE;
    if (type_str == "leave" || type_str == "client_left") return MessageType::LEAVE;
    return MessageType::ERROR;
}