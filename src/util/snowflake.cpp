#include "util/snowflake.h"
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>

Snowflake& Snowflake::instance() {
    static Snowflake inst(1, 1);  // 默认值，会被 init() 覆盖
    return inst;
}

void Snowflake::init(int64_t workerId, int64_t datacenterId) {
    static bool initialized = false;
    if (!initialized) {
        auto& inst = instance();
        inst.workerId_ = workerId;
        inst.datacenterId_ = datacenterId;
        initialized = true;
    }
}

Snowflake::Snowflake(int64_t workerId, int64_t datacenterId)
    : workerId_(workerId)
    , datacenterId_(datacenterId)
    , sequence_(0)
    , lastTimestamp_(-1LL) {}

int64_t Snowflake::currentMillis() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now.time_since_epoch())
        .count();
}

int64_t Snowflake::nextId() {
    std::lock_guard<std::mutex> lock(mutex_);

    int64_t timestamp = currentMillis();

    // 时钟回拨检测
    if (timestamp < lastTimestamp_) {
        int64_t offset = lastTimestamp_ - timestamp;
        // 回拨 < 100ms：自旋等待
        if (offset < 100) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(offset + 1));
            timestamp = currentMillis();
        } else {
            throw std::runtime_error(
                "Snowflake: clock moved backwards by " +
                std::to_string(offset) + "ms, refusing to generate id");
        }
    }

    if (timestamp == lastTimestamp_) {
        // 同一毫秒内，序列号自增
        sequence_ = (sequence_ + 1) & MAX_SEQUENCE;
        if (sequence_ == 0) {
            // 当前毫秒序列号用完，等待下一毫秒
            while (timestamp <= lastTimestamp_) {
                timestamp = currentMillis();
            }
        }
    } else {
        // 不同毫秒，序列号重置
        sequence_ = 0;
    }

    lastTimestamp_ = timestamp;

    return ((timestamp - EPOCH) << TIMESTAMP_SHIFT) |
           (datacenterId_ << DATACENTER_ID_SHIFT) |
           (workerId_ << WORKER_ID_SHIFT) |
           sequence_;
}