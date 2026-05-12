#ifndef SNOWFLAKE_H
#define SNOWFLAKE_H

#include <cstdint>
#include <mutex>

// Snowflake ID 生成器 (线程安全)
// 可用于生成全局唯一且递增的消息序列号
class Snowflake {
public:
    static Snowflake& instance();

    // 初始化 workerId/datacenterId（必须在首次 nextId() 之前调用）
    static void init(int64_t workerId, int64_t datacenterId);

    // 生成下一个 ID
    int64_t nextId();

private:
    Snowflake(int64_t workerId, int64_t datacenterId);

    int64_t currentMillis();

    std::mutex mutex_;
    int64_t workerId_;
    int64_t datacenterId_;
    int64_t sequence_;
    int64_t lastTimestamp_;

    // 2024-01-01 00:00:00 UTC (ms)
    static constexpr int64_t EPOCH = 1704067200000LL;

    // bit 分配: timestamp(41) | datacenter(5) | worker(5) | sequence(12)
    static constexpr int64_t WORKER_ID_BITS = 5;
    static constexpr int64_t DATACENTER_ID_BITS = 5;
    static constexpr int64_t SEQUENCE_BITS = 12;

    static constexpr int64_t MAX_WORKER_ID = (1LL << WORKER_ID_BITS) - 1;
    static constexpr int64_t MAX_DATACENTER_ID = (1LL << DATACENTER_ID_BITS) - 1;
    static constexpr int64_t MAX_SEQUENCE = (1LL << SEQUENCE_BITS) - 1;

    static constexpr int64_t WORKER_ID_SHIFT = SEQUENCE_BITS;
    static constexpr int64_t DATACENTER_ID_SHIFT = WORKER_ID_SHIFT + WORKER_ID_BITS;
    static constexpr int64_t TIMESTAMP_SHIFT = DATACENTER_ID_SHIFT + DATACENTER_ID_BITS;
};

#endif // SNOWFLAKE_H