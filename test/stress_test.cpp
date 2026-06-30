// IM 系统压测工具
//
// 用法:
//   stress_test --clients 100 --rate 5 --duration 60 --host localhost:6666 --auth localhost:50051
//
// 参数:
//   --clients  N    并发连接数（默认 100，必须为偶数）
//   --rate     N    每个连接每秒发送消息数（默认 5）
//   --duration N    压测持续秒数（默认 60）
//   --host     HOST  Gateway WebSocket 地址（默认 localhost:6666）
//   --auth     HOST  AuthService gRPC 地址（默认 localhost:50051）

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/stream.hpp>

#include <grpcpp/grpcpp.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>

#include "apeauth.pb.h"
#include "apeauth.grpc.pb.h"
#include "sdkws.pb.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/fmt/fmt.h>    // fmt::format

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

// ============================================================================
// 配置
// ============================================================================
struct Config {
    int clients = 100;
    int rate = 5;           // 每秒每连接消息数
    int duration = 60;      // 秒
    std::string gateway_host = "localhost";
    std::string gateway_port = "6666";
    std::string auth_host = "localhost:50051";
};

Config g_config;

// ============================================================================
// 全局统计
// ============================================================================
struct Stats {
    std::atomic<int64_t> messages_sent{0};
    std::atomic<int64_t> messages_received{0};
    std::atomic<int64_t> send_errors{0};
    std::atomic<int64_t> recv_errors{0};
    std::atomic<int64_t> connections_ok{0};
    std::atomic<int64_t> connections_failed{0};

    // 延迟采样（单线程写入，多线程读取没问题）
    std::mutex latency_mtx;
    std::vector<double> latencies_ms;
};

Stats g_stats;

// ============================================================================
// 全局延迟记录（跨客户端匹配 clientMsgID → send_time）
// ============================================================================
std::mutex g_pending_mtx;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_pending_map;

// ============================================================================
// 单个压测客户端（协程）
// ============================================================================
class StressClient : public std::enable_shared_from_this<StressClient> {
public:
    StressClient(asio::io_context &ioc, int id)
        : id_(id), send_strand_(asio::make_strand(ioc)),
          ws_strand_(asio::make_strand(ioc)), ws_(ws_strand_) {}

    // ── gRPC 注册+登录获取 token ──
    bool auth_setup(const std::string &auth_addr) {
        auto channel = grpc::CreateChannel(auth_addr,
                                            grpc::InsecureChannelCredentials());
        auto stub = auth::AuthService::NewStub(channel);

        std::string username = fmt::format("stress_{:04d}", id_);
        std::string password = "stress_test";

        // 注册（忽略已存在错误）
        {
            grpc::ClientContext ctx;
            auth::RegisterRequest req;
            auth::AuthResponse resp;
            req.set_username(username);
            req.set_password(password);
            req.set_nickname(username);
            auto status = stub->Register(&ctx, req, &resp);
            if (status.ok() && resp.success()) {
                spdlog::debug("[{}] registered", username);
            }
        }

        // 登录
        {
            grpc::ClientContext ctx;
            auth::LoginRequest req;
            auth::AuthResponse resp;
            req.set_username(username);
            req.set_password(password);
            auto status = stub->Login(&ctx, req, &resp);
            if (!status.ok() || !resp.success()) {
                spdlog::error("[{}] login failed: {} {}", username,
                              status.error_message(), resp.error_message());
                return false;
            }
            token_ = resp.token();
            user_id_ = resp.user().user_id();
            spdlog::debug("[{}] logged in, userID={}", username, user_id_);
        }
        return true;
    }

    void set_target(const std::string &target_id) { target_id_ = target_id; }
    std::string user_id() const { return user_id_; }
    void stop() { running_ = false; }

    // ── WebSocket 连接 + 消息循环 ──
    asio::awaitable<void> run(const std::string &host, const std::string &port) {
        boost::system::error_code ec;
        beast::flat_buffer buffer;

        // 1. TCP 连接
        tcp::resolver resolver(co_await asio::this_coro::executor);
        auto endpoints = co_await resolver.async_resolve(host, port,
            asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            spdlog::error("[{}] resolve failed: {}", id_, ec.message());
            g_stats.connections_failed++;
            co_return;
        }

        co_await asio::async_connect(ws_.next_layer(), endpoints,
            asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            spdlog::error("[{}] connect failed: {}", id_, ec.message());
            g_stats.connections_failed++;
            co_return;
        }

        // 2. WebSocket 握手
        co_await ws_.async_handshake(host, "/",
            asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            spdlog::error("[{}] handshake failed: {}", id_, ec.message());
            g_stats.connections_failed++;
            co_return;
        }

        // 3. 发送认证帧
        ws_.binary(true);
        sdkws::SdkWSReq auth_req;
        auth_req.set_userid(user_id_);
        auth_req.set_token(token_);
        auth_req.set_type(0);

        std::string auth_bin = auth_req.SerializeAsString();
        co_await ws_.async_write(asio::buffer(auth_bin),
            asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            spdlog::error("[{}] auth write failed: {}", id_, ec.message());
            g_stats.connections_failed++;
            co_return;
        }

        // 4. 读取认证响应
        co_await ws_.async_read(buffer,
            asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            spdlog::error("[{}] auth read failed: {}", id_, ec.message());
            g_stats.connections_failed++;
            co_return;
        }
        {
            std::string data = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());
            sdkws::SdkWSResp resp;
            if (!resp.ParseFromString(data) || resp.errorcode() != "0") {
                spdlog::error("[{}] auth rejected: {}", id_, resp.errormsg());
                g_stats.connections_failed++;
                co_return;
            }
        }

        g_stats.connections_ok++;
        spdlog::info("[{}] connected, target={}", id_, target_id_);

        // 5. send_loop 跑在 send_strand_（独立线程），recv_loop 跑在 ws_strand_
        auto self = shared_from_this();
        asio::co_spawn(send_strand_,
            [self]() -> asio::awaitable<void> { co_await self->send_loop(); },
            asio::detached);
        co_await recv_loop();
    }

private:
    // ── 发送回调链：串行化 write 操作 ──
    struct PendingWrite {
        std::shared_ptr<std::string> bin;
        std::string client_msgid;
        std::chrono::steady_clock::time_point send_time;
    };

    void start_next_write() {
        if (send_queue_.empty()) {
            sending_ = false;
            return;
        }
        auto item = std::move(send_queue_.front());
        send_queue_.pop_front();
        ws_.binary(true);
        ws_.async_write(asio::buffer(*item.bin),
            [self = shared_from_this(),
             client_msgid = std::move(item.client_msgid),
             send_time = item.send_time](boost::system::error_code ec,
                                         std::size_t) {
                if (ec) {
                    g_stats.send_errors++;
                    self->send_queue_.clear();
                    self->sending_ = false;
                    return;
                }
                g_stats.messages_sent++;
                {
                    std::lock_guard lk(g_pending_mtx);
                    g_pending_map[client_msgid] = send_time;
                }
                self->start_next_write();
            });
    }

    // ── 发送循环（timer 跑在 send_strand_，写操作 post 到 ws_strand_）──
    asio::awaitable<void> send_loop() {
        boost::system::error_code ec;
        int64_t interval_us = 1'000'000 / g_config.rate;
        auto timer = asio::steady_timer(send_strand_);
        int msg_seq = 0;

        while (running_) {
            timer.expires_after(std::chrono::microseconds(interval_us));
            co_await timer.async_wait(
                asio::redirect_error(asio::use_awaitable, ec));
            if (ec || !running_) break;

            // 构造消息
            sdkws::MsgData msg;
            msg.set_sendid(user_id_);
            msg.set_recvid(target_id_);
            msg.set_convid(user_id_ + "_" + target_id_);
            std::string cid = fmt::format("stress_{}_{}", id_, msg_seq++);
            msg.set_clientmsgid(cid);
            msg.set_sessiontype(1);
            msg.set_contenttype(101);
            msg.set_content(fmt::format("stress test msg #{}", msg_seq));
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            msg.set_sendtime(static_cast<double>(now_ms) / 1000.0);

            sdkws::SendMessageReq send_req;
            *send_req.add_msgs() = msg;

            sdkws::SdkWSReq ws_req;
            ws_req.set_userid(user_id_);
            ws_req.set_token(token_);
            ws_req.set_type(101);
            ws_req.set_data(send_req.SerializeAsString());

            auto bin = std::make_shared<std::string>(ws_req.SerializeAsString());
            auto send_time = std::chrono::steady_clock::now();

            // 投递到 ws_strand_ 执行入队 + 触发写入
            asio::post(ws_strand_,
                [self = shared_from_this(),
                 bin = std::move(bin), cid = std::move(cid),
                 send_time]() mutable {
                    self->send_queue_.push_back(
                        {std::move(bin), std::move(cid), send_time});
                    if (!self->sending_) {
                        self->sending_ = true;
                        self->start_next_write();
                    }
                });
        }
        co_return;
    }

    // ── 接收循环（ACK 通过 send_queue_ 统一发，避免并发写）──
    asio::awaitable<void> recv_loop() {
        auto self = shared_from_this();
        boost::system::error_code ec;
        beast::flat_buffer buffer;

        while (running_) {
            buffer.consume(buffer.size());
            co_await ws_.async_read(buffer,
                asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                if (ec != websocket::error::closed &&
                    ec != asio::error::eof) {
                    g_stats.recv_errors++;
                }
                spdlog::debug("[{}] recv closed: {}", id_, ec.message());
                break;
            }

            std::string data = beast::buffers_to_string(buffer.data());
            sdkws::SdkWSResp resp;
            if (!resp.ParseFromString(data)) continue;

            // DEBUG: 看看实际收到的是什么 type
            static std::atomic<int> dbg_type{0};
            if (dbg_type++ < 10) {
                spdlog::warn("[{}] recv resp.type={} data_size={}",
                             id_, resp.type(), resp.data().size());
            }

            if (resp.type() == 101) {
                // 发送消息的服务器 ACK，忽略
            } else if (resp.type() == 104 || resp.type() == 200) {
                // 在线/离线推送 → 计算延迟 + 发 ACK
                int32_t ack_type =
                    (resp.type() == 104) ? 1 : 2;  // 1=online 2=offline
                sdkws::MsgData msg;
                if (!msg.ParseFromString(resp.data())) {
                    static std::atomic<int> dbg_pf{0};
                    if (dbg_pf++ < 5) {
                        spdlog::warn("[{}] msg.ParseFromString FAILED type={} data_size={}",
                                     id_, resp.type(), resp.data().size());
                    }
                    continue;
                }
                    // 计算延迟
                    if (resp.type() == 104) {
                        auto now = std::chrono::steady_clock::now();
                        std::lock_guard<std::mutex> lk(g_pending_mtx);
                        auto it = g_pending_map.find(msg.clientmsgid());
                        if (it != g_pending_map.end()) {
                            double lat_ms =
                                std::chrono::duration<double, std::milli>(
                                    now - it->second)
                                    .count();
                            g_pending_map.erase(it);
                            std::lock_guard<std::mutex> lk2(
                                g_stats.latency_mtx);
                            g_stats.latencies_ms.push_back(lat_ms);
                        } else {
                            // DEBUG: 看看收到的 cid 和 map 里第一个 key 长什么样
                            static std::atomic<int> dbg{0};
                            if (dbg++ < 5) {
                                std::lock_guard<std::mutex> lk(g_pending_mtx);
                                std::string first_key =
                                    g_pending_map.empty()
                                        ? "(empty)"
                                        : g_pending_map.begin()->first;
                                spdlog::warn(
                                    "[{}] cid mismatch: recv='{}' map_first='{}' map_size={}",
                                    id_, msg.clientmsgid(), first_key,
                                    g_pending_map.size());
                            }
                        }
                    }
                    g_stats.messages_received++;

                    // 构造 ACK 并投递到 ws_strand_ 的写队列
                    sdkws::AckReq ack;
                    ack.set_userid(user_id_);
                    ack.add_servermsgids(msg.servermsgid());
                    ack.set_acktype(ack_type);

                    sdkws::SdkWSReq ack_ws;
                    ack_ws.set_userid(user_id_);
                    ack_ws.set_token(token_);
                    ack_ws.set_type(106);
                    ack_ws.set_data(ack.SerializeAsString());

                    auto ack_bin = std::make_shared<std::string>(
                        ack_ws.SerializeAsString());
                    asio::post(ws_strand_,
                        [self, ack_bin = std::move(ack_bin)]() mutable {
                            self->send_queue_.push_back(
                                {std::move(ack_bin), "", {}});
                            if (!self->sending_) {
                                self->sending_ = true;
                                self->start_next_write();
                            }
                        });
            }
        }
        co_return;
    }

    int id_;
    asio::strand<asio::io_context::executor_type> send_strand_;
    asio::strand<asio::io_context::executor_type> ws_strand_;
    std::string user_id_;
    std::string token_;
    std::string target_id_;
    websocket::stream<tcp::socket> ws_;
    bool running_{true};

    // 写队列：send_loop 的 async_write 通过此队列串行化
    std::deque<PendingWrite> send_queue_;
    bool sending_{false};
};

// ============================================================================
// 统计报告
// ============================================================================
void print_stats(int elapsed_sec, bool final) {
    int64_t sent = g_stats.messages_sent.load();
    int64_t recv = g_stats.messages_received.load();
    int64_t snd_err = g_stats.send_errors.load();
    int64_t rcv_err = g_stats.recv_errors.load();
    int64_t conn_ok = g_stats.connections_ok.load();
    int64_t conn_fail = g_stats.connections_failed.load();

    std::vector<double> lats;
    {
        std::lock_guard<std::mutex> lk(g_stats.latency_mtx);
        lats = g_stats.latencies_ms;
    }
    std::sort(lats.begin(), lats.end());

    double p50 = 0, p95 = 0, p99 = 0;
    if (!lats.empty()) {
        p50 = lats[lats.size() * 0.50];
        p95 = lats[lats.size() * 0.95];
        p99 = lats[lats.size() * 0.99];
    }

    double throughput = elapsed_sec > 0 ? sent / (double)elapsed_sec : 0;

    if (final) {
        spdlog::info("============================================================");
        spdlog::info("  FINAL REPORT");
        spdlog::info("============================================================");
    }

    spdlog::info("  Time elapsed:      {}s", elapsed_sec);
    spdlog::info("  Connections:       {} OK / {} FAIL", conn_ok, conn_fail);
    spdlog::info("  Messages sent:     {} ({} err)", sent, snd_err);
    spdlog::info("  Messages received: {}", recv);
    spdlog::info("  Recv errors:       {}", rcv_err);
    spdlog::info("  Throughput:        {:.0f} msg/s", throughput);
    spdlog::info("  Latency samples:   {}", lats.size());
    spdlog::info("  Latency P50:       {:.2f} ms", p50);
    spdlog::info("  Latency P95:       {:.2f} ms", p95);
    spdlog::info("  Latency P99:       {:.2f} ms", p99);
    if (!lats.empty()) {
        spdlog::info("  Latency min:       {:.2f} ms", lats.front());
        spdlog::info("  Latency max:       {:.2f} ms", lats.back());
        double avg = 0;
        for (double v : lats) avg += v;
        avg /= lats.size();
        spdlog::info("  Latency avg:       {:.2f} ms", avg);
    }
    if (final) {
        spdlog::info("============================================================");
    }
}

// ============================================================================
// 参数解析
// ============================================================================
bool parse_args(int argc, char *argv[]) {
    for (int i = 1; i < argc; i += 2) {
        std::string key = argv[i];
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << key << std::endl;
            return false;
        }
        std::string val = argv[i + 1];
        if (key == "--clients") g_config.clients = std::stoi(val);
        else if (key == "--rate") g_config.rate = std::stoi(val);
        else if (key == "--duration") g_config.duration = std::stoi(val);
        else if (key == "--host") {
            auto pos = val.find(':');
            if (pos != std::string::npos) {
                g_config.gateway_host = val.substr(0, pos);
                g_config.gateway_port = val.substr(pos + 1);
            } else {
                g_config.gateway_host = val;
            }
        }
        else if (key == "--auth") g_config.auth_host = val;
        else {
            std::cerr << "Unknown arg: " << key << std::endl;
            return false;
        }
    }
    if (g_config.clients < 2) {
        std::cerr << "Need at least 2 clients" << std::endl;
        return false;
    }
    if (g_config.clients % 2 != 0) {
        std::cerr << "Clients must be even" << std::endl;
        return false;
    }
    return true;
}

// ============================================================================
// 主流程
// ============================================================================
asio::awaitable<void> run_stress_test() {
    auto executor = co_await asio::this_coro::executor;
    int N = g_config.clients;

    spdlog::info("=== IM Stress Test ===");
    spdlog::info("  Clients:  {}", N);
    spdlog::info("  Rate:     {} msg/s/client", g_config.rate);
    spdlog::info("  Duration: {}s", g_config.duration);
    spdlog::info("  Gateway:  {}:{}", g_config.gateway_host, g_config.gateway_port);
    spdlog::info("  Auth:     {}", g_config.auth_host);
    spdlog::info("");

    // ── 阶段 1: 注册+登录所有用户 ──
    spdlog::info("[Phase 1] Authenticating {} clients...", N);
    std::vector<std::shared_ptr<StressClient>> clients(N);
    for (int i = 0; i < N; ++i) {
        clients[i] = std::make_shared<StressClient>(
            static_cast<asio::io_context&>(executor.context()), i);
        if (!clients[i]->auth_setup(g_config.auth_host)) {
            spdlog::error("Auth setup failed for client {}", i);
            co_return;
        }
    }

    // ── 配对：client[i] → client[(i+1) % N] ──
    // 相邻配对，形成 N 条单向消息流
    // 每对互为收发关系：i 发给 (i+1)%N，同时也收到 (i-1+N)%N 发来的
    for (int i = 0; i < N; ++i) {
        clients[i]->set_target(clients[(i + 1) % N]->user_id());
    }
    spdlog::info("[Phase 1] All {} clients authenticated, pairs: {} -> {}",
                 N, clients[0]->user_id(), clients[1]->user_id());

    // ── 阶段 2: 并发建立 WebSocket 连接 + 启动消息循环 ──
    spdlog::info("[Phase 2] Connecting {} WebSocket clients...", N);
    for (int i = 0; i < N; ++i) {
        asio::co_spawn(executor,
            clients[i]->run(g_config.gateway_host, g_config.gateway_port),
            asio::detached);
    }
    // 等待连接全部建立（最多等 30 秒）
    {
        auto timer = asio::steady_timer(executor);
        for (int wait = 0; wait < 30; ++wait) {
            timer.expires_after(std::chrono::seconds(1));
            co_await timer.async_wait(asio::use_awaitable);
            int64_t ok = g_stats.connections_ok;
            int64_t fail = g_stats.connections_failed;
            spdlog::info("[Phase 2] connections: {} OK / {} FAIL", ok, fail);
            if (ok + fail >= N) break;
        }
    }

    int64_t connected = g_stats.connections_ok.load();
    spdlog::info("[Phase 2] {} of {} clients connected", connected, N);

    if (connected < 2) {
        spdlog::error("Not enough connections to run test");
        co_return;
    }

    // ── 阶段 3: 运行压测 ──
    spdlog::info("[Phase 3] Running stress test for {}s...", g_config.duration);
    auto start_time = std::chrono::steady_clock::now();
    auto timer = asio::steady_timer(executor);

    // 预热 5 秒
    co_await asio::steady_timer(executor,
        std::chrono::seconds(std::min(5, g_config.duration / 4)))
        .async_wait(asio::use_awaitable);
    spdlog::info("  (warmup done, starting measurement)");

    // 重置统计 & 清空预热期间残留的 pending 记录
    g_stats.messages_sent = 0;
    g_stats.messages_received = 0;
    g_stats.send_errors = 0;
    g_stats.recv_errors = 0;
    {
        std::lock_guard<std::mutex> lk(g_stats.latency_mtx);
        g_stats.latencies_ms.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_pending_mtx);
        g_pending_map.clear();
    }
    auto measure_start = std::chrono::steady_clock::now();

    // 定期报告
    int report_interval = std::max(5, g_config.duration / 6);
    for (int elapsed = 0; elapsed < g_config.duration;) {
        timer.expires_after(std::chrono::seconds(report_interval));
        co_await timer.async_wait(asio::use_awaitable);
        elapsed += report_interval;
        if (elapsed > g_config.duration) elapsed = g_config.duration;
        print_stats(elapsed, false);
    }

    // 停止所有客户端
    for (auto &c : clients) {
        c->stop();
    }

    double total_elapsed =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - measure_start)
            .count();
    print_stats((int)total_elapsed, true);
    spdlog::info("Test complete. Press Ctrl+C to exit.");
}

int main(int argc, char *argv[]) {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S] %v");

    if (!parse_args(argc, argv)) {
        std::cerr << "Usage: stress_test --clients N --rate N --duration N "
                     "[--host HOST:PORT] [--auth HOST:PORT]\n";
        return 1;
    }

    // 需要一个足够大的 io_context 线程池来跑 N 个协程
    int threads = std::min(g_config.clients / 10 + 1, (int)std::thread::hardware_concurrency());
    if (threads < 2) threads = 2;
    asio::io_context ioc{threads};

    asio::co_spawn(ioc, run_stress_test(), asio::detached);

    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i) {
        pool.emplace_back([&ioc] { ioc.run(); });
    }
    for (auto &t : pool) t.join();

    return 0;
}
