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

#include "sdkws.pb.h"

class AuthClient;
class MsgClient;
class PushClient;
class GroupClient;
class FriendClient;
class ConversationClient;

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
                       ConversationClient* conversation_client = nullptr);
    ~WSSession();

    // 启动 WebSocket 握手和消息循环（协程入口）
    boost::asio::awaitable<void> start();

    // 异步发送文本帧到客户端
    void asyncSend(std::shared_ptr<std::string> payload);

    // 获取用户账号
    const uint64_t account() const { return account_; }

private:
    friend struct WSSessionTestAccess;
    websocket::stream<boost::asio::ssl::stream<tcp::socket>> ws_;       // ssl
    uint64_t account_{0};
    bool disconnected_{false}; // 仅在本会话 executor 上访问，断线清理幂等。
    beast::flat_buffer buffer_;
    AuthClient* auth_client_;
    MsgClient* msg_client_;
    PushClient* push_client_;
    GroupClient* group_client_;
    FriendClient* friend_client_;
    ConversationClient* conversation_client_;

    // 握手后等待第一帧
    boost::asio::awaitable<void> doRead();
    boost::asio::awaitable<void> handleReg(sdkws::SdkWSReq& req);
    boost::asio::awaitable<void> handleAuth(sdkws::SdkWSReq& req);
    boost::asio::awaitable<void> fallback(sdkws::SdkWSReq& req);

    // 认证通过后进入消息循环（保持心跳 / 等待断开）
    boost::asio::awaitable<void> doReadLoop();

    // 认证通过后拉取并推送离线消息
    boost::asio::awaitable<void> pullAndPushOfflineMsgs();
    // 单一来源的顺序扫描；I/O 回调分离，便于验证分页与中断时序。
    boost::asio::awaitable<void> scanOfflineMessages(
        std::function<boost::asio::awaitable<std::vector<sdkws::MsgData>>(int64_t,int)> fetch,
        std::function<boost::asio::awaitable<void>(sdkws::MsgData)> deliver);
    boost::asio::awaitable<void> deliverOfflineMessage(sdkws::MsgData message);
    boost::asio::awaitable<void> waitOfflineBatch(
        std::chrono::steady_clock::duration timeout = std::chrono::seconds(60));

    // 处理统一ACK（type=106）：在线/离线消息确认，转发给PushService
    boost::asio::awaitable<void> handleAck(const sdkws::SdkWSReq& req);

    // 写队列：所有 WebSocket 写操作统一走此通道，避免 doReadLoop 的
    // co_await async_write 挂起期间 asyncSend 再次写导致 soft_mutex assert
    void startWriteQueue();
    boost::asio::awaitable<void> doDrainWriteQueue();
    boost::asio::awaitable<void> queuedWrite(std::shared_ptr<std::string> payload);

    // 连接断开时的清理回调
    boost::asio::awaitable<void> onDisconnect();

    struct PendingMsg {
        int64_t seq;
        std::string convID;
    };
    // serverMsgId → (seq, convID) 映射，ACK 时按会话更新 last_seq
    std::unordered_map<std::string, PendingMsg> pendingOfflineMsgs_;

    // 一次性 ACK 截止定时器：ACK 完成/断线时 cancel，无轮询。
    // 等待协程保留 shared_ptr，断线清理不能提前销毁在途操作的 timer。
    std::shared_ptr<boost::asio::steady_timer> batch_ack_timer_;

    // 写队列通道：所有 ws_.async_write 统一走此 channel，由
    // doDrainWriteQueue 协程串行消费，避免并发写导致 soft_mutex assert
    std::unique_ptr<boost::asio::experimental::channel<
        void(boost::system::error_code, std::shared_ptr<std::string>)>>
        write_queue_;
    bool write_drain_running_{false};
};

#endif
