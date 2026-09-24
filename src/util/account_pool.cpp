#include "util/account_pool.h"
#include "storage/mysqlconnector.h"
#include <spdlog/spdlog.h>

boost::asio::awaitable<std::pair<uint64_t, uint64_t>>
AccountPoolAllocator::allocate_segment(uint64_t size) {
    // 获得一个专有数据库连接
    auto& pool = MySQLConnector::instance().get_pool(0);
    auto conn = co_await pool.async_get_connection(boost::asio::use_awaitable);

    boost::mysql::results result;
    bool need_rollback = false;
    try {
        // ── 开启事务 ──────────────────────────────────────────────
        co_await conn->async_execute("START TRANSACTION", result,
                                     boost::asio::use_awaitable);

        // 锁住游标行，读取当前号段起点。
        // FOR UPDATE 对 id=1 这一行加排他锁，其他协程的 SELECT ... FOR UPDATE 会阻塞，
        // 直到本事务提交或回滚，保证同一时刻只有一个协程能读到并推进游标。
        co_await conn->async_execute(
            "SELECT next_start FROM account_pool_cursor WHERE id=1 FOR UPDATE",
            result, boost::asio::use_awaitable);

        if (result.rows().empty()) {
            throw std::runtime_error("account_pool_cursor row missing");
        }

        uint64_t start = result.rows()[0][0].as_uint64();
        uint64_t end = start + size;

        if (end > kMaxAccount + 1) {
            co_await conn->async_execute("ROLLBACK", result,
                                         boost::asio::use_awaitable);
            co_return std::make_pair(0ULL, 0ULL);
        }

        // 推进游标到号段末尾，下次补货从这里继续。
        // 本次分配的号段是 [start, end)，这里把 next_start 更新为 end，
        // 保证下一次 allocate_segment 拿到的起点不会和本次重叠。
        co_await conn->async_execute(
            "UPDATE account_pool_cursor SET next_start=" +
            std::to_string(end) + " WHERE id=1",
            result, boost::asio::use_awaitable);

        // ── 提交，结束事务──────────────────────────────────────────────
        co_await conn->async_execute("COMMIT", result,
                                     boost::asio::use_awaitable);
        co_return std::make_pair(start, end);

    } catch (const std::exception& e) {
        need_rollback = true;
        spdlog::error("[AccountPool] allocate_segment failed: {}", e.what());
        throw;
    }

    if (need_rollback) {
    boost::mysql::results rb;
    co_await conn->async_execute("ROLLBACK", rb, boost::asio::use_awaitable);
    co_return std::make_pair(0ULL, 0ULL);
}
}

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

// 生成补货锁的持有者标识：实例 ID + 协程 ID
std::string make_lock_holder() {
    static const std::string kInstanceId =
        "inst-" + std::to_string(::getpid());
    static std::atomic<uint64_t> counter{0};
    return kInstanceId + "-" + std::to_string(counter.fetch_add(1));
}

// ────────────────────────────────────────────────────────────
// 取号：从池中原子取一个可用账号
// ────────────────────────────────────────────────────────────
boost::asio::awaitable<uint64_t> AccountPoolTool::acquire() {
    auto &pool = MySQLConnector::instance().get_pool(0);
    auto conn = co_await pool.async_get_connection(boost::asio::use_awaitable);
    boost::mysql::results result;

    bool failed = false;
    uint64_t account = 0;

    try {
        // 开启事务：取号 + 标记分配必须原子完成
        co_await conn->async_execute(
            "START TRANSACTION", result, boost::asio::use_awaitable);

        // 取一条空闲账号并锁住。SKIP LOCKED 让并发请求跳过已锁行，避免阻塞。
        co_await conn->async_execute(
            "SELECT id, account FROM account_pool "
            "WHERE status=0 LIMIT 1 FOR UPDATE SKIP LOCKED",
            result, boost::asio::use_awaitable);

        if (result.rows().empty()) {
            // 池空，回滚后返回 0
            co_await conn->async_execute(
                "ROLLBACK", result, boost::asio::use_awaitable);
            co_return 0;
        }

        uint64_t pool_id = result.rows()[0][0].as_uint64();
        account = result.rows()[0][1].as_uint64();

        // 标记为已分配，防止被其他注册请求重复取走
        co_await conn->async_execute(
            "UPDATE account_pool SET status=1 WHERE id=" +
                std::to_string(pool_id),
            result, boost::asio::use_awaitable);

        // 提交事务，标记正式生效
        co_await conn->async_execute(
            "COMMIT", result, boost::asio::use_awaitable);

    } catch (const std::exception &e) {
        failed = true;
        spdlog::error("[AccountPool] acquire failed: {}", e.what());
    }

    // 回滚放在 catch 之外（catch 里不能 co_await）
    if (failed) {
        boost::mysql::results rb;
        co_await conn->async_execute(
            "ROLLBACK", rb, boost::asio::use_awaitable);
        co_return 0;
    }

    co_return account;
}

// ────────────────────────────────────────────────────────────
// 查询池中可用数量
// ────────────────────────────────────────────────────────────
boost::asio::awaitable<uint64_t> AccountPoolTool::available_count() {
    auto &pool = MySQLConnector::instance().get_pool(0);
    auto conn = co_await pool.async_get_connection(boost::asio::use_awaitable);
    boost::mysql::results result;

    co_await conn->async_execute(
        "SELECT COUNT(*) FROM account_pool WHERE status=0",
        result, boost::asio::use_awaitable);

    if (result.rows().empty()) {
        co_return 0;
    }
    co_return result.rows()[0][0].as_uint64();
}

// ────────────────────────────────────────────────────────────
// 批量插入账号
// ────────────────────────────────────────────────────────────
boost::asio::awaitable<void> AccountPoolTool::batch_insert_accounts(
    const std::vector<uint64_t> &accounts) {
    if (accounts.empty()) {
        co_return;
    }

    auto &pool = MySQLConnector::instance().get_pool(0);
    auto conn = co_await pool.async_get_connection(boost::asio::use_awaitable);
    boost::mysql::results result;

    bool failed = false;

    try {
        co_await conn->async_execute(
            "START TRANSACTION", result, boost::asio::use_awaitable);

        // 每 1000 个一条 INSERT，避免单条 SQL 过长
        constexpr size_t kChunk = 1000;
        for (size_t i = 0; i < accounts.size(); i += kChunk) {
            size_t end = std::min(i + kChunk, accounts.size());
            std::string sql = "INSERT INTO account_pool (account) VALUES ";
            for (size_t j = i; j < end; ++j) {
                if (j > i) sql += ",";
                sql += "(" + std::to_string(accounts[j]) + ")";
            }
            co_await conn->async_execute(
                sql, result, boost::asio::use_awaitable);
        }

        co_await conn->async_execute(
            "COMMIT", result, boost::asio::use_awaitable);

    } catch (const std::exception &e) {
        failed = true;
        spdlog::error("[AccountPool] batch_insert failed: {}", e.what());
    }

    if (failed) {
        boost::mysql::results rb;
        co_await conn->async_execute(
            "ROLLBACK", rb, boost::asio::use_awaitable);
        throw std::runtime_error("batch_insert_accounts failed");
    }
}

// ────────────────────────────────────────────────────────────
// 补货：分配号段 + 打乱 + 批量插入
// ────────────────────────────────────────────────────────────
boost::asio::awaitable<void> AccountPoolTool::refill(uint64_t batch_size) {
    // 1. 分配号段（内部已用事务 + FOR UPDATE 保证不重叠）
    auto [start, end] =
        co_await AccountPoolAllocator::allocate_segment(batch_size);
    if (start == 0) {
        spdlog::error("[AccountPool] refill failed: no more segments");
        co_return;
    }

    // 2. 生成打乱列表
    auto batch = AccountPoolAllocator::generate_shuffled_batch(start, end);

    // 3. 批量插入
    co_await batch_insert_accounts(batch);

    spdlog::info("[AccountPool] Refilled {} accounts: [{}, {})",
                 batch.size(), start, end);
}


// ────────────────────────────────────────────────────────────
// 懒补货：池子低于水位时触发，用数据库锁保证同一时间只有一个补货
// ────────────────────────────────────────────────────────────
boost::asio::awaitable<void> AccountPoolTool::ensure_pool_not_low() {
    uint64_t cnt = co_await available_count();
    if (cnt >= AccountPoolAllocator::kLowWaterMark) {
        co_return;
    }

    // 用数据库表抢补货锁
    std::string holder = make_lock_holder();
    bool locked = co_await AccountPoolLock::try_acquire(
        "account_pool_refill", holder,
        AccountPoolAllocator::kRefillLockTtlSeconds);

    if (!locked) {
        co_return;  // 已有协程在补货
    }

    spdlog::warn("[AccountPool] Low water mark: available={}, refilling...", cnt);

    // catch 里不能 co_await，这里只标记失败，释放锁放到 catch 之后
    bool refill_failed = false;

    try {
        co_await refill(AccountPoolAllocator::kRefillBatch);
    } catch (const std::exception &e) {
        refill_failed = true;
        spdlog::error("[AccountPool] refill failed: {}", e.what());
    }

    // 无论成功失败，都要释放锁
    co_await AccountPoolLock::release("account_pool_refill", holder);

    if (refill_failed) {
        spdlog::error("[AccountPool] refill failed, lock released");
        co_return;  // 补货失败不抛异常，下次水位检查再触发
    }
}

// ════════════════════════════════════════════════════════════
// AccountPoolLock 实现
// ════════════════════════════════════════════════════════════

boost::asio::awaitable<bool> AccountPoolLock::try_acquire(
    const std::string &lock_name,
    const std::string &holder,
    int ttl_seconds) {

    auto &pool = MySQLConnector::instance().get_pool(0);
    auto conn = co_await pool.async_get_connection(boost::asio::use_awaitable);
    boost::mysql::results result;

    // 1. 清理过期锁，避免死锁残留
    co_await conn->async_execute(
        "DELETE FROM account_pool_refill_lock "
        "WHERE lock_name='" + lock_name + "' AND expires_at < NOW()",
        result, boost::asio::use_awaitable);

    // 2. 抢锁：插入锁记录；若锁已存在但已过期，则更新为当前持有者
    co_await conn->async_execute(
        "INSERT INTO account_pool_refill_lock (lock_name, holder, expires_at) "
        "VALUES ('" + lock_name + "', '" + holder + "', "
        "DATE_ADD(NOW(), INTERVAL " + std::to_string(ttl_seconds) + " SECOND)) "
        "ON DUPLICATE KEY UPDATE "
        "holder = IF(expires_at < NOW(), VALUES(holder), holder), "
        "expires_at = IF(expires_at < NOW(), VALUES(expires_at), expires_at)",
        result, boost::asio::use_awaitable);

    // affected_rows: 1=插入成功, 2=更新成功(抢到过期锁), 0=锁仍有效
    co_return result.affected_rows() >= 1;
}

boost::asio::awaitable<void> AccountPoolLock::release(
    const std::string &lock_name,
    const std::string &holder) {

    auto &pool = MySQLConnector::instance().get_pool(0);
    auto conn = co_await pool.async_get_connection(boost::asio::use_awaitable);
    boost::mysql::results result;

    co_await conn->async_execute(
        "DELETE FROM account_pool_refill_lock "
        "WHERE lock_name='" + lock_name + "' AND holder='" + holder + "'",
        result, boost::asio::use_awaitable);
}