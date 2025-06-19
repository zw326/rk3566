#include "signaling_client_ws.h"
#include <iostream>
#include <chrono>
#include <cstring>
#include <regex>
#include <sstream>

// 静态成员初始化
std::unordered_map<struct lws*, WebSocketSignalingClient*> WebSocketSignalingClient::instance_map_;
std::mutex WebSocketSignalingClient::instance_map_mutex_;

// 辅助函数：获取当前时间戳（毫秒）
static int64_t GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 辅助函数：生成随机ID
static std::string GenerateRandomId(int length = 8) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    std::string id;
    id.reserve(length);
    
    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    for (int i = 0; i < length; ++i) {
        id += alphanum[std::rand() % (sizeof(alphanum) - 1)];
    }
    return id;
}

WebSocketSignalingClient::WebSocketSignalingClient()
    : is_connected_(false)
    , is_connecting_(false)
    , should_reconnect_(false)
    , should_exit_(false)
    , port_(0)
    , reconnect_attempts_(0)
    , context_(nullptr)
    , websocket_connection_(nullptr) {
    
    // 初始化libwebsockets协议
    memset(&protocols_, 0, sizeof(protocols_));
    protocols_[0].name = "webrtc-signaling";
    protocols_[0].callback = WebSocketCallback;
    protocols_[0].per_session_data_size = 0;
    protocols_[0].rx_buffer_size = 65536;
}

WebSocketSignalingClient::~WebSocketSignalingClient() {
    Close();
}

bool WebSocketSignalingClient::Connect(const std::string& url) {
    if (is_connected_ || is_connecting_) {
        std::cout << "Already connected or connecting" << std::endl;
        return false;
    }
    
    // 解析URL
    if (!ParseServerUrl(url)) {
        std::cerr << "Failed to parse server URL: " << url << std::endl;
        return false;
    }
    
    // 启动WebSocket线程
    return StartWebSocketThread();
}

bool WebSocketSignalingClient::ParseServerUrl(const std::string& url) {
    // 使用正则表达式解析URL
    std::regex url_regex("(wss?)://([^:/]+)(?::([0-9]+))?(/.*)?");
    std::smatch matches;
    
    if (!std::regex_match(url, matches, url_regex)) {
        std::cerr << "Invalid WebSocket URL: " << url << std::endl;
        return false;
    }
    
    // 提取URL组件
    scheme_ = matches[1].str();
    host_ = matches[2].str();
    
    // 解析端口
    if (matches[3].matched) {
        port_ = std::stoi(matches[3].str());
    } else {
        // 默认端口
        port_ = (scheme_ == "wss") ? 443 : 80;
    }
    
    // 解析路径
    if (matches[4].matched) {
        path_ = matches[4].str();
    } else {
        path_ = "/";
    }
    
    return true;
}


bool WebSocketSignalingClient::StartWebSocketThread() {
    if (websocket_thread_.joinable()) {
        std::cerr << "WebSocket thread already running" << std::endl;
        return false;
    }
    
    should_exit_ = false;
    is_connecting_ = true;
    
    // 创建WebSocket线程
    websocket_thread_ = std::thread([this]() {
        // 创建WebSocket连接
        if (!CreateWebSocketConnection()) {
            std::cerr << "Failed to create WebSocket connection" << std::endl;
            is_connecting_ = false;
            
            // 通知连接失败
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (state_callback_) {
                state_callback_(false, "Failed to create WebSocket connection");
            }
            return;
        }
        
        // 主循环
        while (!should_exit_) {
            // 处理WebSocket事件
            lws_service(context_, 100);
            
            // 处理消息队列
            if (is_connected_ && websocket_connection_) {
                std::queue<Message> messages;
                {
                    std::lock_guard<std::mutex> lock(message_queue_mutex_);
                    std::swap(messages, message_queue_);
                }
                
                while (!messages.empty() && !should_exit_) {
                    const auto& msg = messages.front();
                    
                    // 构建JSON消息
                    Json::Value json_msg;
                    json_msg["type"] = MessageTypeToString(msg.type);
                    
                    // 解析内容
                    Json::CharReaderBuilder reader;
                    Json::Value content_json;
                    std::string errors;
                    std::istringstream content_stream(msg.content);
                    if (Json::parseFromStream(reader, content_stream, &content_json, &errors)) {
                        // 合并内容到消息
                        for (const auto& key : content_json.getMemberNames()) {
                            json_msg[key] = content_json[key];
                        }
                    }
                    
                    // 添加房间和客户端信息
                    {
                        std::lock_guard<std::mutex> lock(info_mutex_);
                        json_msg["roomId"] = room_id_;
                        json_msg["from"] = client_id_;
                        if (!msg.target_id.empty()) {
                            json_msg["to"] = msg.target_id;
                        }
                    }
                    
                    // 序列化消息
                    Json::StreamWriterBuilder writer;
                    std::string json_str = Json::writeString(writer, json_msg);
                    
                    // 准备发送缓冲区（LWS_PRE是libwebsockets要求的前缀空间）
                    size_t len = json_str.length();
                    unsigned char* buf = new unsigned char[LWS_PRE + len];
                    memcpy(buf + LWS_PRE, json_str.c_str(), len);
                    
                    // 发送消息
                    int ret = lws_write(websocket_connection_, buf + LWS_PRE, len, LWS_WRITE_TEXT);
                    delete[] buf;
                    
                    if (ret < 0) {
                        std::cerr << "Error writing to WebSocket" << std::endl;
                        
                        // 如果发送失败，重新加入队列
                        std::lock_guard<std::mutex> lock(message_queue_mutex_);
                        message_queue_.push(msg);
                        break;
                    }
                    
                    messages.pop();
                }
            }
            
            // 检查是否需要重连
            if (should_reconnect_ && !is_connected_ && !is_connecting_) {
                TryReconnect();
            }
            
            // 避免CPU占用过高
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        // 清理WebSocket连接
        if (websocket_connection_) {
            // 从映射中移除
            {
                std::lock_guard<std::mutex> lock(instance_map_mutex_);
                instance_map_.erase(websocket_connection_);
            }
            
            websocket_connection_ = nullptr;
        }
        
        // 销毁WebSocket上下文
        if (context_) {
            lws_context_destroy(context_);
            context_ = nullptr;
        }
        
        is_connected_ = false;
        is_connecting_ = false;
    });
    
    return true;
}

void WebSocketSignalingClient::TryReconnect() {
    if (reconnect_attempts_ >= max_reconnect_attempts_) {
        std::cerr << "Max reconnect attempts reached" << std::endl;
        should_reconnect_ = false;
        return;
    }
    
    reconnect_attempts_++;
    std::cout << "Attempting to reconnect (" << reconnect_attempts_ << "/" << max_reconnect_attempts_ << ")" << std::endl;
    
    // 清理旧连接
    if (websocket_connection_) {
        // 从映射中移除
        {
            std::lock_guard<std::mutex> lock(instance_map_mutex_);
            instance_map_.erase(websocket_connection_);
        }
        
        websocket_connection_ = nullptr;
    }
    
    // 销毁WebSocket上下文
    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }
    
    // 创建新连接
    is_connecting_ = true;
    if (!CreateWebSocketConnection()) {
        std::cerr << "Failed to reconnect" << std::endl;
        is_connecting_ = false;
        
        // 延迟后再次尝试
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

bool WebSocketSignalingClient::CreateWebSocketConnection() {
    // 创建WebSocket上下文
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols_;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    
    context_ = lws_create_context(&info);
    if (!context_) {
        std::cerr << "Failed to create WebSocket context" << std::endl;
        return false;
    }
    
    // 创建WebSocket连接
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
    
    websocket_connection_ = lws_client_connect_via_info(&connect_info);
    if (!websocket_connection_) {
        std::cerr << "Failed to connect to WebSocket server" << std::endl;
        lws_context_destroy(context_);
        context_ = nullptr;
        return false;
    }
    
    // 将实例添加到映射
    {
        std::lock_guard<std::mutex> lock(instance_map_mutex_);
        instance_map_[websocket_connection_] = this;
    }
    
    return true;
}

void WebSocketSignalingClient::Close() {
    StopWebSocketThread();
    
    // 清空消息队列
    {
        std::lock_guard<std::mutex> lock(message_queue_mutex_);
        std::queue<Message> empty;
        std::swap(message_queue_, empty);
    }
    
    // 重置状态
    is_connected_ = false;
    is_connecting_ = false;
    should_reconnect_ = false;
    reconnect_attempts_ = 0;
}

void WebSocketSignalingClient::StopWebSocketThread() {
    should_exit_ = true;
    
    if (websocket_thread_.joinable()) {
        websocket_thread_.join();
    }
}




bool WebSocketSignalingClient::Register(const std::string& room_id, const std::string& client_id) {
    // 保存房间和客户端信息
    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        room_id_ = room_id;
        client_id_ = client_id.empty() ? GenerateRandomId() : client_id;
    }
    
    // 构建注册消息
    Json::Value msg;
    msg["type"] = "register";
    msg["roomId"] = room_id;
    msg["clientId"] = client_id_;
    
    Json::StreamWriterBuilder writer;
    std::string content = Json::writeString(writer, msg);
    
    return SendMessage(MessageType::REGISTER, content);
}

bool WebSocketSignalingClient::SendOffer(const std::string& sdp, const std::string& target_id) {
    // 构建Offer消息
    Json::Value msg;
    msg["type"] = "offer";
    msg["sdp"] = sdp;
    
    Json::StreamWriterBuilder writer;
    std::string content = Json::writeString(writer, msg);
    
    return SendMessage(MessageType::OFFER, content, target_id);
}

bool WebSocketSignalingClient::SendAnswer(const std::string& sdp, const std::string& target_id) {
    // 构建Answer消息
    Json::Value msg;
    msg["type"] = "answer";
    msg["sdp"] = sdp;
    
    Json::StreamWriterBuilder writer;
    std::string content = Json::writeString(writer, msg);
    
    return SendMessage(MessageType::ANSWER, content, target_id);
}

bool WebSocketSignalingClient::SendCandidate(const std::string& sdp_mid, int sdp_mline_index,
                                           const std::string& candidate, const std::string& target_id) {
    // 构建Candidate消息
    Json::Value msg;
    msg["type"] = "candidate";
    msg["sdpMid"] = sdp_mid;
    msg["sdpMLineIndex"] = sdp_mline_index;
    msg["candidate"] = candidate;
    
    Json::StreamWriterBuilder writer;
    std::string content = Json::writeString(writer, msg);
    
    return SendMessage(MessageType::CANDIDATE, content, target_id);
}

bool WebSocketSignalingClient::SendLeave() {
    // 构建Leave消息
    Json::Value msg;
    msg["type"] = "leave";
    
    Json::StreamWriterBuilder writer;
    std::string content = Json::writeString(writer, msg);
    
    return SendMessage(MessageType::LEAVE, content);
}

bool WebSocketSignalingClient::SendMessage(MessageType type, const std::string& content, const std::string& target_id) {
    // 创建消息
    Message msg;
    msg.type = type;
    msg.content = content;
    msg.target_id = target_id;
    
    // 添加到消息队列
    {
        std::lock_guard<std::mutex> lock(message_queue_mutex_);
        message_queue_.push(std::move(msg));
    }
    
    // 触发WebSocket写事件
    if (is_connected_ && websocket_connection_) {
        lws_callback_on_writable(websocket_connection_);
    }
    
    return true;
}




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


void WebSocketSignalingClient::HandleReceivedMessage(const std::string& message) {
    // 解析JSON消息
    Json::CharReaderBuilder reader;
    Json::Value json;
    std::string errors;
    std::istringstream message_stream(message);
    
    if (!Json::parseFromStream(reader, message_stream, &json, &errors)) {
        std::cerr << "Failed to parse JSON message: " << errors << std::endl;
        return;
    }
    
    // 检查消息类型
    if (!json.isMember("type") || !json["type"].isString()) {
        std::cerr << "Invalid message format: missing type" << std::endl;
        return;
    }
    
    // 获取消息类型
    std::string type_str = json["type"].asString();
    MessageType type = StringToMessageType(type_str);
    
    // 处理特殊消息类型
    if (type == MessageType::ERROR) {
        std::string error_msg = json.isMember("message") ? json["message"].asString() : "Unknown error";
        std::cerr << "Received error message: " << error_msg << std::endl;
    }
    
    // 通知消息回调
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (message_callback_) {
        message_callback_(type, message);
    }
}



int WebSocketSignalingClient::WebSocketCallback(struct lws* wsi, enum lws_callback_reasons reason,
                                              void* user, void* in, size_t len) {
    // 获取实例
    WebSocketSignalingClient* instance = nullptr;
    {
        std::lock_guard<std::mutex> lock(instance_map_mutex_);
        auto it = instance_map_.find(wsi);
        if (it != instance_map_.end()) {
            instance = it->second;
        }
    }
    
    // 处理连接建立回调（可能还没有实例映射）
    if (reason == LWS_CALLBACK_CLIENT_ESTABLISHED) {
        std::lock_guard<std::mutex> lock(instance_map_mutex_);
        auto it = instance_map_.find(wsi);
        if (it != instance_map_.end()) {
            instance = it->second;
            instance->is_connected_ = true;
            instance->is_connecting_ = false;
            instance->reconnect_attempts_ = 0;
            
            // 通知连接状态
            std::lock_guard<std::mutex> callback_lock(instance->callback_mutex_);
            if (instance->state_callback_) {
                instance->state_callback_(true, "Connected to signaling server");
            }
            
            // 如果有房间ID，自动注册
            std::string room_id, client_id;
            {
                std::lock_guard<std::mutex> info_lock(instance->info_mutex_);
                room_id = instance->room_id_;
                client_id = instance->client_id_;
            }
            
            if (!room_id.empty()) {
                instance->Register(room_id, client_id);
            }
        }
        return 0;
    }
    
    // 其他回调需要有效的实例
    if (!instance) {
        return 0;
    }
    
    switch (reason) {
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
            const char* error_msg = in ? static_cast<const char*>(in) : "Unknown error";
            std::cerr << "WebSocket connection error: " << error_msg << std::endl;
            
            instance->is_connected_ = false;
            instance->is_connecting_ = false;
            instance->should_reconnect_ = true;
            
            // 通知连接状态
            std::lock_guard<std::mutex> lock(instance->callback_mutex_);
            if (instance->state_callback_) {
                instance->state_callback_(false, std::string("Connection error: ") + error_msg);
            }
            break;
        }
        
        case LWS_CALLBACK_CLIENT_RECEIVE: {
            // 接收消息
            const char* message = static_cast<const char*>(in);
            instance->HandleReceivedMessage(std::string(message, len));
            break;
        }
        
        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            // 可以发送数据
            // 实际发送在主线程中处理
            break;
        }
        
        case LWS_CALLBACK_CLOSED: {
            std::cout << "WebSocket connection closed" << std::endl;
            
            instance->is_connected_ = false;
            instance->is_connecting_ = false;
            instance->should_reconnect_ = true;
            
            // 通知连接状态
            std::lock_guard<std::mutex> lock(instance->callback_mutex_);
            if (instance->state_callback_) {
                instance->state_callback_(false, "Connection closed");
            }
            break;
        }
        
        default:
            break;
    }
    
    return 0;
}

std::string WebSocketSignalingClient::MessageTypeToString(MessageType type) {
    switch (type) {
        case MessageType::REGISTER:
            return "register";
        case MessageType::OFFER:
            return "offer";
        case MessageType::ANSWER:
            return "answer";
        case MessageType::CANDIDATE:
            return "candidate";
        case MessageType::LEAVE:
            return "leave";
        case MessageType::ERROR:
            return "error";
        default:
            return "unknown";
    }
}

SignalingClient::MessageType WebSocketSignalingClient::StringToMessageType(const std::string& type_str) {
    if (type_str == "register") {
        return MessageType::REGISTER;
    } else if (type_str == "offer") {
        return MessageType::OFFER;
    } else if (type_str == "answer") {
        return MessageType::ANSWER;
    } else if (type_str == "candidate") {
        return MessageType::CANDIDATE;
    } else if (type_str == "leave") {
        return MessageType::LEAVE;
    } else if (type_str == "error") {
        return MessageType::ERROR;
    } else {
        return MessageType::ERROR;
    }
}
