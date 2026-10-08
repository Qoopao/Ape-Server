#include "util/account_pool.h"
#include "storage/mysqlconnector.h"
#include "storage/registration_store.h"
std::vector<uint64_t>
AccountPoolAllocator::generate_shuffled_batch(uint64_t start, uint64_t end) {
  std::vector<uint64_t> batch;
  if (end <= start) {
    return batch;
  }
  batch.reserve(static_cast<size_t>(end - start));
  for (uint64_t i = start; i < end; ++i) {
    batch.push_back(i);
  }

  // 线程局部随机数引擎，避免每次构造
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::shuffle(batch.begin(), batch.end(), rng);
  return batch;
}

boost::asio::awaitable<void> AccountPoolTool::refill(uint64_t batch_size) {
    // Callers must run on get_pool_executor(0).
    co_await registration::refill(MySQLConnector::instance().get_pool(0), batch_size);
}
boost::asio::awaitable<void> AccountPoolTool::ensure_pool_not_low() {
    co_await refill(AccountPoolAllocator::kRefillBatch);
}