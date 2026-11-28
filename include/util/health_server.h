#ifndef APE_HEALTH_SERVER_H
#define APE_HEALTH_SERVER_H

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace ape {
namespace ops {

// 极简 HTTP 健康检查：GET /healthz（存活）与 GET /readyz（就绪）。
// 端口为 0 时不启动（默认关闭，由 --health-port / HEALTH_PORT 显式开启）。
class HealthServer
{
public:
    using Readiness = std::function<bool()>;

    HealthServer() = default;
    ~HealthServer();

    HealthServer(const HealthServer &) = delete;
    HealthServer &operator=(const HealthServer &) = delete;

    // 同步完成 bind/listen，失败返回 false；成功后后台线程 accept。
    bool Start(uint16_t port, Readiness readiness);
    void Stop();
    uint16_t port() const { return port_; }

private:
    void Run();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::unique_ptr<boost::asio::io_context> ioc_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    Readiness readiness_;
    uint16_t port_ = 0;
};

} // namespace ops
} // namespace ape

#endif
