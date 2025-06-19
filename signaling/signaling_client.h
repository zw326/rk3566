#pragma once
#include <functional>
#include <string>
#include <memory>

/**
 * @brief 信令客户端接口类
 * 
 * 该类定义了信令客户端的通用接口，用于与信令服务器通信
 * 支持SDP交换、ICE候选收集与处理、房间管理和客户端状态通知
 */
class SignalingClient {
public:
    // 消息类型枚举
    enum class MessageType {
        REGISTER,    // 注册到房间
        OFFER,       // SDP Offer
        ANSWER,      // SDP Answer
        CANDIDATE,   // ICE Candidate
        LEAVE,       // 离开房间
        ERROR        // 错误消息
    };

    /**
     * @brief 信令状态回调函数类型
     * @param connected 是否已连接
     * @param message 状态描述
     */
    using StateCallback = std::function<void(bool connected, const std::string& message)>;

    /**
     * @brief 消息回调函数类型
     * @param type 消息类型
     * @param message 消息内容
     */
    using MessageCallback = std::function<void(MessageType type, const std::string& message)>;

    //析构函数
    virtual ~SignalingClient() = default;

    /**
     * @brief 连接到信令服务器
     * @param url 服务器URL
     * @return 是否成功发起连接
     */
    virtual bool Connect(const std::string& url) = 0;

    //关闭连接
    virtual void Close() = 0;

    /**
     * @brief 注册到房间
     * @param room_id 房间ID
     * @param client_id 客户端ID（可选）
     * @return 是否成功发送注册消息
     */
    virtual bool Register(const std::string& room_id, const std::string& client_id = "") = 0;

    /**
     * @brief 发送SDP Offer
     * @param sdp SDP字符串
     * @param target_id 目标客户端ID（可选）
     * @return 是否成功发送
     */
    virtual bool SendOffer(const std::string& sdp, const std::string& target_id = "") = 0;

    /**
     * @brief 发送SDP Answer
     * @param sdp SDP字符串
     * @param target_id 目标客户端ID（可选）
     * @return 是否成功发送
     */
    virtual bool SendAnswer(const std::string& sdp, const std::string& target_id = "") = 0;

    /**
     * @brief 发送ICE候选
     * @param sdp_mid SDP媒体标识符
     * @param sdp_mline_index SDP媒体行索引
     * @param candidate 候选字符串
     * @param target_id 目标客户端ID（可选）
     * @return 是否成功发送
     */
    virtual bool SendCandidate(const std::string& sdp_mid, int sdp_mline_index,
                              const std::string& candidate, const std::string& target_id = "") = 0;

    /**
     * @brief 发送离开房间消息
     * @return 是否成功发送
     */
    virtual bool SendLeave() = 0;

    /**
     * @brief 设置状态回调
     * @param callback 回调函数
     */
    virtual void SetStateCallback(StateCallback callback) = 0;

    /**
     * @brief 设置消息回调
     * @param callback 回调函数
     */
    virtual void SetMessageCallback(MessageCallback callback) = 0;

    /**
     * @brief 是否已连接
     * @return 连接状态
     */
    virtual bool IsConnected() const = 0;

    /**
     * @brief 获取当前房间ID
     * @return 房间ID
     */
    virtual std::string GetRoomId() const = 0;

    /**
     * @brief 获取当前客户端ID
     * @return 客户端ID
     */
    virtual std::string GetClientId() const = 0;
};
