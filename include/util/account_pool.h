#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <random>
#include <algorithm>
#include <utility>
#include <boost/asio/awaitable.hpp>
#include "storage/mysqlhandler.h"

class AccountPoolAllocator {
public:
    // 号段范围：[1000000000, 9999999999]，共 90 亿个
    static constexpr uint64_t kMinAccount  = 1000000000ULL;
    static constexpr uint64_t kMaxAccount  = 9999999999ULL;

    // 每次补货分配的号段大小
    static constexpr uint64_t kSegmentSize = 10000;

    // 池中可用账号低于此值时触发补货
    static constexpr uint64_t kLowWaterMark = 1000;

    // 补货目标数量（每次补一批）
    static constexpr uint64_t kRefillBatch = 10000;

    // 补货锁的 TTL（秒）
    static constexpr int kRefillLockTtlSeconds = 30;

    // 分配一个号段，返回 [start, end)。失败返回 (0, 0)
    // 用数据库事务 + 行锁保证并发下号段不重叠
    static boost::asio::awaitable<std::pair<uint64_t, uint64_t>> allocate_segment(uint64_t size);

    // 生成一批打乱的账号（从指定号段）
    static std::vector<uint64_t> generate_shuffled_batch(
        uint64_t start, uint64_t end);
};

class AccountPoolLock {
public:
    // 尝试获取锁。成功返回 true，失败返回 false。
    // holder 是持有者标识（实例ID+协程ID），用于调试和防误删。
    static boost::asio::awaitable<bool> try_acquire(
        const std::string &lock_name,
        const std::string &holder,
        int ttl_seconds);

    // 释放锁。只有 holder 匹配才释放，防止误删别人的锁。
    static boost::asio::awaitable<void> release(
        const std::string &lock_name,
        const std::string &holder);
};

class AccountPoolTool {
public:
    // 从池中原子取一个可用账号，返回 0 表示池空
    static boost::asio::awaitable<uint64_t> acquire();

    // 补一批账号到池中
    static boost::asio::awaitable<void> refill(uint64_t batch_size);

    // 检查池中可用数量，低于阈值时触发补货（带数据库锁互斥）
    static boost::asio::awaitable<void> ensure_pool_not_low();

    // 查询池中可用数量
    static boost::asio::awaitable<uint64_t> available_count();

    // 批量插入账号（号段内绝对不碰撞）
    static boost::asio::awaitable<void> batch_insert_accounts(
        const std::vector<uint64_t> &accounts);
};
