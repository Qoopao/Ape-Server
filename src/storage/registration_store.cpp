#include "storage/registration_store.h"
#include "util/account_pool.h"
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql/error_with_diagnostics.hpp>
#include <boost/mysql/format_sql.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/common_server_errc.hpp>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <spdlog/spdlog.h>

namespace registration {
namespace {
using boost::asio::use_awaitable;
using boost::asio::awaitable;
using boost::mysql::results;
using boost::mysql::any_connection;

// 兼容mysql的unsigned bigint和bigint
// where: 调用点标识，便于定位是哪个查询出的问题
uint64_t number(boost::mysql::field_view f, const char* where) {
    if (f.is_uint64()) return f.as_uint64();
    if (f.is_int64() && f.as_int64() >= 0) return static_cast<uint64_t>(f.as_int64());
    spdlog::error("number() failed at [{}]: is_null={} is_uint64={} is_int64={} is_string={} is_double={}",
                  where, f.is_null(), f.is_uint64(), f.is_int64(),
                  f.is_string(), f.is_double());
    throw Error("SCHEMA_ERROR", std::string("Expected a nonnegative integer at ") + where);
}

// result = to_hex(sha256(salt + data))
std::string digest(const std::string& salt, const std::string& data) {
    const auto bytes = payload(salt, data, "");
    std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
    unsigned int size = 0;
    if (!EVP_Digest(bytes.data(), bytes.size(), out.data(), &size, EVP_sha256(), nullptr))
        throw Error("INTERNAL_ERROR", "Unable to hash registration input");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned i = 0; i < size; ++i) {
        result += hex[out[i] >> 4]; result += hex[out[i] & 15];
    }
    return result;
}

// 幂等性校验
void verify(const Input& input, boost::mysql::row_view row) {
    const std::string salt(row[0].as_string());
    const std::string expected(row[1].as_string());
    const auto actual = digest(salt, input.payload);
    if (actual.size() != expected.size() ||
        CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) != 0)
        throw Error("IDEMPOTENCY_CONFLICT", "Request ID was already used for different input");
}

// 事务启动
awaitable<void> begin(any_connection& conn) {
    results r;
    co_await conn.async_execute("SET SESSION innodb_lock_wait_timeout=2", r, use_awaitable);
    co_await conn.async_execute("SET TRANSACTION ISOLATION LEVEL READ COMMITTED", r, use_awaitable);
    co_await conn.async_execute("START TRANSACTION", r, use_awaitable);
}

// 事务回滚
awaitable<bool> rollback(any_connection& conn) {
    results r;
    boost::system::error_code ec;
    co_await conn.async_execute("ROLLBACK", r, boost::asio::redirect_error(use_awaitable, ec));
    if (ec) spdlog::warn("rollback failed: {}", ec.message());
    co_return !ec;
}

// 判断是不是MySQL返回的、错误码等于code的服务器错误
bool mysqlCode(const std::exception_ptr& e, int code) {
    try {
        std::rethrow_exception(e);
    }
    catch (const boost::system::system_error& ex) {
        return ex.code() == static_cast<boost::mysql::common_server_errc>(code);
    } catch (...) {
        return false;
    }
}

// 线性退避
awaitable<void> backoff(unsigned attempt) {
    namespace asio = boost::asio;
    asio::steady_timer timer(co_await asio::this_coro::executor);

    // 指数退避 + 上限
    auto base = std::min<unsigned>(25u << std::min(attempt, 6u), 1000u); // cap 1s
    // 加抖动，避免惊群
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<unsigned> dist(base / 2, base);
    timer.expires_after(std::chrono::milliseconds(dist(rng)));

    co_await timer.async_wait(use_awaitable);
}

//查 request_id
//  ├─ 没有记录 → nullopt
//  └─ 有记录
//       ├─ 校验输入是否一致（不一致抛 IDEMPOTENCY_CONFLICT）
//       ├─ user_id/account 为空 → nullopt（未完成）
//       └─ 返回之前的 Result（幂等命中，直接用缓存结果）
awaitable<std::optional<Result>> lookup(boost::mysql::connection_pool& pool, const Input& in) {
    auto conn = co_await pool.async_get_connection(use_awaitable);
    results r;
    co_await conn->async_execute(boost::mysql::format_sql(conn->format_opts().value(),
        "SELECT payload_salt,payload_hash,user_id,account FROM registration_requests WHERE request_id={}",
        in.request_id), r, use_awaitable);
    if (r.rows().empty()) {
        spdlog::debug("lookup: no row for request_id");
        co_return std::nullopt;
    }
    spdlog::debug("lookup: row found, cols={}", r.rows()[0].size());
    auto row = r.rows()[0];
    verify(in, row);
    if (row[2].is_null() || row[3].is_null()) {
        spdlog::debug("lookup: user_id/account NULL, treat as incomplete");
        co_return std::nullopt;
    }
    co_return Result{number(row[2], "lookup.user_id"), number(row[3], "lookup.account")};
}

/*

for attempt in 0..2:                       // 最多 3 次尝试
    借连接 → begin() 开事务
    try:
        INSERT 幂等记录（ON DUPLICATE KEY 序列化同 ID 并发）
        SELECT ... FOR UPDATE 拿回该行
        verify() 校验输入一致
        若已有 user_id/account → 回滚 + 返回 replay（幂等命中）
        检查 phone 是否已存在 → 抛 PHONE_EXISTS
        从 account_pool 取一个空闲账号 FOR UPDATE
        若池空 → 回滚 + 返回 nullopt
        INSERT users
        UPDATE account_pool 标记占用
        UPDATE registration_requests 填 user_id/account
        COMMIT
    catch: 记录 failure
    回滚（未提交才需要）
    归还连接
    如果 commit 阶段失败 → 用 lookup 确认，再决定返回/抛 UNCERTAIN
    如果是锁超时/死锁且已回滚 → backoff 后重试
    如果是 1062 唯一键冲突 → 区分是 phone 还是 pool 损坏
    否则重抛

*/
awaitable<std::optional<Result>> once(boost::mysql::connection_pool& pool, const Input& in) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        std::exception_ptr failure;
        bool committing = false, rolledBack = false;
        spdlog::debug("once: attempt {} start", attempt);
        {
            auto conn = co_await pool.async_get_connection(use_awaitable);
            results r;
            co_await begin(*conn.operator->());
            try {
                // Duplicate-key update serializes concurrent requests with the SAME ID.
                // Pending rows never commit: this row and the user commit together.
                co_await conn->async_execute(boost::mysql::format_sql(conn->format_opts().value(),
                    "INSERT INTO registration_requests(request_id,payload_salt,payload_hash) VALUES ({},{},{}) "
                    "ON DUPLICATE KEY UPDATE request_id=registration_requests.request_id",
                    in.request_id, in.user.password_salt, digest(in.user.password_salt, in.payload)), r, use_awaitable);
                co_await conn->async_execute(boost::mysql::format_sql(conn->format_opts().value(),
                    "SELECT payload_salt,payload_hash,user_id,account FROM registration_requests "
                    "WHERE request_id={} FOR UPDATE", in.request_id), r, use_awaitable);
                if (r.rows().empty()) {
                    spdlog::error("once: SELECT FOR UPDATE returned no row after INSERT");
                    throw Error("SCHEMA_ERROR", "registration_requests row missing after insert");
                }
                auto row = r.rows()[0];
                spdlog::debug("once: row cols={} user_id_null={} account_null={}",
                              row.size(), row[2].is_null(), row[3].is_null());
                verify(in, row);
                if (!row[2].is_null() && !row[3].is_null()) {
                    Result replay{number(row[2], "once.replay.user_id"),
                                  number(row[3], "once.replay.account")};
                    if (!co_await rollback(*conn.operator->())) throw Error("DB_ERROR", "Unable to release replay transaction");
                    spdlog::debug("once: idempotent replay hit");
                    co_return replay;
                }
                // Clear business error before touching the pool. UNIQUE(phone) also
                // arbitrates simultaneous registrations with different request IDs.
                co_await conn->async_execute(boost::mysql::format_sql(conn->format_opts().value(),
                    "SELECT user_id FROM users WHERE phone={} LIMIT 1", in.user.phone), r, use_awaitable);
                if (!r.rows().empty()) throw Error("PHONE_EXISTS", "Phone number is already registered");
                co_await conn->async_execute(
                    "SELECT id,account FROM account_pool WHERE status=0 ORDER BY id LIMIT 1 FOR UPDATE", r, use_awaitable);
                if (r.rows().empty()) {
                    if (!co_await rollback(*conn.operator->())) throw Error("DB_ERROR", "Unable to roll back empty-pool transaction");
                    spdlog::debug("once: account_pool empty, return nullopt to trigger refill");
                    co_return std::nullopt;
                }
                spdlog::debug("once: account_pool row cols={}", r.rows()[0].size());
                const auto id = number(r.rows()[0][0], "once.pool.id");
                const auto account = number(r.rows()[0][1], "once.pool.account");
                if (!id || account < AccountPoolAllocator::kMinAccount || account > AccountPoolAllocator::kMaxAccount)
                    throw Error("POOL_INCONSISTENT", "Invalid account pool record");
                co_await conn->async_execute(boost::mysql::format_sql(conn->format_opts().value(),
                    "INSERT INTO users(account,nickname,phone,password_hash,password_salt) VALUES ({},{},{},{},{})",
                    account, in.user.nickname, in.user.phone, in.user.password_hash, in.user.password_salt), r, use_awaitable);
                const auto uid = r.last_insert_id();
                if (!uid) throw Error("SCHEMA_ERROR", "Missing inserted user ID");
                co_await conn->async_execute(boost::mysql::format_sql(conn->format_opts().value(),
                    "UPDATE account_pool SET status=1 WHERE id={} AND status=0", id), r, use_awaitable);
                if (r.affected_rows() != 1) throw Error("POOL_INCONSISTENT", "Account allocation did not update one row");
                co_await conn->async_execute(boost::mysql::format_sql(conn->format_opts().value(),
                    "UPDATE registration_requests SET user_id={},account={} WHERE request_id={}",
                    uid, account, in.request_id), r, use_awaitable);
                if (r.affected_rows() != 1) throw Error("SCHEMA_ERROR", "Registration result was not persisted");
                committing = true;
                co_await conn->async_execute("COMMIT", r, use_awaitable);
                spdlog::debug("once: committed user_id={} account={}", uid, account);
                co_return Result{uid, account};
            } catch (...) { failure = std::current_exception(); }
            rolledBack = co_await rollback(*conn.operator->());
            if (!rolledBack) spdlog::error("once: rollback failed");
        } // release original connection BEFORE querying/retrying/refilling
        if (committing) {
            std::optional<Result> confirmed;
            try { confirmed = co_await lookup(pool, in); } catch (...) {}
            if (confirmed) co_return confirmed;
            throw Error("REGISTRATION_UNCERTAIN", "Retry the same request ID to confirm registration");
        }
        const bool transient = mysqlCode(failure, 1205) || mysqlCode(failure, 1213);
        if (transient && rolledBack) {
            spdlog::warn("once: transient error (1205/1213), attempt {}", attempt);
            if (attempt < 2) { co_await backoff(attempt); continue; }
            throw Error("REGISTRATION_BUSY", "Registration is busy; retry the same request ID");
        }
        if (mysqlCode(failure, 1062)) {
            spdlog::warn("once: 1062 duplicate key, checking phone vs pool");
            // Determine whether the UNIQUE conflict was phone or a corrupt pool.
            auto conn = co_await pool.async_get_connection(use_awaitable);
            results r;
            co_await conn->async_execute(boost::mysql::format_sql(conn->format_opts().value(),
                "SELECT user_id FROM users WHERE phone={} LIMIT 1", in.user.phone), r, use_awaitable);
            if (!r.rows().empty()) throw Error("PHONE_EXISTS", "Phone number is already registered");
            throw Error("POOL_INCONSISTENT", "Account uniqueness conflict; inspect account pool");
        }
        std::rethrow_exception(failure);
    }
    throw Error("INTERNAL_ERROR", "Registration attempts exhausted");
}
} // namespace

std::string payload(const std::string& a, const std::string& b, const std::string& c) {
    return std::to_string(a.size()) + ":" + a + std::to_string(b.size()) + ":" + b
        + std::to_string(c.size()) + ":" + c;
}

awaitable<void> refill(boost::mysql::connection_pool& pool, uint64_t batchSize) {
    if (!batchSize || batchSize > AccountPoolAllocator::kRefillBatch)
        throw Error("REFILL_INVALID_PARAM", "Invalid account refill batch size");
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        std::exception_ptr failure;
        bool committing = false, rolledBack = false;
        spdlog::debug("refill: attempt {} start, batchSize={}", attempt, batchSize);
        {
            auto conn = co_await pool.async_get_connection(use_awaitable);
            results r;
            co_await begin(*conn.operator->());
            try {
                co_await conn->async_execute("SELECT next_start FROM account_pool_cursor WHERE id=1 FOR UPDATE", r, use_awaitable);
                if (r.rows().size() != 1) throw Error("POOL_INCONSISTENT", "Missing account pool cursor");
                spdlog::debug("refill: cursor row cols={} next_start_null={}",
                              r.rows()[0].size(), r.rows()[0][0].is_null());
                const auto start = number(r.rows()[0][0], "refill.cursor.next_start");
                spdlog::debug("refill: next_start={}", start);

                co_await conn->async_execute("SELECT COUNT(*) FROM account_pool WHERE status=0", r, use_awaitable);
                spdlog::debug("refill: count row cols={} count_null={}",
                              r.rows()[0].size(), r.rows()[0][0].is_null());
                const auto freeCount = number(r.rows()[0][0], "refill.count_free");
                spdlog::debug("refill: free_count={} low_water_mark={}",
                              freeCount, AccountPoolAllocator::kLowWaterMark);
                if (freeCount >= AccountPoolAllocator::kLowWaterMark) {
                    if (!co_await rollback(*conn.operator->())) throw Error("DB_ERROR", "Unable to release refill transaction");
                    spdlog::debug("refill: free count sufficient, skip");
                    co_return;
                }
                // Fail closed. Never silently reset/advance an existing cursor.
                co_await conn->async_execute(
                    "SELECT MAX(account) FROM ("
                        "SELECT MAX(account) AS account FROM users "
                        "UNION ALL "
                        "SELECT MAX(account) AS account FROM account_pool"
                        ") t", r, use_awaitable);
                spdlog::debug("refill: greatest row cols={} max_null={}",
                              r.rows()[0].size(), r.rows()[0][0].is_null());
                const auto maximum = number(r.rows()[0][0], "refill.greatest_max");
                spdlog::debug("refill: maximum={}", maximum);
                if (start < AccountPoolAllocator::kMinAccount || start <= maximum)
                    throw Error("POOL_INCONSISTENT", "Cursor overlaps existing accounts; run migration preflight");
                if (start > AccountPoolAllocator::kMaxAccount)
                    throw Error("ACCOUNT_RANGE_EXHAUSTED", "Account number range exhausted");
                const auto count = std::min(batchSize, AccountPoolAllocator::kMaxAccount - start + 1);
                const auto end = start + count;
                spdlog::debug("refill: generating accounts [{}, {}) count={}", start, end, count);
                co_await conn->async_execute(boost::mysql::format_sql(conn->format_opts().value(),
                    "UPDATE account_pool_cursor SET next_start={} WHERE id=1", end), r, use_awaitable);
                const auto accounts = AccountPoolAllocator::generate_shuffled_batch(start, end);
                for (size_t i = 0; i < accounts.size(); i += 1000) {
                    std::string sql = "INSERT INTO account_pool(account,status) VALUES ";
                    for (size_t j = i; j < std::min(i + 1000, accounts.size()); ++j) {
                        if (j != i) sql += ',';
                        sql += '(' + std::to_string(accounts[j]) + ",0)";
                    }
                    co_await conn->async_execute(sql, r, use_awaitable);
                }
                committing = true;
                co_await conn->async_execute("COMMIT", r, use_awaitable);
                spdlog::debug("refill: committed, inserted {} accounts", accounts.size());
                co_return;
            } catch (...) { failure = std::current_exception(); }
            rolledBack = co_await rollback(*conn.operator->());
            if (!rolledBack) spdlog::error("refill: rollback failed");
        }
        if (committing) throw Error("REFILL_UNCERTAIN", "Refill outcome unknown; retry registration with the same request ID");
        if (rolledBack && (mysqlCode(failure,1205) || mysqlCode(failure,1213))) {
            spdlog::warn("refill: transient error (1205/1213), attempt {}", attempt);
            if (attempt < 2) { co_await backoff(attempt); continue; }
            throw Error("REGISTRATION_BUSY", "Account refill is busy");
        }
        std::rethrow_exception(failure);
    }
}

awaitable<Result> create(boost::mysql::connection_pool& pool, const Input& input) {
    spdlog::debug("create: calling once()");
    auto registered = co_await once(pool, input);
    if (!registered) {
        spdlog::debug("create: pool empty, calling refill()");
        co_await refill(pool, AccountPoolAllocator::kRefillBatch);
        spdlog::debug("create: refill done, retrying once()");
        registered = co_await once(pool, input);
    }
    if (!registered) throw Error("ACCOUNT_POOL_UNAVAILABLE", "No account available; retry the same request ID");
    spdlog::debug("create: success");
    co_return *registered;
}
} // namespace registration