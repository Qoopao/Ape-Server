#ifndef REDISCONNECTOR_H
#define REDISCONNECTOR_H

#include "util/ioc_pool.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>
#include <boost/redis.hpp>

// ============================================================================
// RedisConnector — 基于 IOC_Pool 的 Redis 异步连接器
//
// 架构：
//   - 内部使用 IOC_Pool 管理 io_context 池和线程生命周期
//   - 每个 IOC_Pool worker 绑定一个 boost::redis::connection
//   - 所有 Redis 操作通过 co_await + co_spawn 跨 io_context 桥接
//   - 零阻塞，全异步协程模型
// ============================================================================

class RedisConnector {
public:
    static RedisConnector& instance();

    void start(int pool_size = 4);
    void stop();

    int pool_size() const;

    // 返回一个 executor，供 co_spawn 使用（轮询选取 io_context）
    boost::asio::io_context::executor_type get_executor();

    // ── 基础 KV 操作 ──
    boost::asio::awaitable<std::optional<std::string>>
        get(const std::string& key);
    boost::asio::awaitable<void>
        set(const std::string& key, const std::string& val);
    boost::asio::awaitable<void>
        setex(const std::string& key, long long sec, const std::string& val);
    boost::asio::awaitable<long long>
        del(const std::string& key);
    boost::asio::awaitable<bool>
        exists(const std::string& key);
    boost::asio::awaitable<void>
        expire(const std::string& key, long long sec);
    boost::asio::awaitable<bool>
        setnxex(const std::string& key, const std::string& val, long long sec);
    boost::asio::awaitable<long long>
        ttl(const std::string& key);
    boost::asio::awaitable<std::vector<std::optional<std::string>>>
        mget(const std::vector<std::string>& keys);

    // ── Hash 操作 ──
    boost::asio::awaitable<long long>
        hset(const std::string& key, const std::string& field,
             const std::string& val);
    boost::asio::awaitable<std::optional<std::string>>
        hget(const std::string& key, const std::string& field);
    boost::asio::awaitable<long long>
        hdel(const std::string& key, const std::string& field);
    boost::asio::awaitable<long long>
        hdel_range(const std::string& key,
                   const std::vector<std::string>& fields);
    boost::asio::awaitable<std::unordered_map<std::string, std::string>>
        hgetall(const std::string& key);

    // ── Set 操作 ──
    boost::asio::awaitable<long long>
        sadd(const std::string& key, const std::string& member);
    boost::asio::awaitable<long long>
        srem(const std::string& key, const std::string& member);
    boost::asio::awaitable<long long>
        scard(const std::string& key);
    boost::asio::awaitable<std::vector<std::string>>
        smembers(const std::string& key);

    // ── Lua 脚本 ──
    boost::asio::awaitable<long long>
        eval(const std::string& script,
             const std::vector<std::string>& keys,
             const std::vector<std::string>& args);

    // ── List 操作 ──
    boost::asio::awaitable<long long>
        rpush(const std::string& key, const std::string& val);
    boost::asio::awaitable<std::vector<std::string>>
        lrange(const std::string& key, long long start, long long stop);
    boost::asio::awaitable<long long>
        ltrim(const std::string& key, long long start, long long stop);

    // ── Sorted Set 操作 ──
    boost::asio::awaitable<long long>
        zadd(const std::string& key, long long score, const std::string& member);
    boost::asio::awaitable<std::vector<std::string>>
        zrangebyscore(const std::string& key, long long min, long long max,
                      long long offset = 0, long long count = -1);
    boost::asio::awaitable<long long>
        zrem(const std::string& key, const std::string& member);
    boost::asio::awaitable<long long>
        zrem_range(const std::string& key,
                   const std::vector<std::string>& members);

    // ── pub/sub ──
    boost::asio::awaitable<long long>
        publish(const std::string& channel, const std::string& msg);

private:
    RedisConnector() = default;
    ~RedisConnector();
    RedisConnector(const RedisConnector&) = delete;
    RedisConnector& operator=(const RedisConnector&) = delete;

    // 选取一个连接索引（轮询）
    int next_idx();

    std::unique_ptr<IOC_Pool> _iocPool;
    std::vector<std::unique_ptr<boost::redis::connection>> _connections;
    std::atomic<int> _nextIdx{0};
    int _poolSize{0};
    bool _running{false};
};

#endif