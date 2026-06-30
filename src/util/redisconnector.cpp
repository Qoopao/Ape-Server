#include "util/redisconnector.h"

#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>
#include <boost/redis/src.hpp>

#include <spdlog/spdlog.h>

// ═══════════════════════════════════════════════════════════════════════════
// RedisConnector 实现 — 基于 IOC_Pool
//
// 架构：
//   - IOC_Pool 管理 io_context 池和线程生命周期
//   - 每个 worker 绑定一个 boost::redis::connection
//   - 操作通过 co_await dispatch 切换到目标 io_context 后执行
//   - 零阻塞，全异步协程模型
// ═══════════════════════════════════════════════════════════════════════════

RedisConnector& RedisConnector::instance() {
    static RedisConnector instance;
    return instance;
}

RedisConnector::~RedisConnector() {
    stop();
}

void RedisConnector::start(int pool_size) {
    if (_running) {
        spdlog::warn("RedisConnector already running (pool_size={})", _poolSize);
        return;
    }

    _poolSize = pool_size;
    _iocPool = std::make_unique<IOC_Pool>(pool_size);
    _connections.resize(pool_size);

    boost::redis::config cfg;
    cfg.addr.host = "127.0.0.1";
    cfg.addr.port = "6379";
    cfg.password = "redis123";
    cfg.health_check_interval = std::chrono::seconds{5};
    cfg.connect_timeout = std::chrono::seconds{3};
    cfg.reconnect_wait_interval = std::chrono::seconds{2};
    

    for (int i = 0; i < pool_size; ++i) {
        auto& ioc = _iocPool->get_ioc(i);

        boost::redis::logger lg{boost::redis::logger::level::disabled};
        _connections[i] = std::make_unique<boost::redis::connection>(ioc,lg);

        _connections[i]->async_run(cfg, [i](boost::system::error_code ec) {
            if (ec) {
                spdlog::error("RedisConnector[{}] async_run failed: {}", i,
                              ec.message());
            } else {
                spdlog::info("RedisConnector[{}] async_run completed", i);
            }
        });
    }

    _iocPool->startPool();
    _running = true;
    spdlog::info("RedisConnector pool started (IOC_Pool, pool_size={})",
                 pool_size);
}

void RedisConnector::stop() {
    if (!_running) return;

    for (int i = 0; i < _poolSize; ++i) {
        if (_connections[i]) {
            _connections[i]->cancel(boost::redis::operation::all);
        }
    }

    _iocPool->stopPool();

    _connections.clear();
    _poolSize = 0;
    _running = false;

    spdlog::info("RedisConnector pool stopped");
}

int RedisConnector::pool_size() const { return _poolSize; }

boost::asio::io_context::executor_type
RedisConnector::get_executor() {
    return _iocPool->get_ioc(next_idx()).get_executor();
}

int RedisConnector::next_idx() {
    return _nextIdx.fetch_add(1, std::memory_order_relaxed) % _poolSize;
}

// ═══════════════════════════════════════════════════════════════════════════
// 辅助宏：切换到目标 io_context 并执行 Redis 操作
// 用法：REDIS_CO_AWAIT(idx, conn.async_exec(req, resp, ...))
// ═══════════════════════════════════════════════════════════════════════════

// ── 基础 KV 操作 ──

boost::asio::awaitable<std::optional<std::string>>
RedisConnector::get(const std::string& key) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("GET", key);
    boost::redis::response<std::optional<std::string>> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

boost::asio::awaitable<void>
RedisConnector::set(const std::string& key, const std::string& val) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("SET", key, val);
    boost::redis::response<std::string> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    (void)std::get<0>(resp).value();
}

boost::asio::awaitable<void>
RedisConnector::setex(const std::string& key, long long sec,
                      const std::string& val) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("SETEX", key, std::to_string(sec), val);
    boost::redis::response<std::string> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    (void)std::get<0>(resp).value();
}

boost::asio::awaitable<long long>
RedisConnector::del(const std::string& key) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("DEL", key);
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

boost::asio::awaitable<bool>
RedisConnector::exists(const std::string& key) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("EXISTS", key);
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value() > 0;
}

boost::asio::awaitable<void>
RedisConnector::expire(const std::string& key, long long sec) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("EXPIRE", key, std::to_string(sec));
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    (void)std::get<0>(resp).value();
}

boost::asio::awaitable<bool>
RedisConnector::setnxex(const std::string& key, const std::string& val,
                        long long sec) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("SET", key, val, "NX", "EX", std::to_string(sec));
    boost::redis::response<std::optional<std::string>> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value().has_value();
}

boost::asio::awaitable<long long>
RedisConnector::ttl(const std::string& key) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("TTL", key);
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

boost::asio::awaitable<std::vector<std::optional<std::string>>>
RedisConnector::mget(const std::vector<std::string>& keys) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push_range("MGET", keys);
    boost::redis::response<std::vector<std::optional<std::string>>> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

// ── Hash 操作 ──

boost::asio::awaitable<long long>
RedisConnector::hset(const std::string& key, const std::string& field,
                     const std::string& val) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("HSET", key, field, val);
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

boost::asio::awaitable<std::optional<std::string>>
RedisConnector::hget(const std::string& key, const std::string& field) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("HGET", key, field);
    boost::redis::response<std::optional<std::string>> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

boost::asio::awaitable<long long>
RedisConnector::hdel(const std::string& key, const std::string& field) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("HDEL", key, field);
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

boost::asio::awaitable<long long>
RedisConnector::hdel_range(const std::string& key,
                           const std::vector<std::string>& fields) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push_range("HDEL", key, fields);
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

boost::asio::awaitable<std::unordered_map<std::string, std::string>>
RedisConnector::hgetall(const std::string& key) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("HGETALL", key);
    boost::redis::response<std::unordered_map<std::string, std::string>> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

// ── Set 操作 ──

boost::asio::awaitable<long long>
RedisConnector::sadd(const std::string& key, const std::string& member) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("SADD", key, member);
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

boost::asio::awaitable<long long>
RedisConnector::srem(const std::string& key, const std::string& member) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("SREM", key, member);
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

boost::asio::awaitable<long long>
RedisConnector::scard(const std::string& key) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("SCARD", key);
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

// ── List 操作 ──

boost::asio::awaitable<long long>
RedisConnector::rpush(const std::string& key, const std::string& val) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("RPUSH", key, val);
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

boost::asio::awaitable<std::vector<std::string>>
RedisConnector::lrange(const std::string& key, long long start,
                       long long stop) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("LRANGE", key, std::to_string(start), std::to_string(stop));
    boost::redis::response<std::vector<std::string>> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

boost::asio::awaitable<long long>
RedisConnector::ltrim(const std::string& key, long long start, long long stop) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("LTRIM", key, std::to_string(start), std::to_string(stop));
    boost::redis::response<std::string> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    (void)std::get<0>(resp).value();
    co_return 0;
}

// ── Sorted Set 操作 ──

boost::asio::awaitable<long long>
RedisConnector::zadd(const std::string& key, long long score,
                     const std::string& member) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("ZADD", key, std::to_string(score), member);
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

boost::asio::awaitable<std::vector<std::string>>
RedisConnector::zrangebyscore(const std::string& key, long long min,
                              long long max, long long offset,
                              long long count) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;

    if (count > 0) {
        req.push("ZRANGEBYSCORE", key, std::to_string(min),
                 std::to_string(max), "LIMIT", std::to_string(offset),
                 std::to_string(count));
    } else {
        req.push("ZRANGEBYSCORE", key, std::to_string(min),
                 std::to_string(max));
    }
    boost::redis::response<std::vector<std::string>> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

boost::asio::awaitable<long long>
RedisConnector::zrem(const std::string& key, const std::string& member) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("ZREM", key, member);
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

boost::asio::awaitable<long long>
RedisConnector::zrem_range(const std::string& key,
                           const std::vector<std::string>& members) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push_range("ZREM", key, members);
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}

// ── pub/sub ──

boost::asio::awaitable<long long>
RedisConnector::publish(const std::string& channel, const std::string& msg) {
    int idx = next_idx();
    auto& ioc = _iocPool->get_ioc(idx);
    auto& conn = *_connections[idx];

    co_await boost::asio::dispatch(
        boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

    boost::redis::request req;
    req.push("PUBLISH", channel, msg);
    boost::redis::response<long long> resp;
    co_await conn.async_exec(req, resp, boost::asio::use_awaitable);
    co_return std::get<0>(resp).value();
}