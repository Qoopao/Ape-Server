#pragma once
#include <cstdint>
#include <vector>
#include <random>
#include <algorithm>
#include <boost/asio/awaitable.hpp>

class AccountPoolAllocator {
public:
    static constexpr uint64_t kMinAccount = 1000000000ULL;
    static constexpr uint64_t kMaxAccount = 9999999999ULL;
    static constexpr uint64_t kLowWaterMark = 1000;
    static constexpr uint64_t kRefillBatch = 10000;
    static std::vector<uint64_t> generate_shuffled_batch(uint64_t start, uint64_t end);
};

class AccountPoolTool {
public:
    // Run on MySQLConnector::get_pool_executor(0). Both use the same cursor lock.
    static boost::asio::awaitable<void> refill(uint64_t batch_size);
    static boost::asio::awaitable<void> ensure_pool_not_low();
    // Standalone acquire/allocate/insert APIs intentionally removed: registration
    // must not split account allocation, user creation or refill across transactions.
};