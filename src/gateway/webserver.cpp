#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include "gateway/webserver.h"
#include "gateway/session.h"
#include "gateway/connmanager.h"
#include "user/userinfo.h"

WebServer::WebServer(boost::asio::io_context &ioc, uint16_t port)
    : ioc_(ioc), acceptor_(ioc, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port))
{
    spdlog::info("WebSocketServer initialized, listening on port {}", port);
}

// 启动服务器
boost::asio::awaitable<void> WebServer::run()
{
    co_await do_accept(); // 开始接受连接
}

// 停止服务器
void WebServer::stop()
{
    acceptor_.close();
    spdlog::info("Closing server...");
}

// 接受 TCP 连接
boost::asio::awaitable<void> WebServer::do_accept()
{
    ConnManager& connmanager = ConnManager::get_instance();
    while (true)
    {
        try
        {
            // 异步接受 TCP 连接（co_await 等待连接建立）
            boost::asio::ip::tcp::socket socket = co_await acceptor_.async_accept(boost::asio::use_awaitable);
            spdlog::info("TCP connection accepted from {}", socket.remote_endpoint().address().to_string());

            // 创建 WebSocket 会话并启动（detached 分离线程）
            //从现象上看，co_spawn后会执行到session->run()的第一个挂起点，这个session在co_spawn后会立即销毁，所以需要在run里面调用shared_from_this()
            auto session = std::make_shared<Session>(std::move(socket));
            boost::asio::co_spawn(ioc_, session->run(), boost::asio::detached);
        }
        catch (const std::exception &e)
        {
            spdlog::info("Accept error: {}", e.what());
            // 判断acceptor是否还打开，仅在关闭时退出循环
            if (!acceptor_.is_open())
            {
                break; // acceptor已关闭，正常退出
            }
            // 否则继续接受连接（临时错误，如网络波动）
        }
    }
}
