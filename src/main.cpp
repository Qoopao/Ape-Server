#include <boost/asio.hpp>
#include <spdlog/spdlog.h>
#include "gateway/webserver.h"
#include "util/mongoconnector.h"

#include <iostream>

int main()
{
    // 启动WebService
    try
    {
        // 1. 创建 io_context（事件循环核心）
        boost::asio::io_context ioc(1); // 1 个线程

        // 2. 监听 SIGINT 和 SIGTERM 信号
        // 当收到这些信号时，io_context 会停止运行
        boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { ioc.stop(); });
        
        // 2. 创建 WebServer 实例
        WebServer server(ioc, 6666);

        // 3. 用 co_spawn 启动服务器协程（提交到 io_context）
        boost::asio::co_spawn(
            ioc,
            server.listener(),         // 启动服务器的 listener() 协程
            boost::asio::detached // 协程独立运行，不阻塞 main
        );

        // 4. 运行 io_context（阻塞，直到所有事件处理完成或被停止）
        // 这一步是关键：没有它，io_context 不会处理异步事件，程序直接退出
        ioc.run(); // 注意嵌套协程的挂起恢复顺序
    }
    catch (const std::exception &e)
    {
        spdlog::error("Main error: {}", e.what());
    }
    return 0;
}