#ifndef WS_SESSION_H
#define WS_SESSION_H

#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/beast.hpp>
#include <grpcpp/channel.h>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

#include "sdkws.pb.h"

class AuthClient;
class MsgClient;
class PushClient;

namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = boost::asio::ip::tcp;

// WSSession 封装单个 WebSocket 连接
// 由 Gateway 创建，注册到 WSSessionManager 中
class WSSession : public std::enable_shared_from_this<WSSession> {
public:
    explicit WSSession(tcp::socket&& socket, AuthClient* auth_client,
                       MsgClient* msg_client, PushClient* push_client);
    ~WSSession();

    // 启动 WebSocket 握手和消息循环（协程入口）
    boost::asio::awaitable<void> start();

    // 异步发送文本帧到客户端
    void asyncSend(std::shared_ptr<std::string> payload);

    // 获取用户 ID
    const std::string& userId() const { return userId_; }

private:
    websocket::stream<tcp::socket> ws_;
    std::string userId_;
    beast::flat_buffer buffer_;
    AuthClient* auth_client_;
    MsgClient* msg_client_;
    PushClient* push_client_;

    // 握手后等待认证帧
    boost::asio::awaitable<void> doReadAuth();

    // 认证通过后进入消息循环（保持心跳 / 等待断开）
    boost::asio::awaitable<void> doReadLoop();

    // 认证通过后拉取并推送离线消息
    boost::asio::awaitable<void> pullAndPushOfflineMsgs();

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

    // 协程间通知通道：handleAck 清空本批次 pendingOfflineMsgs_ 后
    // 通过此 channel 唤醒 pullAndPushOfflineMsgs，替代定时器忙等轮询
    // capacity=0 保证 try_send 必须等 async_receive 就绪，形成 rendezvous
    std::unique_ptr<
        boost::asio::experimental::channel<void(boost::system::error_code)>>
        batch_ack_signal_;

    // 写队列通道：所有 ws_.async_write 统一走此 channel，由
    // doDrainWriteQueue 协程串行消费，避免并发写导致 soft_mutex assert
    std::unique_ptr<boost::asio::experimental::channel<
        void(boost::system::error_code, std::shared_ptr<std::string>)>>
        write_queue_;
    bool write_drain_running_{false};
};

#endif
