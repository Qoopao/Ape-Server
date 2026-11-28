#include "util/health_server.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <utility>

namespace ape {
namespace ops {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

http::response<http::string_body> JsonResponse(http::status status, std::string body)
{
    http::response<http::string_body> response{status, 11};
    response.set(http::field::content_type, "application/json");
    response.set(http::field::cache_control, "no-store");
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

} // namespace

HealthServer::~HealthServer()
{
    Stop();
}

bool HealthServer::Start(uint16_t port, Readiness readiness)
{
    if (port == 0 || running_.load())
        return false;

    port_ = port;
    readiness_ = std::move(readiness);
    ioc_ = std::make_unique<net::io_context>();
    acceptor_ = std::make_unique<tcp::acceptor>(*ioc_);

    boost::system::error_code ec;
    const tcp::endpoint endpoint{tcp::v4(), port_};
    acceptor_->open(endpoint.protocol(), ec);
    if (ec)
        return false;
    acceptor_->set_option(net::socket_base::reuse_address(true), ec);
    if (ec)
        return false;
    acceptor_->bind(endpoint, ec);
    if (ec)
        return false;
    acceptor_->listen(net::socket_base::max_listen_connections, ec);
    if (ec)
        return false;

    running_.store(true);
    thread_ = std::thread([this]() { Run(); });
    return true;
}

void HealthServer::Stop()
{
    if (!running_.exchange(false))
        return;
    if (acceptor_)
    {
        boost::system::error_code ignored;
        acceptor_->close(ignored);
    }
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
        thread_.join();
}

void HealthServer::Run()
{
    // 阻塞 accept 无法被其他线程 close 可靠唤醒；用非阻塞 + 短睡眠，Stop 最多等 50ms。
    boost::system::error_code ec;
    acceptor_->non_blocking(true, ec);
    while (running_.load())
    {
        tcp::socket socket(*ioc_);
        acceptor_->accept(socket, ec);
        if (ec)
        {
            if (ec == net::error::would_block || ec == net::error::try_again)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (!running_.load())
                break;
            continue;
        }
        socket.non_blocking(false, ec);
        beast::tcp_stream stream(std::move(socket));
        stream.expires_after(std::chrono::seconds(2));

        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        http::read(stream, buffer, request, ec);
        if (ec)
        {
            stream.socket().shutdown(tcp::socket::shutdown_both, ec);
            continue;
        }

        http::response<http::string_body> response =
            JsonResponse(http::status::not_found, R"({"status":"not_found"})");
        if (request.method() == http::verb::get)
        {
            if (request.target() == "/healthz" || request.target() == "/health")
            {
                response = JsonResponse(http::status::ok, R"({"status":"ok"})");
            }
            else if (request.target() == "/readyz" || request.target() == "/ready")
            {
                const bool ready = readiness_ ? readiness_() : true;
                response = JsonResponse(ready ? http::status::ok
                                              : http::status::service_unavailable,
                                        ready ? R"({"status":"ready"})"
                                              : R"({"status":"not_ready"})");
            }
        }
        response.keep_alive(false);
        http::write(stream, response, ec);
        stream.socket().shutdown(tcp::socket::shutdown_send, ec);
    }
    running_.store(false);
}

} // namespace ops
} // namespace ape
