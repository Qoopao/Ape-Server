#include <boost/asio.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast.hpp>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <boost/asio/this_coro.hpp> // 确保包含this_coro头文件

// 简化命名空间，提升代码可读性
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::use_awaitable;
namespace this_coro = boost::asio::this_coro;

#include "gateway/webserver.h"
#include "user/userinfo.h"

WebServer::WebServer(asio::io_context &ioc, uint16_t port)
    : ioc_(ioc), acceptor_(ioc, tcp::endpoint(tcp::v4(), port))
{
    spdlog::info("WebSocketServer initialized, listening on port {}", port);
}

// 停止服务器（优化：处理error_code，避免抛异常）
void WebServer::stop()
{
    acceptor_.close();
}

// 读写回显逻辑（核心修正：精细化错误处理）
awaitable<void> read_and_write_back(tcp::socket socket)
{
    // 保存客户端地址，用于日志打印
    std::string client_addr = socket.remote_endpoint().address().to_string();
    uint16_t client_port = socket.remote_endpoint().port();
    spdlog::debug("New connection from {}:{}", client_addr, client_port);

    try
    {
        char data[1024];
        for (;;)
        {
            // 方式1：用error_code替代异常捕获（推荐，更精细）
            boost::system::error_code ec;
            std::size_t n = co_await socket.async_read_some(asio::buffer(data), 
                asio::redirect_error(use_awaitable, ec));

            // 处理读取结果
            if (ec) {
                if (ec == asio::error::eof || ec == asio::error::connection_reset) {
                    // 正常的连接关闭，打印info级别日志
                    spdlog::info("Client {}:{} closed connection (EOF/Reset)", client_addr, client_port);
                } else {
                    // 真正的网络错误，打印error级别日志
                    spdlog::error("Read error from {}:{}: {}", client_addr, client_port, ec.message());
                }
                break; // 退出循环，释放socket
            }

            // 回写数据（同样处理写错误）
            ec.clear();
            co_await asio::async_write(socket, asio::buffer(data, n),
                asio::redirect_error(use_awaitable, ec));
            if (ec) {
                spdlog::error("Write error to {}:{}: {}", client_addr, client_port, ec.message());
                break;
            }
            spdlog::debug("Echoed {} bytes to {}:{}", n, client_addr, client_port);
        }
    }
    catch (const std::exception& e)
    {
        // 捕获未预期的异常（如内存错误等）
        spdlog::error("Unexpected error in read_and_write_back ({}:{}): {}", 
            client_addr, client_port, e.what());
    }
    spdlog::debug("Connection with {}:{} closed completely", client_addr, client_port);
}

// 监听逻辑（优化：简化executor，增强可读性）
asio::awaitable<void> WebServer::listener()
{

    for (;;)
    {
        try {
            // 异步接受连接
            tcp::socket socket = co_await acceptor_.async_accept(use_awaitable);
            // 启动协程处理连接（用ioc_作为executor，更直观）
            co_spawn(ioc_, read_and_write_back(std::move(socket)), detached);
        }
        catch (const std::exception& e)
        {
            spdlog::warn("Accept error: {}", e.what());
            // 仅当acceptor关闭时退出循环
            if (!acceptor_.is_open()) {
                spdlog::info("Acceptor closed, exiting listener loop");
                break;
            }
            // 临时错误（如网络波动），继续接受连接
        }
    }
    co_return; // 显式返回，符合协程规范
}

