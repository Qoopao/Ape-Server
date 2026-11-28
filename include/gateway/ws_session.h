#ifndef WS_SESSION_H
#define WS_SESSION_H

#include <boost/asio.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/beast.hpp>
#include <cstdint>
#include <grpcpp/channel.h>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>

#include "om/trace_context.h"
#include "sdkws.pb.h"

class AuthClient;
class MsgClient;
class PushClient;
class GroupClient;
class FriendClient;
class ConversationClient;
class GatewayRouter;

namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = boost::asio::ip::tcp;

// WSSession 封装单个 WebSocket 连接
// 由 Gateway 创建，注册到 WSSessionManager 中
class WSSession : public std::enable_shared_from_this<WSSession> {
public:
    explicit WSSession(boost::asio::ssl::stream<tcp::socket> &&stream, AuthClient* auth_client,
                       MsgClient* msg_client, PushClient* push_client,
                       GroupClient* group_client = nullptr, FriendClient* friend_client = nullptr,
                       ConversationClient* conversation_client = nullptr,
                       GatewayRouter* router = nullptr, std::string instance_id = {});
    ~WSSession();

    // 启动 WebSocket 握手和消息循环（协程入口）
    boost::asio::awaitable<void> start();

    // 异步发送文本帧到客户端
    void asyncSend(std::shared_ptr<std::string> payload);

    // 主动踢下线：尽力发送 KICKED 帧后断开（跨实例互踢 / 同实例重复登录）
    void kick(const std::string &reason = "duplicate_login");

    // 网关下线：发送 SERVER_DRAINING，客户端应立即重连到其他实例
    void notifyDraining();

    // 立刻关闭连接（grace 期满后的强制关闭）
    void closeNow();

    // 本连接的唯一标识，路由表 CAS 与互踢比对用（4.7.1）
    const std::string &sessionId() const { return session_id_; }

    // 获取用户账号
    const uint64_t account() const { return account_; }

private:
    friend struct WSSessionTestAccess;
    websocket::stream<boost::asio::ssl::stream<tcp::socket>> ws_;       // ssl
    std::string session_id_;
    uint64_t account_{0};
    bool disconnected_{false}; // 仅在本会话 executor 上访问，断线清理幂等。
    beast::flat_buffer buffer_;
    AuthClient* auth_client_;
    MsgClient* msg_client_;
    PushClient* push_client_;
    GroupClient* group_client_;
    FriendClient* friend_client_;
    ConversationClient* conversation_client_;

    // 路由与实例身份（多网关改造 4.2 / 4.3 / 4.7）
    GatewayRouter* router_{nullptr};
    std::string instance_id_;
    bool route_owned_{false};
    bool draining_{false};

    // 握手后等待第一帧
    boost::asio::awaitable<bool> doRead();  // 返回 true = 连接已转为已认证长连接，交由 doReadLoop 接管；返回 false = 本连接使命结束，由调用方清理。
    boost::asio::awaitable<void> handleReg(sdkws::SdkWSReq& req);
    boost::asio::awaitable<void> handleAuth(sdkws::SdkWSReq& req);
    boost::asio::awaitable<void> fallback(sdkws::SdkWSReq& req);

    // 认证通过后进入消息循环（保持心跳 / 等待断开）
    boost::asio::awaitable<void> doReadLoop();

    // 认证通过后同步离线消息（PR-8：按会话有界快照）
    boost::asio::awaitable<void> syncOffline(const sdkws::SdkWSReq& req);
    // 群同步（210）：209 之后发 251 内联（小群）并以 210 收尾；大群只写 needsPull。
    boost::asio::awaitable<void> syncGroups(const sdkws::SdkWSReq& req);
    // 路由续期（CAS）：返回 false 表示已失去所有权，调用方应断开
    boost::asio::awaitable<bool> renewRoute();
    std::string routeValue() const { return instance_id_ + ":" + session_id_; }

    // 处理统一ACK（type=106）：在线/离线消息确认，转发给PushService
    boost::asio::awaitable<void> handleAck(const sdkws::SdkWSReq& req,
                                           const ape::otel::TraceContext& trace);

    // 写队列：所有 WebSocket 写操作统一走此通道，避免 doReadLoop 的
    // co_await async_write 挂起期间 asyncSend 再次写导致 soft_mutex assert
    void startWriteQueue();
    boost::asio::awaitable<void> doDrainWriteQueue();
    boost::asio::awaitable<void> queuedWrite(std::shared_ptr<std::string> payload);
    // 轮间水位控制：等待写队列回落到 threshold 以下（有超时）
    boost::asio::awaitable<void> waitWriteQueueBelow(
        std::size_t threshold, std::chrono::milliseconds timeout);

    // 连接断开时的清理回调
    boost::asio::awaitable<void> onDisconnect();

    // 写队列通道：所有 ws_.async_write 统一走此 channel，由
    // doDrainWriteQueue 协程串行消费，避免并发写导致 soft_mutex assert
    std::unique_ptr<boost::asio::experimental::channel<
        void(boost::system::error_code, std::shared_ptr<std::string>)>>
        write_queue_;
    bool write_drain_running_{false};
    // 仅在本会话 executor 上访问：用于登录同步的轮间水位控制
    std::size_t write_queue_depth_{0};
};

#endif
