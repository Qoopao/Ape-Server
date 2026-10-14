#include "storage/mysqlhandler.h"
#include "messagequeue/message_producer.h"
#include "util/uuid.h"
#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>
#include "storage/mysqlconnector.h"
#include "storage/mongohandler.h"
#include "util/snowflake.h"
#include "storage/redishandler.h"
#include <boost/asio/co_spawn.hpp>
#include "sdkws.pb.h"
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql/error_with_diagnostics.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/with_params.hpp>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <spdlog/spdlog.h>
#include "util/account_pool.h"
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/mysql/format_sql.hpp>
#include <boost/mysql/common_server_errc.hpp>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <atomic>
#include <stdexcept>
#include <unordered_set>

// ═══════════════════════════════════════════════════════════════════════════
// 辅助：将 results.rows() 的第一行解析为 userInfo
// 字段顺序：user_id, account, nickname, password_hash, password_salt
// ═══════════════════════════════════════════════════════════════════════════
namespace {

uint64_t unsigned_field(boost::mysql::field_view value, const char *column)
{
    if (value.is_uint64())
        return value.as_uint64();
    if (value.is_int64() && value.as_int64() >= 0)
        return static_cast<uint64_t>(value.as_int64());
    throw std::runtime_error(std::string("Invalid MySQL field ") + column
        + ": expected a nonnegative integer, kind="
        + std::to_string(static_cast<int>(value.kind())));
}

std::string string_field(boost::mysql::field_view value, const char *column)
{
    if (value.is_string())
        return std::string(value.as_string());
    throw std::runtime_error(std::string("Invalid MySQL field ") + column
        + ": expected a string, kind=" + std::to_string(static_cast<int>(value.kind())));
}

boost::mysql::datetime datetime_field(boost::mysql::field_view value, const char *column)
{
    if (value.is_datetime())
        return value.as_datetime();
    throw std::runtime_error(std::string("Invalid MySQL field ") + column
        + ": expected datetime, kind=" + std::to_string(static_cast<int>(value.kind())));
}

userInfo parse_user_from_row(const boost::mysql::rows_view &rows)
{
    auto row = rows.at(0);
    userInfo user{};

    user.user_id       = unsigned_field(row.at(0), "users.user_id");
    user.account       = unsigned_field(row.at(1), "users.account");
    user.nickname      = string_field(row.at(2), "users.nickname");
    if (row.size() == 3)
        return user;

    user.phone         = string_field(row.at(3), "users.phone");
    user.password_hash = string_field(row.at(4), "users.password_hash");
    user.password_salt = string_field(row.at(5), "users.password_salt");

    // created_at → 第 7 列 (index 6)
    if (row.size() > 6 && !row.at(6).is_null())
    {
        auto dt = datetime_field(row.at(6), "users.created_at");
        std::ostringstream oss;
        oss << static_cast<int>(dt.year()) << '-'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.month()) << '-'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.day()) << ' '
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.hour()) << ':'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.minute()) << ':'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.second());
        user.created_at = oss.str();
    }

    // updated_at → 第 8 列 (index 7)
    if (row.size() > 7 && !row.at(7).is_null())
    {
        auto dt = datetime_field(row.at(7), "users.updated_at");
        std::ostringstream oss;
        oss << static_cast<int>(dt.year()) << '-'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.month()) << '-'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.day()) << ' '
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.hour()) << ':'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.minute()) << ':'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.second());
        user.updated_at = oss.str();
    }

    // last_login_at → 第 9 列 (index 8)，可为 NULL
    if (row.size() > 8 && !row.at(8).is_null())
    {
        auto dt = datetime_field(row.at(8), "users.last_login_at");
        std::ostringstream oss;
        oss << static_cast<int>(dt.year()) << '-'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.month()) << '-'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.day()) << ' '
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.hour()) << ':'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.minute()) << ':'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.second());
        user.last_login_at = oss.str();
    }

    return user;
}

auto user_query(uint64_t value, bool by_account, bool public_only = false)
{
    if (public_only)
    {
        return by_account
            ? boost::mysql::with_params("SELECT user_id, account, nickname FROM users WHERE account = {}", value)
            : boost::mysql::with_params("SELECT user_id, account, nickname FROM users WHERE user_id = {}", value);
    }
    return by_account
        ? boost::mysql::with_params(
            "SELECT user_id, account, nickname, phone, password_hash, password_salt, "
            "created_at, updated_at, last_login_at FROM users WHERE account = {}", value)
        : boost::mysql::with_params(
            "SELECT user_id, account, nickname, phone, password_hash, password_salt, "
            "created_at, updated_at, last_login_at FROM users WHERE user_id = {}", value);
}

boost::asio::awaitable<std::optional<userInfo>> find_user(
    boost::mysql::any_connection &connection, uint64_t value, bool by_account, bool public_only = false)
{
    auto stmt = user_query(value, by_account, public_only);
    boost::mysql::results result;
    co_await connection.async_execute(stmt, result, boost::asio::use_awaitable);
    if (result.rows().empty())
        co_return std::nullopt;
    co_return parse_user_from_row(result.rows());
}

} // namespace

// ── 插入用户 ──
boost::asio::awaitable<std::optional<uint64_t>>
MySQLHandler::insert_user(const userInfo &user)
{
    try
    {
        // account 暂为 0（表里 DEFAULT 0），等取号后再 UPDATE
        // phone 必填，从 user.phone 传入
        auto &connector = MySQLConnector::instance();
        co_await boost::asio::dispatch(boost::asio::bind_executor(
            connector.get_pool_executor(0), boost::asio::use_awaitable));
        auto connection = co_await connector.get_pool(0).async_get_connection(boost::asio::use_awaitable);
        const auto id = co_await insert_user(*connection.operator->(), user, 0);
        spdlog::info("[MySQL] Inserted user: user_id={}", id);
        co_return id;
    }
    catch (const std::exception &e)
    {
        spdlog::error("[MySQL] insert_user failed: {}", e.what());
        co_return std::nullopt;
    }
}

boost::asio::awaitable<bool>
MySQLHandler::update_account(uint64_t user_id, uint64_t account)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "UPDATE users SET account = {} WHERE user_id = {}",
            account, user_id);

        co_await MySQLConnector::instance().async_execute(stmt);

        // async_execute 返回受影响行数，为 1 表示更新成功
        co_return true;
    }
    catch (const std::exception &e)
    {
        spdlog::error(
            "update_account failed: user_id={}, account={}, err={}",
            user_id, account, e.what());
        co_return false;
    }
}

// ── 通过账号查找用户 ──

boost::asio::awaitable<std::optional<userInfo>>
MySQLHandler::find_user_by_account(const uint64_t account)
{
    try
    {
        const auto result = co_await MySQLConnector::instance().async_execute(user_query(account, true));
        if (result.rows().empty())
            co_return std::nullopt;
        co_return parse_user_from_row(result.rows());
    }
    catch (const std::exception &e)
    {
        spdlog::error("Find user by account failed: {}", e.what());
        co_return std::nullopt;
    }
}

// ── 通过 user_id 查找用户 ──

boost::asio::awaitable<std::optional<userInfo>>
MySQLHandler::find_user_by_id(const uint64_t user_id)
{
    try
    {
        const auto result = co_await MySQLConnector::instance().async_execute(user_query(user_id, false));
        if (result.rows().empty())
            co_return std::nullopt;
        co_return parse_user_from_row(result.rows());
    }
    catch (const std::exception &e)
    {
        spdlog::error("Find user by id failed: {}", e.what());
        co_return std::nullopt;
    }
}

// ── 更新最后登录时间 ──

boost::asio::awaitable<bool>
MySQLHandler::update_last_login(const uint64_t user_id)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "UPDATE users SET last_login_at = NOW() WHERE user_id = {}", user_id);

        co_await MySQLConnector::instance().async_execute(stmt);
        co_return true;
    }
    catch (const std::exception &e)
    {
        spdlog::error("Update last_login failed: {}", e.what());
        co_return false;
    }
}

// ── 检查用户名是否已存在 ──

boost::asio::awaitable<bool>
MySQLHandler::account_exists(const uint64_t account)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "SELECT COUNT(*) FROM users WHERE account = {}", account);

        boost::mysql::results result =
            co_await MySQLConnector::instance().async_execute(stmt);
        auto rows = result.rows();
        if (!rows.empty())
        {
            co_return rows.at(0).at(0).as_int64() > 0;
        }
        co_return false;
    }
    catch (const std::exception &e)
    {
        spdlog::error("Check account exists failed: {}", e.what());
        co_return false;
    }
}

boost::asio::awaitable<std::optional<userInfo>>
MySQLHandler::find_user_by_account(boost::mysql::any_connection &connection, uint64_t account)
{
    co_return co_await find_user(connection, account, true);
}

boost::asio::awaitable<uint64_t>
MySQLHandler::insert_user(boost::mysql::any_connection &connection, const userInfo &user, uint64_t account)
{
    boost::mysql::results result;
    co_await connection.async_execute(boost::mysql::with_params(
        "INSERT INTO users(account,nickname,phone,password_hash,password_salt) VALUES ({},{},{},{},{})",
        account, user.nickname, user.phone, user.password_hash, user.password_salt),
        result, boost::asio::use_awaitable);
    co_return result.last_insert_id();
}

namespace mysql_registration {
using Error = MySQLHandler::RegistrationError;
using Input = MySQLHandler::RegistrationInput;
using Result = MySQLHandler::RegistrationResult;
std::string payload(const std::string&, const std::string&, const std::string&);
boost::asio::awaitable<void> refill(boost::mysql::connection_pool&, uint64_t);
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
                const auto uid = co_await MySQLHandler::insert_user(*conn.operator->(), in.user, account);
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
} // namespace mysql_registration
std::string MySQLHandler::registration_payload(const std::string &phone,
    const std::string &nickname, const std::string &password)
{
    return mysql_registration::payload(phone, nickname, password);
}

boost::asio::awaitable<MySQLHandler::RegistrationResult>
MySQLHandler::register_user(boost::mysql::connection_pool &pool, const RegistrationInput &input)
{
    co_return co_await mysql_registration::create(pool, input);
}

boost::asio::awaitable<void>
MySQLHandler::refill_accounts(boost::mysql::connection_pool &pool, uint64_t batch_size)
{
    co_await mysql_registration::refill(pool, batch_size);
}

namespace {

using boost::asio::awaitable;
using boost::asio::use_awaitable;
using boost::mysql::results;
using boost::mysql::any_connection;

class FriendError : public std::runtime_error
{
public:
    FriendError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(code)) {}
    std::string code;
};

uint64_t number(boost::mysql::field_view value, const char *column = "friend record integer")
{
    return unsigned_field(value, column);
}

int64_t now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

awaitable<void> initialize(any_connection& connection)
{
    static std::atomic<bool> ready{false};
    if (ready.load())
        co_return;
    results r;
    co_await connection.async_execute(
        "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
        "AND table_name IN ('friend_pairs','friend_applications','friends','friend_chat_deletions')", r, use_awaitable);
    if (number(r.rows()[0][0]) == 4) {
        ready.store(true);
        co_return;
    }
    co_await connection.async_execute(R"sql(CREATE TABLE IF NOT EXISTS friend_pairs (
    low_account BIGINT UNSIGNED NOT NULL,
    high_account BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (low_account, high_account)
) ENGINE=InnoDB)sql", r, use_awaitable);
    co_await connection.async_execute(R"sql(CREATE TABLE IF NOT EXISTS friend_applications (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    from_account BIGINT UNSIGNED NOT NULL,
    to_account BIGINT UNSIGNED NOT NULL,
    message VARCHAR(512) NOT NULL DEFAULT '',
    status TINYINT NOT NULL DEFAULT 0,
    created_time BIGINT NOT NULL,
    handled_time BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (id),
    KEY incoming (to_account, id),
    KEY outgoing (from_account, id),
    KEY pending (from_account, to_account, status)
) ENGINE=InnoDB)sql", r, use_awaitable);
    co_await connection.async_execute(R"sql(CREATE TABLE IF NOT EXISTS friends (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    owner_account BIGINT UNSIGNED NOT NULL,
    friend_account BIGINT UNSIGNED NOT NULL,
    remark VARCHAR(128) NOT NULL DEFAULT '',
    created_time BIGINT NOT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY relationship (owner_account, friend_account),
    KEY owner_list (owner_account, id)
) ENGINE=InnoDB)sql", r, use_awaitable);
    co_await connection.async_execute(R"sql(CREATE TABLE IF NOT EXISTS friend_chat_deletions (
    owner_account BIGINT UNSIGNED NOT NULL,
    peer_account BIGINT UNSIGNED NOT NULL,
    deleted_time BIGINT NOT NULL,
    deleted_seq BIGINT NOT NULL,
    PRIMARY KEY (owner_account, peer_account)
) ENGINE=InnoDB)sql", r, use_awaitable);
    ready.store(true);
}

awaitable<void> clearedConversations(any_connection &connection, uint64_t owner, friendpb::Reply &reply)
{
    results r;
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "SELECT peer_account,deleted_time,deleted_seq FROM friend_chat_deletions WHERE owner_account={}", owner), r, use_awaitable);
    for (const auto row : r.rows()) {
        auto *cleared = reply.add_clearedconversations();
        cleared->set_peer(unsigned_field(row[0], "friend_chat_deletions.peer_account"));
        cleared->set_deletedtime(unsigned_field(row[1], "friend_chat_deletions.deleted_time"));
        cleared->set_deletedseq(unsigned_field(row[2], "friend_chat_deletions.deleted_seq"));
    }
}

awaitable<sdkws::PublicUserInfo> user(any_connection& connection, uint64_t account)
{
    const auto found = co_await find_user(connection, account, true, true);
    if (!found)
        throw FriendError("USER_NOT_FOUND", "User does not exist");
    sdkws::PublicUserInfo result;
    result.set_userid(found->account);
    result.set_nickname(found->nickname);
    co_return result;
}

awaitable<void> lockPair(any_connection& connection, uint64_t a, uint64_t b)
{
    // 取得好友关系行的排他锁
    results r;
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "INSERT INTO friend_pairs(low_account,high_account) VALUES ({},{}) "
        "ON DUPLICATE KEY UPDATE low_account=friend_pairs.low_account", std::min(a,b), std::max(a,b)), r, use_awaitable);
}

awaitable<friendpb::Application> application(any_connection& connection, uint64_t id)
{
    results r;
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "SELECT id,from_account,to_account,message,status,created_time,handled_time "
        "FROM friend_applications WHERE id={}", id), r, use_awaitable);
    if (r.rows().empty())
        throw FriendError("APPLICATION_NOT_FOUND", "Application does not exist");
    const auto row = r.rows()[0];
    friendpb::Application result;
    result.set_id(number(row[0]));
    result.mutable_sender()->CopyFrom(co_await user(connection, number(row[1])));
    result.mutable_recipient()->CopyFrom(co_await user(connection, number(row[2])));
    result.set_message(std::string(row[3].as_string()));
    result.set_status(static_cast<friendpb::ApplicationStatus>(number(row[4])));
    result.set_createdtime(number(row[5]));
    result.set_handledtime(number(row[6]));
    co_return result;
}

awaitable<void> mutate(any_connection& connection, const friendpb::Command& command, friendpb::Reply& reply)
{
    results r;
    uint64_t target = command.target();
    if (command.operation() == friendpb::HandleApplication) {
        auto existing = co_await application(connection, command.applicationid());
        if (existing.recipient().userid() != command.actor())
            throw FriendError("FORBIDDEN", "Only the recipient may handle this application");
        target = existing.sender().userid();
    }
    if (!target || target == command.actor())
        throw FriendError("INVALID_TARGET", "Cannot add yourself");
    co_await user(connection, target);
    co_await connection.async_execute("SET TRANSACTION ISOLATION LEVEL READ COMMITTED", r, use_awaitable);
    co_await connection.async_execute("START TRANSACTION", r, use_awaitable);
    std::exception_ptr failure;
    try {
        co_await lockPair(connection, command.actor(), target);
        if (command.operation() == friendpb::Apply) {
            co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
                "SELECT id FROM friends WHERE owner_account={} AND friend_account={}", command.actor(), target), r, use_awaitable);
            if (!r.rows().empty())
                throw FriendError("ALREADY_FRIEND", "Already friends");
            co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
                "SELECT id FROM friend_applications WHERE from_account={} AND to_account={} AND status=0 LIMIT 1",
                command.actor(), target), r, use_awaitable);
            uint64_t id = 0;
            if (!r.rows().empty()) {
                id = number(r.rows()[0][0]);
            } else {
                co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
                    "INSERT INTO friend_applications(from_account,to_account,message,created_time) VALUES ({},{},{},{})",
                    command.actor(), target, command.message(), now()), r, use_awaitable);
                id = r.last_insert_id();
            }
            reply.add_applications()->CopyFrom(co_await application(connection, id));
        } else if (command.operation() == friendpb::DeleteFriend) {
            co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
                "DELETE FROM friends WHERE (owner_account={} AND friend_account={}) "
                "OR (owner_account={} AND friend_account={})",
                command.actor(), target, target, command.actor()), r, use_awaitable);
            if (r.affected_rows() > 0) {
                const auto deletedTime = now();
                const auto deletedSeq = Snowflake::instance().nextId();
                co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
                    "INSERT INTO friend_chat_deletions(owner_account,peer_account,deleted_time,deleted_seq) VALUES ({},{},{},{}),({},{},{},{}) "
                    "ON DUPLICATE KEY UPDATE deleted_time=VALUES(deleted_time),deleted_seq=VALUES(deleted_seq)",
                    command.actor(), target, deletedTime, deletedSeq, target, command.actor(), deletedTime, deletedSeq), r, use_awaitable);
                co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
                    "UPDATE friend_applications SET status=2,handled_time={} WHERE status=0 AND "
                    "((from_account={} AND to_account={}) OR (from_account={} AND to_account={}))",
                    now(), command.actor(), target, target, command.actor()), r, use_awaitable);
            }
            co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
                "SELECT deleted_seq FROM friend_chat_deletions WHERE owner_account={} AND peer_account={}",
                command.actor(), target), r, use_awaitable);
            if (!r.rows().empty()) {
                const auto conversation = std::to_string(std::min(command.actor(), target)) + "_" +
                    std::to_string(std::max(command.actor(), target));
                const auto cutoff = static_cast<int64_t>(number(r.rows()[0][0]));
                co_await MySQLHandler::DeleteConversationPair(connection, conversation);
                co_await MySQLHandler::QueueConversationCleanup(connection, command.actor(), target, cutoff);
            }
        } else {
            co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
                "SELECT status FROM friend_applications WHERE id={} FOR UPDATE", command.applicationid()), r, use_awaitable);
            if (r.rows().empty())
                throw FriendError("APPLICATION_NOT_FOUND", "Application does not exist");
            const auto status = static_cast<friendpb::ApplicationStatus>(number(r.rows()[0][0]));
            const auto wanted = command.accept() ? friendpb::Accepted : friendpb::Rejected;
            if (status != friendpb::Pending && status != wanted)
                throw FriendError("ALREADY_HANDLED", "Application was already handled differently");
            if (status == friendpb::Pending) {
                const auto time = now();
                co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
                    "UPDATE friend_applications SET status={},handled_time={} WHERE id={} AND status=0",
                    static_cast<int>(wanted), time, command.applicationid()), r, use_awaitable);
                if (command.accept()) {
                    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
                        "INSERT INTO friends(owner_account,friend_account,created_time) VALUES ({},{},{}),({},{},{}) "
                        "ON DUPLICATE KEY UPDATE owner_account=friends.owner_account",
                        command.actor(), target, time, target, command.actor(), time), r, use_awaitable);
                }
            }
            reply.add_applications()->CopyFrom(co_await application(connection, command.applicationid()));
        }
        co_await connection.async_execute("COMMIT", r, use_awaitable);
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) {
        boost::system::error_code ec;
        co_await connection.async_execute("ROLLBACK", r, boost::asio::redirect_error(use_awaitable, ec));
        reply.clear_applications();
        std::rethrow_exception(failure);
    }
    reply.add_changedaccounts(command.actor());
    reply.add_changedaccounts(target);
}

}

awaitable<friendpb::Reply> MySQLHandler::ExecuteFriendCommand(const friendpb::Command& command)
{
    friendpb::Reply reply;
    try {
        if (!command.actor() || command.operation() < friendpb::LookupUser || command.operation() > friendpb::DeleteFriend)
            throw FriendError("INVALID_PARAM", "Invalid friend operation");
        if (command.message().size() > 512)
            throw FriendError("INVALID_PARAM", "Application message is too long");
        auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
        co_await initialize(*connection.operator->());
        co_await user(*connection.operator->(), command.actor());
        results r;
        if (command.operation() == friendpb::LookupUser) {
            if (!command.target())
                throw FriendError("INVALID_TARGET", "Invalid account");
            reply.mutable_user()->CopyFrom(co_await user(*connection.operator->(), command.target()));
        } else if (command.operation() == friendpb::Apply || command.operation() == friendpb::HandleApplication
                   || command.operation() == friendpb::DeleteFriend) {
            co_await mutate(*connection.operator->(), command, reply);
        } else {
            const auto limit = std::clamp<uint32_t>(command.limit(), 1, 100);
            uint64_t snapshot = command.snapshot();
            if (!snapshot) {
                if (command.operation() == friendpb::ListFriends)
                    co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(), "SELECT CAST(COALESCE(MAX(id),0) AS UNSIGNED) FROM friends WHERE owner_account={}", command.actor()), r, use_awaitable);
                else
                    co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(), "SELECT CAST(COALESCE(MAX(id),0) AS UNSIGNED) FROM friend_applications WHERE from_account={} OR to_account={}", command.actor(), command.actor()), r, use_awaitable);
                snapshot = number(r.rows()[0][0], "friend list snapshot");
            }
            reply.set_snapshot(snapshot);
            if (command.operation() == friendpb::ListFriends) {
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "SELECT f.id,f.friend_account,f.remark,f.created_time,u.nickname FROM friends f "
                    "JOIN users u ON u.account=f.friend_account WHERE f.owner_account={} AND f.id>{} AND f.id<={} ORDER BY f.id LIMIT {}",
                    command.actor(), command.after(), snapshot, limit + 1), r, use_awaitable);
                const auto rows = r.rows();
                reply.set_hasmore(rows.size() > limit);
                for (size_t i = 0; i < std::min<size_t>(rows.size(), limit); ++i) {
                    auto* f = reply.add_friends();
                    f->set_owneruserid(command.actor()); f->set_remark(std::string(rows[i][2].as_string()));
                    f->set_createtime(number(rows[i][3], "friends.created_time")); f->mutable_frienduser()->set_userid(number(rows[i][1], "friends.friend_account"));
                    f->mutable_frienduser()->set_nickname(std::string(rows[i][4].as_string()));
                    reply.set_cursor(number(rows[i][0], "friends.id"));
                }
            } else {
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "SELECT id FROM friend_applications WHERE (from_account={} OR to_account={}) AND id>{} AND id<={} ORDER BY id LIMIT {}",
                    command.actor(), command.actor(), command.after(), snapshot, limit + 1), r, use_awaitable);
                std::vector<uint64_t> ids;
                for (const auto row : r.rows()) ids.push_back(number(row[0], "friend_applications.id"));
                reply.set_hasmore(ids.size() > limit);
                for (size_t i = 0; i < std::min<size_t>(ids.size(), limit); ++i) {
                    reply.add_applications()->CopyFrom(co_await application(*connection.operator->(), ids[i]));
                    reply.set_cursor(ids[i]);
                }
            }
        }
        if (command.operation() == friendpb::ListFriends || command.operation() == friendpb::DeleteFriend)
            co_await clearedConversations(*connection.operator->(), command.actor(), reply);
        reply.set_errorcode("0");
    } catch (const FriendError& e) {
        reply.set_errorcode(e.code); reply.set_errormessage(e.what());
    } catch (const std::exception& e) {
        reply.set_errorcode("DB_ERROR"); reply.set_errormessage("Friend data is temporarily unavailable");
        spdlog::error("MySQLHandler friends: operation={}, actor={}, target={}, applicationID={}, error={}",
            static_cast<int>(command.operation()), command.actor(), command.target(),
            command.applicationid(), e.what());
    }
    co_return reply;
}

boost::asio::awaitable<bool> MySQLHandler::WithFriendMessage(const sdkws::MsgData &message,
    std::function<boost::asio::awaitable<void>()> action, bool requireUndelivered)
{
    auto task = [](sdkws::MsgData message, std::function<boost::asio::awaitable<void>()> action,
        bool requireUndelivered)
        -> boost::asio::awaitable<bool> {
        auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
        co_await initialize(*connection.operator->());
        results r;
        co_await connection->async_execute("SET TRANSACTION ISOLATION LEVEL READ COMMITTED", r, use_awaitable);
        co_await connection->async_execute("START TRANSACTION", r, use_awaitable);
        std::exception_ptr failure;
        bool allowed = false;
        try {
            co_await lockPair(*connection.operator->(), message.sendid(), message.recvid());
            co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                "SELECT COUNT(*) FROM friends WHERE (owner_account={} AND friend_account={}) "
                "OR (owner_account={} AND friend_account={})",
                message.sendid(), message.recvid(), message.recvid(), message.sendid()), r, use_awaitable);
            allowed = message.sendid() != message.recvid() && number(r.rows()[0][0]) == 2;
            if (allowed) {
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "SELECT deleted_seq FROM friend_chat_deletions WHERE owner_account={} AND peer_account={}",
                    message.sendid(), message.recvid()), r, use_awaitable);
                allowed = r.rows().empty() || message.seq() > number(r.rows()[0][0]);
            }
            if (allowed && !message.servermsgid().empty()) {
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "SELECT stage,state FROM im_outbox WHERE operation=1 AND server_msg_id={}", message.servermsgid()),
                    r, use_awaitable);
                allowed = r.rows().empty() || (number(r.rows()[0][0]) >= 1 && number(r.rows()[0][1]) != 3);
                if (allowed && requireUndelivered) {
                    co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                        "SELECT 1 FROM im_message_receipts WHERE owner_account={} AND server_msg_id={}",
                        message.recvid(), message.servermsgid()), r, use_awaitable);
                    allowed = r.rows().empty();
                }
            }
            if (allowed) {
                co_await action();
            }
            co_await connection->async_execute("COMMIT", r, use_awaitable);
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            boost::system::error_code ec;
            co_await connection->async_execute("ROLLBACK", r, boost::asio::redirect_error(use_awaitable, ec));
            std::rethrow_exception(failure);
        }
        co_return allowed;
    };
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        task(message, std::move(action), requireUndelivered), use_awaitable);
}

namespace {

uint64_t conversationNumber(boost::mysql::field_view value)
{
    return unsigned_field(value, "conversation integer");
}

std::string conversationPreview(const sdkws::MsgData& message)
{
    if (message.contenttype() != 101)
        return "[message type " + std::to_string(message.contenttype()) + "]";
    auto text = message.content();
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_object() && parsed.contains("content") && parsed["content"].is_string())
        text = parsed["content"].get<std::string>();
    size_t end = std::min<size_t>(text.size(), 512);
    while (end < text.size() && end > 0 &&
        (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
        --end;
    return nlohmann::json{{"content", text.substr(0, end)}}.dump();
}

conversation::Conversation conversationRow(boost::mysql::row_view row)
{
    conversation::Conversation item;
    item.set_owneruserid(conversationNumber(row[0]));
    item.set_conversationid(std::string(row[1].as_string()));
    item.set_conversationtype(1);
    item.set_userid(conversationNumber(row[2]));
    item.set_ispinned(conversationNumber(row[3]) != 0);
    item.set_recvmsgopt(static_cast<int32_t>(conversationNumber(row[4])));
    item.set_minseq(static_cast<int64_t>(conversationNumber(row[5])));
    item.set_maxseq(static_cast<int64_t>(conversationNumber(row[6])));
    item.set_readseq(static_cast<int64_t>(conversationNumber(row[7])));
    item.set_unreadcount(static_cast<int64_t>(conversationNumber(row[8])));
    const std::string latest(row[9].as_string());
    if (!latest.empty() && !google::protobuf::util::JsonStringToMessage(
            latest, item.mutable_latestmsg()).ok())
        throw std::runtime_error("Invalid stored conversation summary");
    return item;
}

const std::string conversationColumns =
    "owner_account,conversation_id,peer_account,is_pinned,recv_msg_opt,min_seq,max_seq,read_seq,unread_count,latest_message";

template <class Action>
boost::asio::awaitable<bool> conversationTransaction(boost::mysql::any_connection& connection, Action action)
{
    results r;
    co_await connection.async_execute("START TRANSACTION", r, use_awaitable);
    std::exception_ptr failure;
    bool result = false;
    try {
        result = co_await action();
        co_await connection.async_execute("COMMIT", r, use_awaitable);
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) {
        boost::system::error_code ec;
        co_await connection.async_execute("ROLLBACK", r, boost::asio::redirect_error(use_awaitable, ec));
        std::rethrow_exception(failure);
    }
    co_return result;
}

}

boost::asio::awaitable<void> MySQLHandler::InitializeConversations()
{
    auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    results r;
    co_await connection->async_execute(R"sql(CREATE TABLE IF NOT EXISTS im_conversations (
        owner_account BIGINT UNSIGNED NOT NULL,
        conversation_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        peer_account BIGINT UNSIGNED NOT NULL,
        is_pinned TINYINT UNSIGNED NOT NULL DEFAULT 0,
        recv_msg_opt TINYINT UNSIGNED NOT NULL DEFAULT 0,
        is_hidden TINYINT UNSIGNED NOT NULL DEFAULT 0,
        min_seq BIGINT NOT NULL DEFAULT 0,
        max_seq BIGINT NOT NULL DEFAULT 0,
        read_seq BIGINT NOT NULL DEFAULT 0,
        unread_count BIGINT UNSIGNED NOT NULL DEFAULT 0,
        latest_time BIGINT UNSIGNED NOT NULL DEFAULT 0,
        latest_message MEDIUMTEXT NOT NULL,
        PRIMARY KEY(owner_account,conversation_id),
        KEY conversation_pair(conversation_id),
        KEY conversation_order(owner_account,is_hidden,is_pinned,latest_time,conversation_id)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)sql", r, use_awaitable);
    co_await connection->async_execute(R"sql(CREATE TABLE IF NOT EXISTS im_conversation_unread (
        owner_account BIGINT UNSIGNED NOT NULL,
        conversation_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        seq BIGINT NOT NULL,
        PRIMARY KEY(owner_account,conversation_id,seq),
        CONSTRAINT conversation_unread_owner FOREIGN KEY(owner_account,conversation_id)
            REFERENCES im_conversations(owner_account,conversation_id) ON DELETE CASCADE
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)sql", r, use_awaitable);
}

boost::asio::awaitable<void> MySQLHandler::RecordConversationMessage(
    boost::mysql::any_connection& connection, const sdkws::MsgData& message)
{
    if (message.isgroupmsg() || message.seq() <= 0)
        throw std::invalid_argument("Invalid single-chat conversation message");
    conversation::MsgInfo summary;
    summary.set_servermsgid(message.servermsgid());
    summary.set_clientmsgid(message.clientmsgid());
    summary.set_sessiontype(1);
    summary.set_sendid(message.sendid());
    summary.set_recvid(message.recvid());
    summary.set_sendername(message.sendernickname());
    summary.set_latestmsgrecvtime(static_cast<int64_t>(message.sendtime()));
    summary.set_contenttype(message.contenttype());
    summary.set_content(conversationPreview(message));
    std::string json;
    if (!google::protobuf::util::MessageToJsonString(summary, &json).ok())
        throw std::runtime_error("Unable to serialize conversation summary");
    results r;
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "INSERT INTO im_conversations(owner_account,conversation_id,peer_account,max_seq,latest_time,latest_message) "
        "VALUES ({},{},{},{},{},{}),({},{},{},{},{},{}) "
        "ON DUPLICATE KEY UPDATE "
        "is_hidden=IF(VALUES(max_seq)>max_seq,0,is_hidden),"
        "latest_message=IF(VALUES(max_seq)>max_seq,VALUES(latest_message),latest_message),"
        "latest_time=IF(VALUES(max_seq)>max_seq,VALUES(latest_time),latest_time),"
        "max_seq=GREATEST(max_seq,VALUES(max_seq))",
        message.sendid(), message.convid(), message.recvid(), message.seq(),
        static_cast<uint64_t>(message.sendtime()), json,
        message.recvid(), message.convid(), message.sendid(), message.seq(),
        static_cast<uint64_t>(message.sendtime()), json), r, use_awaitable);
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "INSERT IGNORE INTO im_conversation_unread(owner_account,conversation_id,seq) "
        "SELECT owner_account,conversation_id,{} FROM im_conversations "
        "WHERE owner_account={} AND conversation_id={} AND read_seq<{} AND min_seq<{}",
        message.seq(), message.recvid(), message.convid(), message.seq(), message.seq()), r, use_awaitable);
    if (r.affected_rows())
        co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
            "UPDATE im_conversations SET unread_count=unread_count+1 WHERE owner_account={} AND conversation_id={}",
            message.recvid(), message.convid()), r, use_awaitable);
}

boost::asio::awaitable<void> MySQLHandler::DeleteConversationPair(
    boost::mysql::any_connection& connection, const std::string& id)
{
    results r;
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "DELETE FROM im_conversations WHERE conversation_id={}", id), r, use_awaitable);
}

boost::asio::awaitable<std::vector<conversation::Conversation>> MySQLHandler::ListConversations(uint64_t owner)
{
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(1),
        [owner]() -> boost::asio::awaitable<std::vector<conversation::Conversation>> {
            auto connection = co_await MySQLConnector::instance().get_pool(1).async_get_connection(use_awaitable);
            results r;
            const auto sql = "SELECT " + conversationColumns + " FROM im_conversations" +
                boost::mysql::format_sql(connection->format_opts().value(),
                    " WHERE owner_account={} AND is_hidden=0 ORDER BY is_pinned DESC,latest_time DESC,conversation_id DESC", owner);
            co_await connection->async_execute(sql, r, use_awaitable);
            std::vector<conversation::Conversation> items;
            items.reserve(r.rows().size());
            for (auto row : r.rows())
                items.push_back(conversationRow(row));
            co_return items;
        }, use_awaitable);
}

boost::asio::awaitable<std::optional<conversation::Conversation>> MySQLHandler::FindConversation(
    uint64_t owner, std::string id)
{
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(1),
        [owner, id = std::move(id)]() -> boost::asio::awaitable<std::optional<conversation::Conversation>> {
            auto connection = co_await MySQLConnector::instance().get_pool(1).async_get_connection(use_awaitable);
            results r;
            const auto sql = "SELECT " + conversationColumns + " FROM im_conversations" +
                boost::mysql::format_sql(connection->format_opts().value(),
                    " WHERE owner_account={} AND conversation_id={} AND is_hidden=0", owner, id);
            co_await connection->async_execute(sql, r, use_awaitable);
            if (r.rows().empty())
                co_return std::nullopt;
            co_return conversationRow(r.rows()[0]);
        }, use_awaitable);
}

boost::asio::awaitable<conversation::GetSortedConversationListResp> MySQLHandler::SortedConversations(
    conversation::GetSortedConversationListReq query)
{
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(1),
        [query = std::move(query)]() -> boost::asio::awaitable<conversation::GetSortedConversationListResp> {
            if (!query.userid() || query.conversationids_size() > 1000)
                throw std::invalid_argument("Invalid conversation list request");
            auto connection = co_await MySQLConnector::instance().get_pool(1).async_get_connection(use_awaitable);
            std::string filter = boost::mysql::format_sql(connection->format_opts().value(),
                " WHERE owner_account={} AND is_hidden=0", query.userid());
            if (query.conversationids_size()) {
                filter += " AND conversation_id IN (";
                for (int i = 0; i < query.conversationids_size(); ++i) {
                    if (i)
                        filter += ',';
                    filter += boost::mysql::format_sql(connection->format_opts().value(), "{}", query.conversationids(i));
                }
                filter += ')';
            }
            conversation::GetSortedConversationListResp response;
            results isolation;
            co_await connection->async_execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ", isolation, use_awaitable);
            co_await conversationTransaction(*connection.operator->(), [&]() -> boost::asio::awaitable<bool> {
                results r;
                co_await connection->async_execute("SELECT COUNT(*),COALESCE(SUM(unread_count),0) FROM im_conversations" + filter,
                    r, use_awaitable);
                response.set_conversationtotal(static_cast<int64_t>(conversationNumber(r.rows()[0][0])));
                const auto sum = r.rows()[0][1];
                response.set_unreadtotal(sum.is_string() ? std::stoll(std::string(sum.as_string()))
                    : static_cast<int64_t>(conversationNumber(sum)));
                const auto page = std::max(1, query.pagination().pagenumber());
                const auto size = std::clamp(query.pagination().shownumber(), 1, 100);
                const auto offset = static_cast<int64_t>(page - 1) * size;
                auto sql = "SELECT " + conversationColumns + " FROM im_conversations" + filter +
                    boost::mysql::format_sql(connection->format_opts().value(),
                        " ORDER BY is_pinned DESC,latest_time DESC,conversation_id DESC LIMIT {} OFFSET {}", size, offset);
                co_await connection->async_execute(sql, r, use_awaitable);
                for (auto row : r.rows()) {
                    auto item = conversationRow(row);
                    auto* entry = response.add_conversationelems();
                    entry->set_conversationid(item.conversationid());
                    entry->set_recvmsgopt(item.recvmsgopt());
                    entry->set_unreadcount(item.unreadcount());
                    entry->set_ispinned(item.ispinned());
                    *entry->mutable_msginfo() = item.latestmsg();
                }
                co_return true;
            });
            co_return response;
        }, use_awaitable);
}

boost::asio::awaitable<bool> MySQLHandler::MarkConversationRead(uint64_t owner, std::string id, int64_t seq)
{
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        [owner, id = std::move(id), seq]() -> boost::asio::awaitable<bool> {
            auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
            co_return co_await conversationTransaction(*connection.operator->(), [&]() -> boost::asio::awaitable<bool> {
                results r;
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "SELECT max_seq,read_seq FROM im_conversations WHERE owner_account={} AND conversation_id={} FOR UPDATE",
                    owner, id), r, use_awaitable);
                if (r.rows().empty() || seq <= 0 || static_cast<uint64_t>(seq) > conversationNumber(r.rows()[0][0]))
                    co_return false;
                if (static_cast<uint64_t>(seq) <= conversationNumber(r.rows()[0][1]))
                    co_return true;
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "DELETE FROM im_conversation_unread WHERE owner_account={} AND conversation_id={} AND seq<={}",
                    owner, id, seq), r, use_awaitable);
                const auto removed = r.affected_rows();
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "UPDATE im_conversations SET read_seq={},unread_count=unread_count-LEAST(unread_count,{}) "
                    "WHERE owner_account={} AND conversation_id={}", seq, removed, owner, id), r, use_awaitable);
                co_return true;
            });
        }, use_awaitable);
}

boost::asio::awaitable<bool> MySQLHandler::UpdateConversationSettings(uint64_t owner, std::string id,
    bool hasPinned, bool pinned, bool hasRecvMsgOpt, int32_t recvMsgOpt)
{
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        [owner, id = std::move(id), hasPinned, pinned, hasRecvMsgOpt, recvMsgOpt]() -> boost::asio::awaitable<bool> {
            if ((!hasPinned && !hasRecvMsgOpt) || (hasRecvMsgOpt && (recvMsgOpt < 0 || recvMsgOpt > 1)))
                throw std::invalid_argument("Invalid conversation settings");
            auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
            results r;
            co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                "UPDATE im_conversations SET is_pinned=IF({},{},is_pinned),recv_msg_opt=IF({},{},recv_msg_opt) "
                "WHERE owner_account={} AND conversation_id={} AND is_hidden=0",
                hasPinned, pinned, hasRecvMsgOpt, recvMsgOpt, owner, id), r, use_awaitable);
            if (r.affected_rows())
                co_return true;
            co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                "SELECT 1 FROM im_conversations WHERE owner_account={} AND conversation_id={} AND is_hidden=0",
                owner, id), r, use_awaitable);
            co_return !r.rows().empty();
        }, use_awaitable);
}

boost::asio::awaitable<bool> MySQLHandler::DeleteConversation(uint64_t owner, std::string id)
{
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        [owner, id = std::move(id)]() -> boost::asio::awaitable<bool> {
            auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
            co_return co_await conversationTransaction(*connection.operator->(), [&]() -> boost::asio::awaitable<bool> {
                results r;
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "SELECT max_seq FROM im_conversations WHERE owner_account={} AND conversation_id={} FOR UPDATE",
                    owner, id), r, use_awaitable);
                if (r.rows().empty())
                    co_return true;
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "DELETE FROM im_conversation_unread WHERE owner_account={} AND conversation_id={}", owner, id), r, use_awaitable);
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "UPDATE im_conversations SET is_hidden=1,read_seq=max_seq,unread_count=0 "
                    "WHERE owner_account={} AND conversation_id={}", owner, id), r, use_awaitable);
                co_return true;
            });
        }, use_awaitable);
}

namespace {

const std::string outboxColumns =
    "id,operation,actor_account,peer_account,conversation_id,server_msg_id,seq,payload,state,stage,attempts,lease_token,dispatch_done";

MySQLHandler::OutboxTask outboxRow(boost::mysql::row_view row)
{
    MySQLHandler::OutboxTask task;
    task.id = unsigned_field(row[0], "outbox.id");
    task.operation = static_cast<int>(unsigned_field(row[1], "outbox.operation"));
    task.actor = unsigned_field(row[2], "outbox.actor");
    task.peer = unsigned_field(row[3], "outbox.peer");
    task.conversation = std::string(row[4].as_string());
    task.serverId = std::string(row[5].as_string());
    task.seq = static_cast<int64_t>(unsigned_field(row[6], "outbox.seq"));
    task.payload = std::string(row[7].as_string());
    task.state = static_cast<int>(unsigned_field(row[8], "outbox.state"));
    task.stage = static_cast<int>(unsigned_field(row[9], "outbox.stage"));
    task.attempts = static_cast<int>(unsigned_field(row[10], "outbox.attempts"));
    task.leaseToken = std::string(row[11].as_string());
    task.dispatchDone = unsigned_field(row[12], "outbox.dispatch_done") != 0;
    return task;
}

sdkws::MsgData outboxMessage(const MySQLHandler::OutboxTask& task)
{
    sdkws::MsgData message;
    if (!google::protobuf::util::JsonStringToMessage(task.payload, &message).ok())
        throw std::runtime_error("Invalid outbox message payload");
    return message;
}

// 当前好友关系和历史删除状态，是否允许这条消息继续处理。
boost::asio::awaitable<bool> friendAllows(boost::mysql::any_connection& connection,
    uint64_t actor, uint64_t peer, int64_t seq)
{
    results r;
    // 从friends表中查关系行数
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "SELECT COUNT(*) FROM friends WHERE (owner_account={} AND friend_account={}) "
        "OR (owner_account={} AND friend_account={})", actor, peer, peer, actor), r, use_awaitable);
    
    // 正常的好友关系有两条记录:A-B,B-A
    if (actor == peer || unsigned_field(r.rows()[0][0], "friends.count") != 2)
        co_return false;

    // friend_chat_deletions记录了好友关系删除时消息的最高水位，以后好友关系恢复后旧消息也不能返回
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "SELECT deleted_seq FROM friend_chat_deletions WHERE owner_account={} AND peer_account={}",
        actor, peer), r, use_awaitable);
    co_return r.rows().empty() || seq > static_cast<int64_t>(unsigned_field(r.rows()[0][0], "deletion.seq"));
}

}

boost::asio::awaitable<void> MySQLHandler::InitializeOutbox()
{
    auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    co_await initialize(*connection.operator->());
    results r;
    co_await connection->async_execute(R"sql(CREATE TABLE IF NOT EXISTS im_outbox (
        id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
        operation TINYINT UNSIGNED NOT NULL,
        dedupe_key VARCHAR(256) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        actor_account BIGINT UNSIGNED NOT NULL,
        peer_account BIGINT UNSIGNED NOT NULL,
        conversation_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        server_msg_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL DEFAULT '',
        seq BIGINT NOT NULL,
        payload MEDIUMTEXT NOT NULL,
        state TINYINT UNSIGNED NOT NULL DEFAULT 0,
        stage TINYINT UNSIGNED NOT NULL DEFAULT 0,
        dispatch_done TINYINT UNSIGNED NOT NULL DEFAULT 0,
        attempts INT UNSIGNED NOT NULL DEFAULT 0,
        available_at BIGINT NOT NULL DEFAULT 0,
        lease_until BIGINT NOT NULL DEFAULT 0,
        lease_token VARCHAR(64) CHARACTER SET ascii NOT NULL DEFAULT '',
        last_error VARCHAR(2048) NOT NULL DEFAULT '',
        created_at BIGINT NOT NULL,
        updated_at BIGINT NOT NULL,
        UNIQUE KEY outbox_dedupe(operation,dedupe_key),
        KEY outbox_ready(state,available_at,id),
        KEY outbox_lease(state,lease_until,id),
        KEY outbox_message(server_msg_id,operation),
        KEY outbox_pair(conversation_id,operation,seq)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)sql", r, use_awaitable);
    co_await connection->async_execute(R"sql(CREATE TABLE IF NOT EXISTS im_message_receipts (
        owner_account BIGINT UNSIGNED NOT NULL,
        server_msg_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        conversation_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        seq BIGINT NOT NULL,
        delivered_at BIGINT NOT NULL,
        PRIMARY KEY(owner_account,server_msg_id),
        KEY receipt_conversation(owner_account,conversation_id,seq)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)sql", r, use_awaitable);
}

boost::asio::awaitable<MySQLHandler::OutboxTask> MySQLHandler::QueueSingleMessage(sdkws::MsgData message)
{
    // 这里可以优化，随机取得一个执行器
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        [message = std::move(message)]() -> boost::asio::awaitable<OutboxTask> {

            // 检验单聊消息的合法性
            if (!message.sendid() || !message.recvid() || message.sendid() == message.recvid() ||
                message.isgroupmsg() || message.sessiontype() != 1 || message.clientmsgid().empty() ||
                message.clientmsgid().size() > 128 || message.content().size() > 64 * 1024 ||
                std::any_of(message.clientmsgid().begin(), message.clientmsgid().end(),
                    [](unsigned char c) { return c < 33 || c > 126; })){
                throw std::invalid_argument("Invalid single-chat message");
            }
                
            // 从池中取得一条连接
            auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
            OutboxTask task;
            co_await conversationTransaction(*connection.operator->(), [&]() -> boost::asio::awaitable<bool> {
                
                // 先取得好友关系的锁，避免当前事务执行过程中受到好友关系变更的影响
                co_await lockPair(*connection.operator->(), message.sendid(), message.recvid());
                // 判断一下当前好友关系、这条消息的序号能不能被继续处理
                if (!co_await friendAllows(*connection.operator->(), message.sendid(), message.recvid(), message.seq()))
                    throw std::invalid_argument("Only friends may exchange messages");

                const auto key = std::to_string(message.sendid()) + ":" + message.clientmsgid();
                results r;
                auto query = "SELECT " + outboxColumns + " FROM im_outbox" +
                    boost::mysql::format_sql(connection->format_opts().value(),
                        " WHERE operation=1 AND dedupe_key={} FOR UPDATE", key);
                co_await connection->async_execute(query, r, use_awaitable);
                if (!r.rows().empty()) {
                    task = outboxRow(r.rows()[0]);
                    if (task.state == 3 || task.payload.empty())
                        throw std::invalid_argument("This clientMsgID belongs to a deleted conversation");
                    const auto original = outboxMessage(task);
                    if (!co_await friendAllows(*connection.operator->(), original.sendid(), original.recvid(), original.seq()))
                        throw std::invalid_argument("This clientMsgID belongs to a deleted conversation");
                    if (original.recvid() != message.recvid() || original.content() != message.content() ||
                        original.contenttype() != message.contenttype() || original.sessiontype() != message.sessiontype())
                        throw std::invalid_argument("clientMsgID was reused for a different message");
                    if (task.state == 3)
                        throw std::invalid_argument("This message was cancelled by a friend deletion");
                    co_return true;
                }
                std::string payload;
                if (!google::protobuf::util::MessageToJsonString(message, &payload).ok())
                    throw std::invalid_argument("Invalid message payload");
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "INSERT INTO im_outbox(operation,dedupe_key,actor_account,peer_account,conversation_id,server_msg_id,"
                    "seq,payload,created_at,updated_at) VALUES(1,{},{},{},{},{},{},{},{},{})",
                    key, message.sendid(), message.recvid(), message.convid(), message.servermsgid(), message.seq(),
                    payload, now(), now()), r, use_awaitable);
                task.id = r.last_insert_id();
                task.actor = message.sendid(); 
                task.peer = message.recvid(); 
                task.seq = message.seq();
                task.conversation = message.convid(); 
                task.serverId = message.servermsgid();
                task.payload = std::move(payload); 
                task.operation = 1;
                co_return true;
            });
            co_return task;
        }, use_awaitable);
}

boost::asio::awaitable<void> MySQLHandler::QueueConversationCleanup(boost::mysql::any_connection& connection,
    uint64_t actor, uint64_t peer, int64_t cutoff)
{
    const auto id = std::to_string(std::min(actor, peer)) + "_" + std::to_string(std::max(actor, peer));
    results r;
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "INSERT INTO im_outbox(operation,dedupe_key,actor_account,peer_account,conversation_id,seq,payload,created_at,updated_at) "
        "VALUES(2,{},{},{},{},{},'',{},{}) ON DUPLICATE KEY UPDATE id=im_outbox.id",
        id + ":" + std::to_string(cutoff), actor, peer, id, cutoff, now(), now()), r, use_awaitable);
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "UPDATE im_outbox SET state=3,payload='',lease_token='',last_error='Friend relationship removed',updated_at={} "
        "WHERE conversation_id={} AND operation=1 AND seq<={} AND state IN (0,1)", now(), id, cutoff), r, use_awaitable);
}

boost::asio::awaitable<MySQLHandler::OutboxTask> MySQLHandler::GetOutboxTask(uint64_t id)
{
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(1),
        [id]() -> boost::asio::awaitable<OutboxTask> {
            auto connection = co_await MySQLConnector::instance().get_pool(1).async_get_connection(use_awaitable);
            results r;
            auto query = "SELECT " + outboxColumns + " FROM im_outbox" +
                boost::mysql::format_sql(connection->format_opts().value(), " WHERE id={}", id);
            co_await connection->async_execute(query, r, use_awaitable);
            if (r.rows().empty())
                throw std::runtime_error("Outbox task missing");
            co_return outboxRow(r.rows()[0]);
        }, use_awaitable);
}

// 从数据库里面取任务
boost::asio::awaitable<std::optional<MySQLHandler::OutboxTask>> MySQLHandler::ClaimOutboxTask()
{
    auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    std::optional<OutboxTask> task;
    co_await conversationTransaction(*connection.operator->(), [&]() -> boost::asio::awaitable<bool> {
        results r;
        const auto query = "SELECT " + outboxColumns + " FROM im_outbox" +
            boost::mysql::format_sql(connection->format_opts().value(),
                " WHERE state=0 AND available_at<={} ORDER BY available_at,id LIMIT 1 FOR UPDATE SKIP LOCKED", now());
        co_await connection->async_execute(query, r, use_awaitable);
        if (r.rows().empty()) {
            const auto expired = "SELECT " + outboxColumns + " FROM im_outbox" +
                boost::mysql::format_sql(connection->format_opts().value(),
                    " WHERE state=1 AND lease_until<={} ORDER BY lease_until,id LIMIT 1 FOR UPDATE SKIP LOCKED", now());
            co_await connection->async_execute(expired, r, use_awaitable);
            if (r.rows().empty())
                co_return false;
        }
        task = outboxRow(r.rows()[0]);
        task->leaseToken = uuid::newone_str();
        task->attempts++;
        task->state = 1;
        co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
            "UPDATE im_outbox SET state=1,attempts=attempts+1,lease_token={},lease_until={},updated_at={} WHERE id={}",
            task->leaseToken, now() + 45000, now(), task->id), r, use_awaitable);
        co_return true;
    });
    co_return task;
}

boost::asio::awaitable<void> MySQLHandler::RetryOutboxTask(OutboxTask task, std::string error)
{
    auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    results r;
    const auto delay = std::min<int64_t>(60000, int64_t{1000} << std::min(task.attempts, 6));
    co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
        "UPDATE im_outbox SET state=0,available_at={},lease_token='',last_error={},updated_at={} "
        "WHERE id={} AND state=1 AND lease_token={}", now() + delay, error.substr(0, 1500), now(),
        task.id, task.leaseToken), r, use_awaitable);
}

boost::asio::awaitable<bool> MySQLHandler::QueueMessageAck(uint64_t owner, std::string serverId)
{
    if (!owner || serverId.empty() || serverId.size() > 128)
        throw std::invalid_argument("Invalid message acknowledgement");
    sdkws::MsgData message;
    {
        auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
        results r;
        co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
            "SELECT payload,stage,peer_account,state FROM im_outbox WHERE operation=1 AND server_msg_id={}", serverId), r, use_awaitable);
        if (!r.rows().empty()) {
            if (unsigned_field(r.rows()[0][2], "outbox.recipient") != owner)
                throw std::invalid_argument("Acknowledgement owner does not match recipient");
            if (unsigned_field(r.rows()[0][3], "outbox.state") == 3)
                co_return true;
            if (unsigned_field(r.rows()[0][1], "outbox.stage") == 0 ||
                !google::protobuf::util::JsonStringToMessage(std::string(r.rows()[0][0].as_string()), &message).ok())
                throw std::invalid_argument("Message has not been committed");
        }
    }
    if (message.servermsgid().empty()) {
        auto found = co_await MongoHandler::GetMsgByServerMsgIDAsync(serverId);
        if (!found)
            co_return true;
        message = std::move(*found);
    }
    if (message.isgroupmsg())
        co_return false;
    if (message.recvid() != owner)
        throw std::invalid_argument("Acknowledgement owner does not match recipient");
    auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    co_await conversationTransaction(*connection.operator->(), [&]() -> boost::asio::awaitable<bool> {
        co_await lockPair(*connection.operator->(), message.sendid(), message.recvid());
        results r;
        co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
            "INSERT INTO im_message_receipts(owner_account,server_msg_id,conversation_id,seq,delivered_at) "
            "VALUES({},{},{},{},{}) ON DUPLICATE KEY UPDATE server_msg_id=im_message_receipts.server_msg_id",
            owner, serverId, message.convid(), message.seq(), now()), r, use_awaitable);
        co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
            "INSERT INTO im_outbox(operation,dedupe_key,actor_account,peer_account,conversation_id,server_msg_id,seq,"
            "payload,created_at,updated_at) VALUES(3,{},{},{},{},{},{},'',{},{}) ON DUPLICATE KEY UPDATE id=im_outbox.id",
            std::to_string(owner) + ":" + serverId, message.sendid(), owner, message.convid(), serverId,
            message.seq(), now(), now()), r, use_awaitable);
        co_return true;
    });
    co_return true;
}

// 这个设计是为了把请求的执行变成数据库里面可恢复的流程
// 如果请求的流程（如redis写失败）因为某个环节中断了，可以通过各个流程组件的幂等性和task的状态进行恢复（重复执行）
boost::asio::awaitable<void> MySQLHandler::ProcessOutboxTask(OutboxTask task, IMessageProducer& producer)
{
    auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    auto phase = [&](bool persist) -> boost::asio::awaitable<bool> {
        co_return co_await conversationTransaction(*connection.operator->(), [&]() -> boost::asio::awaitable<bool> {
            co_await lockPair(*connection.operator->(), task.actor, task.peer);
            results r;
            auto query = "SELECT " + outboxColumns + " FROM im_outbox" +
                boost::mysql::format_sql(connection->format_opts().value(), " WHERE id={} FOR UPDATE", task.id);
            co_await connection->async_execute(query, r, use_awaitable);
            if (r.rows().empty())
                co_return false;
            auto current = outboxRow(r.rows()[0]);
            if (current.state != 1 || current.leaseToken != task.leaseToken)
                co_return false;
            task.stage = current.stage;
            // 单聊消息的保存和分发
            if (task.operation == 1) {  
                // 再次检查
                if (!co_await friendAllows(*connection.operator->(), task.actor, task.peer, task.seq)) {
                    co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                        "UPDATE im_outbox SET state=3,last_error='Friend relationship removed',updated_at={} WHERE id={}",
                        now(), task.id), r, use_awaitable);
                    co_return false;
                }
                auto message = outboxMessage(task);
                if (persist) {
                    if (task.stage == 0) {
                        // 保存正文
                        if (!co_await MongoHandler::SaveMsgToMongoAsync(message, false))
                            throw std::runtime_error("MongoDB message persistence failed");
                        // 更新会话
                        co_await RecordConversationMessage(*connection.operator->(), message);
                        // 更新任务阶段
                        co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                            "UPDATE im_outbox SET stage=1,updated_at={} WHERE id={}", now(), task.id), r, use_awaitable);
                    }
                    co_return true;
                }

                // 设置消息为可投递
                co_await MongoHandler::PublishMessage(task.serverId);
                // 查询客户端是否已经发送这条消息的ACK
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "SELECT 1 FROM im_message_receipts WHERE owner_account={} AND server_msg_id={}", task.peer, task.serverId),
                    r, use_awaitable);
                if (r.rows().empty()) {
                    if (!co_await RedisHandler::SaveMsg(message))
                        throw std::runtime_error("Redis message caching failed");
                    // 带确认投递成功状态的投递
                    if (!co_await producer.deliverConfirmed(task.conversation, task.serverId))
                        throw std::runtime_error("Kafka broker did not confirm delivery");
                    // 更新任务状态为待执行（可以被worker领取），因为还没确认消息是否完成分发（进入离线流程/收到ACK），保留重试的能力
                    co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                        "UPDATE im_outbox SET state=0,available_at={},lease_token='',last_error='',updated_at={} WHERE id={}",
                        now() + 5000, now(), task.id), r, use_awaitable);
                    co_return true;
                }
            } else if (task.operation == 2) {       // 删除好友后的历史清理
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "UPDATE im_outbox SET state=3,payload='',lease_token='',last_error='Friend relationship removed',updated_at={} "
                    "WHERE operation=1 AND conversation_id={} AND seq<={}", now(), task.conversation, task.seq), r, use_awaitable);
                auto ids = co_await MongoHandler::GetSingleChatMessageIDsAsync(task.conversation, task.seq);
                co_await RedisHandler::DeleteSingleChatHistory(task.actor, task.peer, std::move(ids));
                co_await MongoHandler::DeleteSingleChatHistoryAsync(task.conversation, task.seq);
            } else if (task.operation == 3) {       // 客户端ACK后的投递状态与缓存清理
                if (!co_await MongoHandler::MarkMsgsAsDeliveredAsync(task.peer, {task.serverId}))
                    throw std::runtime_error("MongoDB acknowledgement update failed");
                // ACK cleanup deletes the realtime cache. No ZSET or max-ACK watermarks
                // are produced; legacy per-message metadata is removed during migration.
                co_await RedisHandler::CleanupAcknowledgedMessage(task.peer,task.serverId);
            } else {
                throw std::runtime_error("Unknown outbox operation");
            }
            co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                "UPDATE im_outbox SET state=2,lease_token='',last_error='',updated_at={} WHERE id={}", now(), task.id),
                r, use_awaitable);
            co_return true;
        });
    };
    if (task.operation == 1 && !co_await phase(true))
        co_return;
    co_await phase(false);
}

boost::asio::awaitable<void> MySQLHandler::FilterVisibleMessages(std::vector<sdkws::MsgData>& messages)
{
    if (messages.empty())
        co_return;
    co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(1),
        [&messages]() -> boost::asio::awaitable<void> {
            auto connection = co_await MySQLConnector::instance().get_pool(1).async_get_connection(use_awaitable);
            std::string source;
            for (const auto& message : messages) {
                if (!source.empty())
                    source += " UNION ALL ";
                source += boost::mysql::format_sql(connection->format_opts().value(),
                    "SELECT {} server_id,CAST({} AS UNSIGNED) send_id,CAST({} AS UNSIGNED) recv_id,{} seq",
                    message.servermsgid(), message.sendid(), message.recvid(), message.seq());
            }
            const auto sql = "SELECT m.server_id FROM (" + source + ") m "
                "JOIN friends a ON a.owner_account=m.send_id AND a.friend_account=m.recv_id "
                "JOIN friends b ON b.owner_account=m.recv_id AND b.friend_account=m.send_id "
                "LEFT JOIN friend_chat_deletions d ON d.owner_account=m.send_id AND d.peer_account=m.recv_id "
                "LEFT JOIN im_outbox o ON o.operation=1 AND o.server_msg_id=m.server_id "
                "WHERE (d.deleted_seq IS NULL OR m.seq>d.deleted_seq) AND (o.id IS NULL OR (o.stage=1 AND o.state<>3))";
            results r;
            co_await connection->async_execute(sql, r, use_awaitable);
            std::unordered_set<std::string> visible;
            for (auto row : r.rows())
                visible.emplace(row[0].as_string());
            messages.erase(std::remove_if(messages.begin(), messages.end(),
                [&](const auto& message) { return !visible.contains(message.servermsgid()); }), messages.end());
        }, use_awaitable);
}

boost::asio::awaitable<int64_t> MySQLHandler::FriendDeletionSeq(uint64_t owner, uint64_t peer)
{
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(1),
        [owner, peer]() -> boost::asio::awaitable<int64_t> {
            auto connection = co_await MySQLConnector::instance().get_pool(1).async_get_connection(use_awaitable);
            results r;
            co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                "SELECT deleted_seq FROM friend_chat_deletions WHERE owner_account={} AND peer_account={}", owner, peer),
                r, use_awaitable);
            co_return r.rows().empty() ? 0 : static_cast<int64_t>(unsigned_field(r.rows()[0][0], "deletion.seq"));
        }, use_awaitable);
}

boost::asio::awaitable<bool> MySQLHandler::MessageDispatchDone(uint64_t owner, std::string serverId)
{
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(1),
        [owner, serverId = std::move(serverId)]() -> boost::asio::awaitable<bool> {
            auto connection = co_await MySQLConnector::instance().get_pool(1).async_get_connection(use_awaitable);
            results r;
            co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                "SELECT 1 FROM im_outbox WHERE operation=1 AND server_msg_id={} AND (state=3 OR dispatch_done=1) "
                "UNION ALL SELECT 1 FROM im_message_receipts WHERE owner_account={} AND server_msg_id={} LIMIT 1",
                serverId, owner, serverId), r, use_awaitable);
            co_return !r.rows().empty();
        }, use_awaitable);
}

boost::asio::awaitable<void> MySQLHandler::ConfirmMessageDispatch(std::string serverId)
{
    co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        [serverId = std::move(serverId)]() -> boost::asio::awaitable<void> {
            auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
            results r;
            co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                "UPDATE im_outbox SET state=2,dispatch_done=1,lease_token='',last_error='',updated_at={} "
                "WHERE operation=1 AND server_msg_id={} AND stage=1 AND state<>3", now(), serverId), r, use_awaitable);
        }, use_awaitable);
}

// Group persistence: all SQL stays in the storage handler.
namespace group_storage {
using boost::asio::awaitable;
using boost::asio::use_awaitable;
using boost::mysql::results;
using Connection = boost::mysql::any_connection;
namespace {
struct Error : std::runtime_error {
    std::string code;
    Error(std::string c, std::string message) : std::runtime_error(message), code(std::move(c)) {}
};
uint64_t number(boost::mysql::field_view f) {
    return f.is_uint64() ? f.as_uint64() : static_cast<uint64_t>(f.as_int64());
}
std::string str(boost::mysql::field_view f) { return std::string(f.as_string()); }
int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string hex(const std::string& bytes) {
    const char* digits = "0123456789abcdef";
    std::string result;
    for (unsigned char c : bytes) { result += digits[c >> 4]; result += digits[c & 15]; }
    return result;
}
std::string unhex(const std::string& s) {
    std::string result;
    auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    if (s.size() % 2) throw Error("CACHE_ERROR", "Invalid saved request");
    for (size_t i = 0; i < s.size(); i += 2) result += char(digit(s[i]) * 16 + digit(s[i+1]));
    return result;
}
template<class... Args>
awaitable<results> query(Connection& c, boost::mysql::constant_string_view sql, Args&&... args) {
    results r;
    co_await c.async_execute(boost::mysql::format_sql(c.format_opts().value(), sql,
        std::forward<Args>(args)...), r, use_awaitable);
    co_return r;
}
awaitable<void> finish(Connection& c, bool commit) {
    results r;
    co_await c.async_execute(commit ? "COMMIT" : "ROLLBACK", r, use_awaitable);
}
groupchat::Group groupRow(boost::mysql::row_view row) {
    groupchat::Group g;
    g.set_id(std::to_string(number(row[0]))); g.set_name(str(row[1]));
    g.set_owner(number(row[2])); g.set_membercount(number(row[3]));
    g.set_dismissed(number(row[4]) != 0); g.set_lastseq(number(row[5]));
    g.set_large(g.membercount() > 200);
    return g;
}
groupchat::Message messageRow(boost::mysql::row_view row) {
    groupchat::Message m;
    m.set_groupid(std::to_string(number(row[0]))); m.set_seq(number(row[1]));
    m.set_id(str(row[2])); m.set_clientid(str(row[3])); m.set_sender(number(row[4]));
    m.set_nickname(str(row[5])); m.set_text(str(row[6])); m.set_time(number(row[7]));
    return m;
}
uint64_t groupId(const std::string& s) {
    if (s.empty() || s.size() > 20 || s.find_first_not_of("0123456789") != std::string::npos)
        throw Error("INVALID_GROUP", "请输入有效的数字群号");
    try { auto id = std::stoull(s); if (id) return id; } catch (...) {}
    throw Error("INVALID_GROUP", "请输入有效的数字群号");
}
awaitable<uint64_t> allocateGroupNumber(Connection& c,uint64_t id) {
    auto cursor=co_await query(c,"SELECT next_number FROM im_group_number_cursor WHERE id=1 FOR UPDATE");
    const auto value=number(cursor.rows()[0][0]);
    if(value>AccountPoolAllocator::kMaxAccount) throw Error("GROUP_NUMBER_EXHAUSTED","群号已用尽");
    co_await query(c,"INSERT INTO im_group_numbers(group_id,number) VALUES({},{})",id,value);
    co_await query(c,"UPDATE im_group_number_cursor SET next_number={} WHERE id=1",value+1);
    co_return value;
}
awaitable<void> memberState(Connection& c, groupchat::Group& g, uint64_t actor) {
    auto publicNumber=co_await query(c,"SELECT number FROM im_group_numbers WHERE group_id={}",groupId(g.id()));
    if(!publicNumber.rows().empty()) g.set_number(std::to_string(number(publicNumber.rows()[0][0])));

    auto r = co_await query(c, "SELECT active,join_seq,read_seq,epoch FROM im_group_members WHERE group_id={} AND account={}", groupId(g.id()), actor);
    if (r.rows().empty()) co_return;
    auto row = r.rows()[0];
    g.set_joined(number(row[0]) != 0 && !g.dismissed());
    g.set_joinseq(number(row[1])); g.set_readseq(number(row[2])); g.set_epoch(number(row[3]));
    if (g.joined()) {
        // Snowflake/global progress is not used: every group has contiguous sequence numbers.
        const auto first = std::max(g.joinseq(), g.readseq());
        auto count = co_await query(c, "SELECT COUNT(*) FROM im_group_messages WHERE group_id={} AND seq>{} AND sender<>{}", groupId(g.id()), first, actor);
        g.set_unread(number(count.rows()[0][0]));
    }
}
awaitable<groupchat::Reply> perform(Connection& c, const groupchat::Command& cmd) {
    groupchat::Reply reply; reply.set_errorcode("0"); reply.set_groupid(cmd.groupid());
    auto op = cmd.operation(); const uint64_t actor = cmd.actor();
    if (!actor || op < groupchat::List || op > groupchat::MyApplications)
        throw Error("INVALID_PARAM", "无效群操作");
    if (cmd.text().size() > (op == groupchat::Send ? 65536u : 512u))
        throw Error("INVALID_PARAM", "内容过长");
    const auto limit = std::clamp<uint32_t>(cmd.limit(), 1, op == groupchat::Pull ? 50 : 100);
    auto user = co_await query(c, "SELECT nickname FROM users WHERE account={}", actor);
    if (user.rows().empty()) throw Error("INVALID_ACCOUNT", "账号不存在");
    const auto nickname = str(user.rows()[0][0]);
    if(op==groupchat::MyApplications) {
        auto r=co_await query(c,"SELECT a.id,a.group_id,g.name,a.reason,a.status,a.created_time,n.number FROM im_group_applications a JOIN im_groups g ON a.group_id=g.id JOIN im_group_numbers n ON n.group_id=g.id WHERE a.account={} AND a.id>{} ORDER BY a.id LIMIT {}",actor,cmd.after(),limit+1);
        reply.set_hasmore(r.rows().size()>limit);
        for(size_t i=0;i<std::min<size_t>(limit,r.rows().size());++i) {
            auto row=r.rows()[i]; auto* a=reply.add_applications(); a->set_id(number(row[0]));
            a->set_groupid(std::to_string(number(row[1]))); a->set_groupname(str(row[2]));
            a->set_groupnumber(std::to_string(number(row[6])));
            a->set_account(actor); a->set_nickname(nickname); a->set_reason(str(row[3])); a->set_status(number(row[4])); a->set_time(number(row[5])); reply.set_cursor(a->id());
        }
        co_return reply;
    }
    if (op == groupchat::List) {
        auto r = co_await query(c, "SELECT g.id,g.name,g.owner,g.member_count,g.dismissed,g.last_seq FROM im_groups g JOIN im_group_members m ON m.group_id=g.id WHERE m.account={} AND g.id>{} ORDER BY g.id LIMIT {}", actor, cmd.after(), limit+1);
        reply.set_hasmore(r.rows().size() > limit);
        for (size_t i=0; i<std::min<size_t>(limit,r.rows().size()); ++i) {
            auto g = groupRow(r.rows()[i]);
            co_await memberState(c,g,actor); reply.set_cursor(groupId(g.id()));
            *reply.add_groups() = g;
        }
        co_return reply;
    }
    if (op == groupchat::Create) {
        if (cmd.text().empty() || cmd.text().size() > 256) throw Error("INVALID_NAME", "群名不能为空且不能超过256字节");
        const auto id = static_cast<uint64_t>(Snowflake::instance().nextId());
        co_await query(c,"INSERT INTO im_groups(id,name,owner) VALUES({},{},{})",id,cmd.text(),actor);
        co_await allocateGroupNumber(c,id);
        co_await query(c,"INSERT INTO im_group_members(group_id,account) VALUES({},{})",id,actor);
        auto r = co_await query(c,"SELECT id,name,owner,member_count,dismissed,last_seq FROM im_groups WHERE id={}",id);
        auto g=groupRow(r.rows()[0]); co_await memberState(c,g,actor); *reply.add_groups()=g;
        reply.set_groupid(g.id()); co_return reply;
    }
    auto id=groupId(cmd.groupid());

    if(op==groupchat::Search) {
        auto lookup=co_await query(c,"SELECT group_id FROM im_group_numbers WHERE number={}",id);
        if(!lookup.rows().empty()) id=number(lookup.rows()[0][0]);
        else if(cmd.groupid().size()<=10) throw Error("GROUP_NOT_FOUND","未找到该群号");
        // Longer identifiers remain accepted for old clients and persisted requests.
    }
    auto r=co_await query(c,"SELECT id,name,owner,member_count,dismissed,last_seq FROM im_groups WHERE id={} FOR UPDATE",id);
    if(r.rows().empty()) throw Error("GROUP_NOT_FOUND","群不存在");
    auto g=groupRow(r.rows()[0]); co_await memberState(c,g,actor); reply.set_epoch(g.epoch());
    if(op==groupchat::Search) { reply.set_groupid(g.id()); *reply.add_groups()=g; co_return reply; }
    if(g.dismissed()) {
        if(op==groupchat::Dismiss && g.owner()==actor) co_return reply;
        throw Error("GROUP_DISMISSED","群已解散");
    }
    if(op==groupchat::Apply) {
        if(g.joined()) throw Error("ALREADY_JOINED","你已在群内");
        r=co_await query(c,"SELECT id FROM im_group_applications WHERE group_id={} AND account={} AND status=0 ORDER BY id DESC LIMIT 1",id,actor);
        if(r.rows().empty()) co_await query(c,"INSERT INTO im_group_applications(group_id,account,reason,created_time) VALUES({},{},{},{})",id,actor,cmd.text(),now());
        co_return reply;
    }
    if(!g.joined()) {
        if(op==groupchat::Leave) co_return reply;
        throw Error("NOT_MEMBER","你已不在群内");
    }
    if(cmd.epoch() && cmd.epoch()!=g.epoch()) throw Error("MEMBERSHIP_CHANGED","成员身份已变化，请刷新");
    if(op==groupchat::Applications || op==groupchat::Review || op==groupchat::Rename || op==groupchat::Kick || op==groupchat::Dismiss)
        if(g.owner()!=actor) throw Error("OWNER_REQUIRED","仅群主可以执行此操作");
    if(op==groupchat::Members) {
        r=co_await query(c,"SELECT m.account,u.nickname FROM im_group_members m JOIN users u ON u.account=m.account WHERE m.group_id={} AND m.active=1 AND m.account>{} ORDER BY m.account LIMIT {}",id,cmd.after(),limit+1);
        reply.set_hasmore(r.rows().size()>limit);
        for(size_t i=0;i<std::min<size_t>(limit,r.rows().size());++i) {
            auto* m=reply.add_members(); m->set_account(number(r.rows()[i][0])); m->set_nickname(str(r.rows()[i][1]));
            m->set_owner(m->account()==g.owner()); reply.set_cursor(m->account());
        }
    } else if(op==groupchat::Applications) {
        r=co_await query(c,"SELECT a.id,a.account,u.nickname,a.reason,a.status,a.created_time FROM im_group_applications a JOIN users u ON u.account=a.account WHERE a.group_id={} AND a.id>{} AND a.status=0 ORDER BY a.id LIMIT {}",id,cmd.after(),limit+1);
        reply.set_hasmore(r.rows().size()>limit);
        for(size_t i=0;i<std::min<size_t>(limit,r.rows().size());++i) {
            auto row=r.rows()[i]; auto* a=reply.add_applications();
            a->set_id(number(row[0])); a->set_groupid(g.id()); a->set_account(number(row[1]));
            a->set_nickname(str(row[2])); a->set_reason(str(row[3])); a->set_status(number(row[4])); a->set_time(number(row[5])); reply.set_cursor(a->id());
        }
    } else if(op==groupchat::Review) {
        r=co_await query(c,"SELECT account,status FROM im_group_applications WHERE id={} AND group_id={} FOR UPDATE",cmd.applicationid(),id);
        if(r.rows().empty()) throw Error("APPLICATION_NOT_FOUND","申请不存在");
        auto target=number(r.rows()[0][0]); const auto status=number(r.rows()[0][1]);
        if(status && status!=uint64_t(cmd.accept()?1:2)) throw Error("APPLICATION_HANDLED","申请已处理");
        if(!status) {
            if(cmd.accept()) {
                // Group row serializes all joins, exits and sends, including re-joins.
                co_await query(c,"INSERT INTO im_group_members(group_id,account,join_seq,ack_seq,read_seq) VALUES({},{},{},{},{}) ON DUPLICATE KEY UPDATE epoch=IF(active=0,epoch+1,epoch),join_seq=IF(active=0,VALUES(join_seq),join_seq),ack_seq=IF(active=0,VALUES(ack_seq),ack_seq),read_seq=IF(active=0,VALUES(read_seq),read_seq),active=1",id,target,g.lastseq(),g.lastseq(),g.lastseq());
                co_await query(c,"UPDATE im_groups SET member_count=(SELECT COUNT(*) FROM im_group_members WHERE group_id={} AND active=1) WHERE id={}",id,id);
            }
            co_await query(c,"UPDATE im_group_applications SET status={} WHERE id={}",cmd.accept()?1:2,cmd.applicationid());
        }
    } else if(op==groupchat::Rename) {
        if(cmd.text().empty() || cmd.text().size()>256) throw Error("INVALID_NAME","群名不能为空且不能超过256字节");
        if(cmd.text()==g.name()) throw Error("NAME_UNCHANGED","新群名称不能与当前名称相同");
        co_await query(c,"UPDATE im_groups SET name={} WHERE id={}",cmd.text(),id);
    } else if(op==groupchat::Leave || op==groupchat::Kick) {
        auto target=op==groupchat::Leave?actor:cmd.target();
        if(target==g.owner()) throw Error("OWNER_CANNOT_LEAVE","群主不能退出或被踢出，请解散群");
        co_await query(c,"UPDATE im_group_members SET active=0 WHERE group_id={} AND account={}",id,target);
        co_await query(c,"UPDATE im_groups SET member_count=(SELECT COUNT(*) FROM im_group_members WHERE group_id={} AND active=1) WHERE id={}",id,id);
    } else if(op==groupchat::Dismiss) {
        co_await query(c,"UPDATE im_groups SET dismissed=1 WHERE id={}",id);
        co_await query(c,"UPDATE im_group_applications SET status=3 WHERE group_id={} AND status=0",id);
    } else if(op==groupchat::Send) {
        if(cmd.text().empty()) throw Error("EMPTY_MESSAGE","消息不能为空");
        r=co_await query(c,"SELECT group_id,seq,id,client_id,sender,nickname,text,created_time FROM im_group_messages WHERE group_id={} AND sender={} AND client_id={}",id,actor,cmd.clientid());
        if(!r.rows().empty()) {
            auto m=messageRow(r.rows()[0]);
            if(m.text()!=cmd.text()) throw Error("IDEMPOTENCY_CONFLICT","消息标识重复且内容不同");
            *reply.add_messages()=m;
        } else {
            auto seq=g.lastseq()+1; const auto serverId=uuid::newone_str(); auto time=now();
            co_await query(c,"INSERT INTO im_group_messages(group_id,seq,id,client_id,sender,nickname,text,created_time) VALUES({},{},{},{},{},{},{},{})",id,seq,serverId,cmd.clientid(),actor,nickname,cmd.text(),time);
            co_await query(c,"UPDATE im_groups SET last_seq={} WHERE id={}",seq,id);
            groupchat::Message m; m.set_id(serverId); m.set_clientid(cmd.clientid()); m.set_groupid(g.id()); m.set_sender(actor); m.set_nickname(nickname); m.set_text(cmd.text()); m.set_seq(seq); m.set_time(time); *reply.add_messages()=m;
        }
    } else if(op==groupchat::Pull) {
        const auto snapshot=cmd.snapshot()?std::min(cmd.snapshot(),g.lastseq()):g.lastseq();
        const auto after=std::max(cmd.after(),g.joinseq());
        r=co_await query(c,"SELECT group_id,seq,id,client_id,sender,nickname,text,created_time FROM im_group_messages WHERE group_id={} AND seq>{} AND seq<={} ORDER BY seq LIMIT {}",id,after,snapshot,limit+1);
        reply.set_snapshot(snapshot); reply.set_cursor(after); reply.set_hasmore(r.rows().size()>limit);
        for(size_t i=0;i<std::min<size_t>(limit,r.rows().size());++i) { auto m=messageRow(r.rows()[i]); reply.set_cursor(m.seq()); *reply.add_messages()=m; }
    } else if(op==groupchat::Ack || op==groupchat::Read) {
        if(cmd.after()>g.lastseq()) throw Error("INVALID_SEQ","无效消息进度");
        const auto seq=std::max(cmd.after(),g.joinseq());
        if(op==groupchat::Ack) co_await query(c,"UPDATE im_group_members SET ack_seq=GREATEST(ack_seq,{}) WHERE group_id={} AND account={}",seq,id,actor);
        else co_await query(c,"UPDATE im_group_members SET read_seq=GREATEST(read_seq,{}) WHERE group_id={} AND account={}",seq,id,actor);
    }
    co_return reply;
}
awaitable<groupchat::Reply> executeOnPool(groupchat::Command cmd) {
    auto connection=co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    auto& c=*connection.operator->(); results r; std::exception_ptr failure;
    groupchat::Reply reply;
    const bool mutation=cmd.operation()==groupchat::Create || cmd.operation()==groupchat::Apply || cmd.operation()==groupchat::Review || cmd.operation()==groupchat::Rename || cmd.operation()==groupchat::Leave || cmd.operation()==groupchat::Kick || cmd.operation()==groupchat::Dismiss || cmd.operation()==groupchat::Send;
    // Permission reads after waiting for the group lock must see newly committed kicks/joins.
    co_await c.async_execute("SET TRANSACTION ISOLATION LEVEL READ COMMITTED",r,use_awaitable);
    co_await c.async_execute("START TRANSACTION",r,use_awaitable);
    try {
        if(mutation) {
            if(!cmd.actor() || cmd.clientid().empty() || cmd.clientid().size()>64) throw Error("INVALID_REQUEST_ID","缺少有效请求标识");
            // Persist request and response atomically. Hex encoding avoids binary charset conversion.
            auto payload=hex(cmd.SerializeAsString());
            co_await query(c,"INSERT INTO im_group_requests(actor,client_id,payload,reply) VALUES({},{},{},'') ON DUPLICATE KEY UPDATE client_id=im_group_requests.client_id",cmd.actor(),cmd.clientid(),payload);
            auto old=co_await query(c,"SELECT payload,reply FROM im_group_requests WHERE actor={} AND client_id={} FOR UPDATE",cmd.actor(),cmd.clientid());
            if(str(old.rows()[0][0])!=payload) throw Error("IDEMPOTENCY_CONFLICT","请求标识重复且内容不同");
            if(!str(old.rows()[0][1]).empty()) {
                if(!reply.ParseFromString(unhex(str(old.rows()[0][1])))) throw Error("CACHE_ERROR","无法恢复请求结果");
                co_await finish(c,true); co_return reply;
            }
        }
        reply=co_await perform(c,cmd);
        if(mutation) co_await query(c,"UPDATE im_group_requests SET reply={} WHERE actor={} AND client_id={}",hex(reply.SerializeAsString()),cmd.actor(),cmd.clientid());
        co_await finish(c,true); co_return reply;
    } catch(...) { failure=std::current_exception(); }
    co_await finish(c,false);
    std::rethrow_exception(failure);
}
awaitable<groupchat::Notice> buildDelivery(Connection& c,uint64_t actor,uint64_t id) {
    groupchat::Notice notice;
    auto state=co_await query(c,"SELECT g.last_seq,g.member_count,m.ack_seq,m.epoch FROM im_groups g JOIN im_group_members m ON g.id=m.group_id WHERE g.id={} AND m.account={} AND m.active=1 AND g.dismissed=0 FOR UPDATE",id,actor);
    if(state.rows().empty()) co_return notice;
    notice.set_groupid(std::to_string(id)); notice.set_lastseq(number(state.rows()[0][0])); notice.set_epoch(number(state.rows()[0][3]));
    if(number(state.rows()[0][1])<=200) {
        auto messages=co_await query(c,"SELECT group_id,seq,id,client_id,sender,nickname,text,created_time FROM im_group_messages WHERE group_id={} AND seq>{} ORDER BY seq LIMIT 20",id,number(state.rows()[0][2]));
        for(auto row:messages.rows()) *notice.add_messages()=messageRow(row);
    }
    co_return notice;
}
}
}
boost::asio::awaitable<void> MySQLHandler::InitializeGroups() {
    using namespace group_storage;
    {
    auto c=co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    // Embedded schema allows startup from any working directory; standalone SQL is provided too.
    const std::vector<std::string> statements={
        "CREATE TABLE IF NOT EXISTS im_groups(id BIGINT UNSIGNED PRIMARY KEY,name VARCHAR(256) NOT NULL,owner BIGINT UNSIGNED NOT NULL,member_count BIGINT UNSIGNED NOT NULL DEFAULT 1,dismissed TINYINT NOT NULL DEFAULT 0,last_seq BIGINT UNSIGNED NOT NULL DEFAULT 0) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS im_group_members(group_id BIGINT UNSIGNED NOT NULL,account BIGINT UNSIGNED NOT NULL,active TINYINT NOT NULL DEFAULT 1,epoch BIGINT UNSIGNED NOT NULL DEFAULT 1,join_seq BIGINT UNSIGNED NOT NULL DEFAULT 0,ack_seq BIGINT UNSIGNED NOT NULL DEFAULT 0,read_seq BIGINT UNSIGNED NOT NULL DEFAULT 0,PRIMARY KEY(group_id,account),KEY account_groups(account,group_id),FOREIGN KEY(group_id) REFERENCES im_groups(id)) ENGINE=InnoDB",
        "CREATE TABLE IF NOT EXISTS im_group_applications(id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,group_id BIGINT UNSIGNED NOT NULL,account BIGINT UNSIGNED NOT NULL,reason VARCHAR(512) NOT NULL,status TINYINT NOT NULL DEFAULT 0,created_time BIGINT NOT NULL,KEY group_page(group_id,id),KEY applicant(account,id),FOREIGN KEY(group_id) REFERENCES im_groups(id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS im_group_messages(group_id BIGINT UNSIGNED NOT NULL,seq BIGINT UNSIGNED NOT NULL,id VARCHAR(64) NOT NULL,client_id VARCHAR(64) NOT NULL,sender BIGINT UNSIGNED NOT NULL,nickname VARCHAR(256) NOT NULL,text MEDIUMTEXT NOT NULL,created_time BIGINT NOT NULL,PRIMARY KEY(group_id,seq),UNIQUE KEY message_id(id),UNIQUE KEY sender_request(group_id,sender,client_id),KEY sender_progress(group_id,sender,seq),FOREIGN KEY(group_id) REFERENCES im_groups(id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS im_group_requests(actor BIGINT UNSIGNED NOT NULL,client_id VARCHAR(64) NOT NULL,payload MEDIUMTEXT NOT NULL,reply MEDIUMTEXT NOT NULL,PRIMARY KEY(actor,client_id)) ENGINE=InnoDB DEFAULT CHARSET=ascii"
    };
    results r;
    for(const auto& sql:statements) co_await c->async_execute(sql,r,use_awaitable);
    co_await c->async_execute("CREATE TABLE IF NOT EXISTS im_group_numbers(group_id BIGINT UNSIGNED PRIMARY KEY,number BIGINT UNSIGNED NOT NULL UNIQUE,FOREIGN KEY(group_id) REFERENCES im_groups(id)) ENGINE=InnoDB",r,use_awaitable);
    co_await c->async_execute("CREATE TABLE IF NOT EXISTS im_group_number_cursor(id TINYINT PRIMARY KEY,next_number BIGINT UNSIGNED NOT NULL) ENGINE=InnoDB",r,use_awaitable);
    co_await c->async_execute("INSERT IGNORE INTO im_group_number_cursor VALUES(1,1000000000)",r,use_awaitable);
    }
    co_await EnsureGroupNumbers();
}
boost::asio::awaitable<groupchat::Reply> MySQLHandler::ExecuteGroupCommand(groupchat::Command cmd) {
    using namespace group_storage;
    try { 
        co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),executeOnPool(std::move(cmd)),use_awaitable); 
    } catch(const Error& e) { 
        groupchat::Reply reply; 
        reply.set_errorcode(e.code); 
        reply.set_errormessage(e.what()); 
        co_return reply; 
    } catch(const std::exception& e) { 
        spdlog::error("group command: {}",e.what()); 
        groupchat::Reply reply; reply.set_errorcode("DB_ERROR"); 
        reply.set_errormessage("群服务暂时不可用，请稍后重试"); 
        co_return reply; 
    }
}
boost::asio::awaitable<groupchat::Notice> MySQLHandler::WithGroupDelivery(uint64_t actor,std::string id,
    std::function<void(const groupchat::Notice&)> action) {
    using namespace group_storage;
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        [](uint64_t account,std::string group,std::function<void(const groupchat::Notice&)> push)->awaitable<groupchat::Notice> {

            auto c=co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
            results r; 
            co_await c->async_execute("START TRANSACTION",r,use_awaitable);
            std::exception_ptr failure; groupchat::Notice notice;
            try { 
                notice=co_await buildDelivery(*c.operator->(),account,groupId(group)); 
                if(push && !notice.groupid().empty()) {
                    push(notice);
                }
                co_await finish(*c.operator->(),true); 
            } catch(...) { 
                failure=std::current_exception(); 
            }
            if(failure) { 
                co_await finish(*c.operator->(),false); std::rethrow_exception(failure); 
            }
            co_return notice;
        }(actor,std::move(id),std::move(action)),use_awaitable);
}

boost::asio::awaitable<groupchat::Notice> MySQLHandler::GroupDeliveryBatch(uint64_t actor,std::string id) {
    co_return co_await WithGroupDelivery(actor,std::move(id),{});
}

boost::asio::awaitable<std::vector<uint64_t>> MySQLHandler::GroupNotificationAccounts(std::string id) {
    using namespace group_storage;
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        [](std::string group)->awaitable<std::vector<uint64_t>> {
            auto c=co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
            auto rows=co_await query(*c.operator->(),"SELECT account FROM im_group_members WHERE group_id={} UNION SELECT owner FROM im_groups WHERE id={}",groupId(group),groupId(group));
            std::vector<uint64_t> accounts;
            for(auto row:rows.rows()) accounts.push_back(group_storage::number(row[0]));
            co_return accounts;
        }(std::move(id)),use_awaitable);
}
boost::asio::awaitable<std::vector<std::string>> MySQLHandler::PendingGroupsForDelivery(uint64_t actor) {
    using namespace group_storage;
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        [](uint64_t account)->awaitable<std::vector<std::string>> {
            auto c=co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
            auto rows=co_await query(*c.operator->(),"SELECT g.id FROM im_groups g JOIN im_group_members m ON g.id=m.group_id WHERE m.account={} AND m.active=1 AND g.dismissed=0 AND g.last_seq>m.ack_seq",account);
            std::vector<std::string> ids;
            for(auto row:rows.rows()) ids.push_back(std::to_string(group_storage::number(row[0])));
            co_return ids;
        }(actor),use_awaitable);
}

boost::asio::awaitable<void> MySQLHandler::EnsureGroupNumbers() {
    using namespace group_storage;
    auto c=co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    auto missing=co_await query(*c.operator->(),"SELECT g.id FROM im_groups g LEFT JOIN im_group_numbers n ON g.id=n.group_id WHERE n.group_id IS NULL ORDER BY g.id");
    std::vector<uint64_t> ids;
    for(auto row:missing.rows()) ids.push_back(group_storage::number(row[0]));
    for(auto id:ids) {
        results r; std::exception_ptr failure;
        co_await c->async_execute("SET TRANSACTION ISOLATION LEVEL READ COMMITTED",r,use_awaitable);
        co_await c->async_execute("START TRANSACTION",r,use_awaitable);
        try {
            co_await query(*c.operator->(),"SELECT next_number FROM im_group_number_cursor WHERE id=1 FOR UPDATE");
            auto exists=co_await query(*c.operator->(),"SELECT number FROM im_group_numbers WHERE group_id={}",id);
            if(exists.rows().empty()) co_await allocateGroupNumber(*c.operator->(),id);
            co_await finish(*c.operator->(),true);
        } catch(...) { failure=std::current_exception(); }
        if(failure) { co_await finish(*c.operator->(),false); std::rethrow_exception(failure); }
    }
}
