#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include "gateway/session.h"
#include "gateway/connmanager.h"

#include <iostream>

int globcounter = 0;

Session::Session(boost::asio::ip::tcp::socket socket) : ws_(std::move(socket))
{

    // 配置 WebSocket 流
    ws_.set_option(boost::beast::websocket::stream_base::timeout::suggested(boost::beast::role_type::server));
    ws_.set_option(boost::beast::websocket::stream_base::decorator(
        [](boost::beast::websocket::response_type &res)
        {
            res.set(boost::beast::http::field::server, "IM-System-WebSocket-Server");
        }));
}

// 启动会话
boost::asio::awaitable<void> Session::run()
{
    auto self = shared_from_this();
    try
    {
        // 接受 WebSocket 握手（升级 TCP 连接为 WebSocket），注意虽然是同名函数，但是两个对象的类型是不一样的
        co_await ws_.async_accept(boost::asio::use_awaitable);
        spdlog::info("WebSocket handshake accepted");

        ws_.control_callback(
            [self](boost::beast::websocket::frame_type kind, boost::beast::string_view payload)
            {
                if (kind == boost::beast::websocket::frame_type::ping)
                {
                    boost::beast::websocket::ping_data pong_data = boost::beast::websocket::ping_data{payload};
                    boost::beast::error_code ec;
                    spdlog::info("Auto-replied pong for ping (payload: {})", payload);
                }

                boost::ignore_unused(kind, payload);
            });

        co_await do_read();
    }
    catch (const boost::system::system_error &e)
    {
        ConnManager &connmanager = ConnManager::get_instance();
        const boost::system::error_code &ec = e.code();
        // 判定为客户端主动退出
        if ((ec == boost::beast::websocket::error::closed) ||
            (ec == boost::asio::error::eof) ||
            (ec == boost::asio::error::connection_reset))
        {
            if (is_authenticated)
            {
                connmanager.rmv_conn(user.userid);
                spdlog::info("Client {} disconnected", user.userid);
            }
            else
            {
                spdlog::info("Unauthenticated client disconnected");
            }
        }
        else
        {
            if (is_authenticated)
            {
                connmanager.rmv_conn(user.userid);
            }
            spdlog::error("Session error: {}", e.what());
        }
    }
}

// 读取消息
boost::asio::awaitable<void> Session::do_read()
{
    boost::beast::flat_buffer buffer;
    while (true)
    {
        // 异步读取 WebSocket 消息
        std::size_t n = co_await ws_.async_read(buffer, boost::asio::use_awaitable);
        std::string message = boost::beast::buffers_to_string(buffer.data());
        spdlog::info("Received message: {}", message);

        // 处理消息
        process_message(message);

        // 清空缓冲区
        buffer.consume(n);
    }
}

// 处理消息（解析 JSON 并回复）
void Session::process_message(const std::string &message)
{
    try
    {
        nlohmann::json msg_json = nlohmann::json::parse(message);
        std::string type = msg_json["type"];

        // 认证前仅允许处理auth类消息
        if (!is_authenticated)
        {
            if (type != "auth")
            {
                spdlog::warn("Unauthenticated client tried to send non-auth message: {}", type);
                nlohmann::json error = {{"type", "error"}, {"code", 401}, {"message", "Not authenticated, message not processed"}};
                auto self = shared_from_this();
                std::string error_str = error.dump();
                boost::asio::co_spawn(
                    ws_.get_executor(),
                    [self, error_str]() -> boost::asio::awaitable<void>
                    {
                        co_await self->do_write(error_str);
                    },
                    boost::asio::detached);
                return;
            }
        }

        // 处理认证消息
        if (type == "auth")
        {
            // 防止重复认证
            if (is_authenticated)
            {
                spdlog::warn("User {} tried to authenticate again", user.userid);
                nlohmann::json response = {
                    {"type", "auth_response"},
                    {"code", 200},
                    {"message", "Already authenticated"}};
                auto self = shared_from_this();
                std::string response_str = response.dump();
                boost::asio::co_spawn(
                    ws_.get_executor(),
                    [self, response_str]() -> boost::asio::awaitable<void>
                    {
                        co_await self->do_write(response_str);
                    },
                    boost::asio::detached);
                return;
            }

            // 解析userid
            if (!msg_json.contains("userid"))
            {
                spdlog::warn("Auth message missing userid");
                nlohmann::json error = {
                    {"type", "auth_response"},
                    {"code", 400},
                    {"message", "Missing userid or token"}};
                auto self = shared_from_this();
                std::string error_str = error.dump();
                boost::asio::co_spawn(
                    ws_.get_executor(),
                    [self, error_str]() -> boost::asio::awaitable<void>
                    {
                        co_await self->do_write(error_str);
                    },
                    boost::asio::detached);
                return;
            }
            else
            {
                std::string userid = msg_json["userid"];
                spdlog::info("Received auth request: userid={}", userid);

                if (!validate_auth(userid))
                {
                    spdlog::warn("Auth failed");
                    nlohmann::json error = {
                        {"type", "auth_response"},
                        {"code", 403},
                        {"message", "Auth failed"}};
                    auto self = shared_from_this();
                    std::string error_str = error.dump();
                    boost::asio::co_spawn(
                        ws_.get_executor(),
                        [self, error_str]() -> boost::asio::awaitable<void>
                        {
                            co_await self->do_write(error_str);
                        },
                        boost::asio::detached);
                    return;
                }
                else
                {
                    // 认证成功
                    is_authenticated = true;
                    user.userid = userid;
                    ConnManager &connmanager = ConnManager::get_instance();
                    connmanager.add_conn(userid, shared_from_this());
                    spdlog::info("User {} authenticated successfully", userid);

                    nlohmann::json response = {
                        {"type", "auth_response"},
                        {"code", 200},
                        {"message", "Authentication successful"}};
                    auto self = shared_from_this();
                    std::string response_str = response.dump();
                    boost::asio::co_spawn(
                        ws_.get_executor(),
                        [self, response_str]() -> boost::asio::awaitable<void>
                        {
                            co_await self->do_write(response_str);
                        },
                        boost::asio::detached);
                    return;
                }
            }
        }
        else if (type == "ping")
        {
            nlohmann::json response = {{"type", "pong"}, {"timestamp", std::time(nullptr)}};
            std::string response_str = response.dump();
            auto self = shared_from_this();
            // 启动协程发送响应（不阻塞当前流程）
            boost::asio::co_spawn(
                ws_.get_executor(), // 这里与外层的ioc是同一个执行器
                [self, response_str]() -> boost::asio::awaitable<void>
                {
                    // 调用do_write，此时do_write内的self会绑定Session的生命周期
                    co_await self->do_write(response_str);
                },
                boost::asio::detached);
        }
        else if (type == "chat")
        {
            spdlog::info("message type: chat, to do ...");
            // 调用 gRPC 消息服务处理聊天消息
            // 示例：forward_to_message_service(msg_json);
        }
    }
    catch (const nlohmann::json::exception &e)
    {
        spdlog::error("JSON parse error: {}", e.what());
        // 发送错误响应
        nlohmann::json error = {{"type", "error"}, {"message", "Invalid JSON format"}};
        auto self = shared_from_this();
        std::string exception_str = error.dump();
        boost::asio::co_spawn(
            ws_.get_executor(),
            [self, exception_str]() -> boost::asio::awaitable<void>
            {
                co_await self->do_write(exception_str);
            },
            boost::asio::detached);
    }
}

// 发送消息
boost::asio::awaitable<void> Session::do_write(const std::string &message)
{
    // auto self = shared_from_this();
    try
    {
        co_await ws_.async_write(boost::asio::buffer(message), boost::asio::use_awaitable);
        spdlog::info("Sent message: {}", message);
    }
    catch (const std::exception &e)
    {
        spdlog::info("Write error: {}", e.what());
        throw; // 重新抛出，终止会话
    }
}

userInfo Session::getuser()
{
    return user;
}

bool Session::validate_auth(std::string userid)
{
    return true;
}