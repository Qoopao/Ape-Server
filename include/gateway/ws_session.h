#ifndef WS_SESSION_H
#define WS_SESSION_H

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <grpcpp/channel.h>
#include <memory>
#include <string>

class AuthClient;
class MsgClient;

namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = boost::asio::ip::tcp;

// WSSession 封装单个 WebSocket 连接
// 由 Gateway 创建，注册到 WSSessionManager 中
class WSSession : public std::enable_shared_from_this<WSSession> {
public:
    explicit WSSession(tcp::socket&& socket, AuthClient* auth_client,
                       MsgClient* msg_client);
    ~WSSession();

    // 启动 WebSocket 握手和消息循环（协程入口）
    boost::asio::awaitable<void> start();

    // 异步发送文本帧到客户端（线程安全：通过 io_context::post 投递）
    void asyncSend(std::shared_ptr<std::string> payload);

    // 获取用户 ID
    const std::string& userId() const { return userId_; }

private:
    websocket::stream<tcp::socket> ws_;
    std::string userId_;
    beast::flat_buffer buffer_;
    AuthClient* auth_client_;
    MsgClient* msg_client_;

    // 握手后等待认证帧
    boost::asio::awaitable<void> doReadAuth();

    // 认证通过后进入消息循环（保持心跳 / 等待断开）
    boost::asio::awaitable<void> doReadLoop();

    // 连接断开时的清理回调
    void onDisconnect();
};

#endif
