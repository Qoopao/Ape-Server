#include "storage/mysqlhandler.h"
#include "om/obs_metrics.h"
#include "om/trace_context.h"
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
#include <string_view>


namespace {
// 定义在下方匿名 namespace；这里先声明，供较前的好友删除逻辑使用。
boost::asio::awaitable<int64_t> currentConversationSeq(
    boost::mysql::any_connection& connection, const std::string& conversation);
}

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
                const auto conversation = std::to_string(std::min(command.actor(), target)) + "_" +
                    std::to_string(std::max(command.actor(), target));
                const auto deletedSeq = co_await currentConversationSeq(connection, conversation);
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
                        "SELECT 1 FROM im_conversation_receipts WHERE owner_account={} "
                        "AND conversation_id={} AND delivered_seq>={}",
                        message.recvid(), message.convid(), message.seq()), r, use_awaitable);
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
    // REPEATABLE READ 下，缺失行的 SELECT ... FOR UPDATE 会取 gap lock；
    // 发送/ACK 并发时会在 im_outbox 唯一索引上互相死锁。
    // 这里统一用 READ COMMITTED：不取 gap lock，幂等仍由唯一键保证。
    co_await connection.async_execute(
        "SET TRANSACTION ISOLATION LEVEL READ COMMITTED", r, use_awaitable);
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
    // 单聊会话级 seq 计数器：双向共用，删除会话时也保留（避免重建会话后 seq 回退）
    co_await connection->async_execute(R"sql(CREATE TABLE IF NOT EXISTS im_conversation_seq (
        conversation_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,
        next_seq BIGINT UNSIGNED NOT NULL DEFAULT 0
    ) ENGINE=InnoDB DEFAULT CHARSET=ascii)sql", r, use_awaitable);
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

boost::asio::awaitable<std::vector<conversation::Conversation>>
MySQLHandler::ListRecentConversations(uint64_t owner, uint32_t limit)
{
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(1),
        [owner, limit]() -> boost::asio::awaitable<std::vector<conversation::Conversation>> {
            auto connection = co_await MySQLConnector::instance().get_pool(1).async_get_connection(use_awaitable);
            results r;
            const auto sql = "SELECT " + conversationColumns + " FROM im_conversations" +
                boost::mysql::format_sql(connection->format_opts().value(),
                    " WHERE owner_account={} AND is_hidden=0 ORDER BY latest_time DESC,conversation_id DESC LIMIT {}",
                    owner, limit);
            co_await connection->async_execute(sql, r, use_awaitable);
            std::vector<conversation::Conversation> items;
            items.reserve(r.rows().size());
            for (auto row : r.rows())
                items.push_back(conversationRow(row));
            co_return items;
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
    "id,operation,actor_account,peer_account,conversation_id,server_msg_id,seq,payload,state,stage,attempts,lease_token,dispatch_done,traceparent";

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
    task.traceparent = std::string(row[13].as_string());
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
boost::asio::awaitable<bool> areFriends(boost::mysql::any_connection& connection,
    uint64_t actor, uint64_t peer)
{
    results r;
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "SELECT COUNT(*) FROM friends WHERE (owner_account={} AND friend_account={}) "
        "OR (owner_account={} AND friend_account={})", actor, peer, peer, actor), r, use_awaitable);
    co_return actor != peer && unsigned_field(r.rows()[0][0], "friends.count") == 2;
}

// 好友删除水位（会话 seq 空间）
boost::asio::awaitable<int64_t> deletionSeq(boost::mysql::any_connection& connection,
    uint64_t actor, uint64_t peer)
{
    results r;
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "SELECT deleted_seq FROM friend_chat_deletions WHERE owner_account={} AND peer_account={}",
        actor, peer), r, use_awaitable);
    co_return r.rows().empty() ? 0 : static_cast<int64_t>(number(r.rows()[0][0]));
}

boost::asio::awaitable<bool> friendAllows(boost::mysql::any_connection& connection,
    uint64_t actor, uint64_t peer, int64_t seq)
{
    if (!co_await areFriends(connection, actor, peer))
        co_return false;
    const auto cutoff = co_await deletionSeq(connection, actor, peer);
    co_return seq > cutoff;
}

// 单聊每个会话一个稠密 seq：双向共用，严格 +1。分配必须与消息落库在同一事务。
boost::asio::awaitable<int64_t> allocateConversationSeq(
    boost::mysql::any_connection& connection, const std::string& conversation)
{
    results r;
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "INSERT IGNORE INTO im_conversation_seq(conversation_id,next_seq) VALUES({},0)",
        conversation), r, use_awaitable);
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "UPDATE im_conversation_seq SET next_seq=LAST_INSERT_ID(next_seq+1) "
        "WHERE conversation_id={}", conversation), r, use_awaitable);
    co_await connection.async_execute("SELECT LAST_INSERT_ID()", r, use_awaitable);
    co_return static_cast<int64_t>(number(r.rows()[0][0]));
}

// 当前会话已分配到的最后一个 seq（未分配过则为 0）
boost::asio::awaitable<int64_t> currentConversationSeq(
    boost::mysql::any_connection& connection, const std::string& conversation)
{
    results r;
    co_await connection.async_execute(boost::mysql::format_sql(connection.format_opts().value(),
        "SELECT next_seq FROM im_conversation_seq WHERE conversation_id={}",
        conversation), r, use_awaitable);
    co_return r.rows().empty() ? 0 : static_cast<int64_t>(number(r.rows()[0][0]));
}

// 单聊 conversationID 必须是 "digits_digits"（min(uid)_max(uid)）。
// 只接受 ASCII 数字：std::stoull 会宽松接受前导空格/符号和尾随字符，
// 不能直接用来判定协议是否合法（"1_2_3"、"_"、"abc"、" 1_2" 都必须拒绝）。
bool parseConversationID(const std::string& conversation, uint64_t& first, uint64_t& second)
{
    const auto sep = conversation.find('_');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= conversation.size() ||
        conversation.find('_', sep + 1) != std::string::npos)
        return false;
    const auto digits = [](std::string_view text) {
        return !text.empty() && std::all_of(text.begin(), text.end(),
            [](unsigned char c) { return c >= '0' && c <= '9'; });
    };
    const std::string_view text(conversation);
    if (!digits(text.substr(0, sep)) || !digits(text.substr(sep + 1)))
        return false;
    try {
        first = std::stoull(std::string(text.substr(0, sep)));
        second = std::stoull(std::string(text.substr(sep + 1)));
    } catch (const std::exception&) {
        return false;
    }
    return first > 0 && second > 0;
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
        traceparent VARCHAR(96) CHARACTER SET ascii NOT NULL DEFAULT '',
        created_at BIGINT NOT NULL,
        updated_at BIGINT NOT NULL,
        UNIQUE KEY outbox_dedupe(operation,dedupe_key),
        KEY outbox_ready(state,available_at,id),
        KEY outbox_lease(state,lease_until,id),
        KEY outbox_message(server_msg_id,operation),
        KEY outbox_pair(conversation_id,operation,seq),
        KEY outbox_retention(state,operation,updated_at,id)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)sql", r, use_awaitable);

    // 老库补 traceparent 列：异步投递链路的 origin trace（可空、默认空串）。
    co_await connection->async_execute(
        "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema=DATABASE() "
        "AND table_name='im_outbox' AND column_name='traceparent'", r, use_awaitable);
    if (number(r.rows()[0][0]) == 0)
        co_await connection->async_execute(
            "ALTER TABLE im_outbox ADD COLUMN traceparent VARCHAR(96) CHARACTER SET ascii "
            "NOT NULL DEFAULT '' AFTER last_error", r, use_awaitable);

    // 清理线程按 (state,operation,updated_at,id) 扫描，避免全表扫描（11.1）。
    // CREATE TABLE 里已带该索引（新库直接生效）；老库在启动时幂等补建。
    co_await connection->async_execute(
        "SELECT COUNT(*) FROM information_schema.statistics WHERE table_schema=DATABASE() "
        "AND table_name='im_outbox' AND index_name='outbox_retention'", r, use_awaitable);
    if (number(r.rows()[0][0]) == 0)
        co_await connection->async_execute(
            "ALTER TABLE im_outbox ADD KEY outbox_retention(state,operation,updated_at,id)", r, use_awaitable);
    // 领取按 operation 过滤：MsgService/GroupService 各扫自己的任务（outbox_op_ready）。
    co_await connection->async_execute(
        "SELECT COUNT(*) FROM information_schema.statistics WHERE table_schema=DATABASE() "
        "AND table_name='im_outbox' AND index_name='outbox_op_ready'", r, use_awaitable);
    if (number(r.rows()[0][0]) == 0)
        co_await connection->async_execute(
            "ALTER TABLE im_outbox ADD KEY outbox_op_ready(operation,state,available_at,id)", r, use_awaitable);
    co_await connection->async_execute(R"sql(CREATE TABLE IF NOT EXISTS im_conversation_receipts (
        owner_account BIGINT UNSIGNED NOT NULL,
        conversation_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        delivered_seq BIGINT NOT NULL DEFAULT 0,
        updated_at BIGINT NOT NULL,
        PRIMARY KEY(owner_account, conversation_id)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)sql", r, use_awaitable);
}

boost::asio::awaitable<std::vector<MySQLHandler::OutboxBacklog>>
MySQLHandler::CollectOutboxBacklog(int pool)
{
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(pool),
        []() -> boost::asio::awaitable<std::vector<MySQLHandler::OutboxBacklog>> {
            auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
            results r;
            co_await connection->async_execute(
                "SELECT operation,COUNT(*),"
                "CAST(IFNULL(TIMESTAMPDIFF(SECOND,FROM_UNIXTIME(MIN(created_at)/1000),NOW()),0) AS SIGNED) "
                "FROM im_outbox WHERE state IN (0,1) GROUP BY operation", r, use_awaitable);
            std::vector<MySQLHandler::OutboxBacklog> result;
            result.reserve(r.rows().size());
            for (auto row : r.rows())
            {
                MySQLHandler::OutboxBacklog item;
                item.operation = static_cast<int>(unsigned_field(row[0], "outbox.operation"));
                item.pending = static_cast<int64_t>(unsigned_field(row[1], "outbox.pending"));
                item.oldest_age_seconds = static_cast<int64_t>(number(row[2]));
                result.push_back(item);
            }
            co_return result;
        }, use_awaitable);
}

boost::asio::awaitable<MySQLHandler::OutboxTask> MySQLHandler::QueueSingleMessage(
    sdkws::MsgData message, std::string traceparent)
{
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        [message = std::move(message), traceparent = std::move(traceparent)]() mutable -> boost::asio::awaitable<OutboxTask> {
            // 单聊消息合法性（seq 由服务端分配，不读取调用方的 seq）
            if (!message.sendid() || !message.recvid() || message.sendid() == message.recvid() ||
                message.isgroupmsg() || message.sessiontype() != 1 ||
                message.convid().empty() || message.clientmsgid().empty() ||
                message.clientmsgid().size() > 128 || message.content().size() > 64 * 1024 ||
                std::any_of(message.clientmsgid().begin(), message.clientmsgid().end(),
                    [](unsigned char c) { return c < 33 || c > 126; })) {
                throw std::invalid_argument("Invalid single-chat message");
            }

            auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
            OutboxTask task;
            co_await conversationTransaction(*connection.operator->(), [&]() -> boost::asio::awaitable<bool> {
                co_await lockPair(*connection.operator->(), message.sendid(), message.recvid());

                // 幂等：同一 (发送方, clientMsgID) 只分配一次 seq
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
                    co_return true;
                }

                // 先确认好友关系（不含删除水位），避免给非法消息消耗 seq
                if (!co_await areFriends(*connection.operator->(), message.sendid(), message.recvid()))
                    throw std::invalid_argument("Only friends may exchange messages");

                const auto seq = co_await allocateConversationSeq(*connection.operator->(), message.convid());
                message.set_seq(seq);
                if (seq <= co_await deletionSeq(*connection.operator->(), message.sendid(), message.recvid()))
                    throw std::invalid_argument("Message is older than the friend deletion boundary");

                std::string payload;
                if (!google::protobuf::util::MessageToJsonString(message, &payload).ok())
                    throw std::invalid_argument("Invalid message payload");
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "INSERT INTO im_outbox(operation,dedupe_key,actor_account,peer_account,conversation_id,server_msg_id,"
                    "seq,payload,traceparent,created_at,updated_at) VALUES(1,{},{},{},{},{},{},{},{},{},{})",
                    key, message.sendid(), message.recvid(), message.convid(), message.servermsgid(), message.seq(),
                    payload, traceparent, now(), now()), r, use_awaitable);
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

boost::asio::awaitable<MySQLHandler::OutboxTask> MySQLHandler::GetOutboxTask(uint64_t id, int pool)
{
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(pool),
        [id, pool]() -> boost::asio::awaitable<OutboxTask> {
            auto connection = co_await MySQLConnector::instance().get_pool(pool).async_get_connection(use_awaitable);
            results r;
            auto query = "SELECT " + outboxColumns + " FROM im_outbox" +
                boost::mysql::format_sql(connection->format_opts().value(), " WHERE id={}", id);
            co_await connection->async_execute(query, r, use_awaitable);
            if (r.rows().empty())
                throw std::runtime_error("Outbox task missing");
            co_return outboxRow(r.rows()[0]);
        }, use_awaitable);
}

// 从数据库里面取任务。operations 限定本服务负责的任务类型：
// MsgService {1,2,3}，GroupService {4}；互不领取对方的行。
boost::asio::awaitable<std::optional<MySQLHandler::OutboxTask>> MySQLHandler::ClaimOutboxTask(
    const std::vector<int>& operations, int pool)
{
    std::string operationFilter;
    for (const auto operation : operations) {
        if (!operationFilter.empty())
            operationFilter += ',';
        operationFilter += std::to_string(operation);
    }
    if (operationFilter.empty())
        co_return std::nullopt;
    const std::string scope = "operation IN (" + operationFilter + ") AND ";
    auto connection = co_await MySQLConnector::instance().get_pool(pool).async_get_connection(use_awaitable);
    std::optional<OutboxTask> task;
    co_await conversationTransaction(*connection.operator->(), [&]() -> boost::asio::awaitable<bool> {
        results r;
        // operation/now() 都是本进程生成的整数；直接拼接避免 format_sql 的常量 SQL 限制。
        const auto timestamp = now();
        const auto query = "SELECT " + outboxColumns + " FROM im_outbox WHERE " + scope +
            "state=0 AND available_at<=" + std::to_string(timestamp) +
            " ORDER BY available_at,id LIMIT 1 FOR UPDATE SKIP LOCKED";
        co_await connection->async_execute(query, r, use_awaitable);
        if (r.rows().empty()) {
            const auto expired = "SELECT " + outboxColumns + " FROM im_outbox WHERE " + scope +
                "state=1 AND lease_until<=" + std::to_string(timestamp) +
                " ORDER BY lease_until,id LIMIT 1 FOR UPDATE SKIP LOCKED";
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

boost::asio::awaitable<void> MySQLHandler::RetryOutboxTask(OutboxTask task, std::string error,
    int pool, int64_t delayMs)
{
    auto connection = co_await MySQLConnector::instance().get_pool(pool).async_get_connection(use_awaitable);
    results r;
    const auto delay = delayMs > 0
        ? delayMs
        : std::min<int64_t>(60000, int64_t{1000} << std::min(task.attempts, 6));
    co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
        "UPDATE im_outbox SET state=0,available_at={},lease_token='',last_error={},updated_at={} "
        "WHERE id={} AND state=1 AND lease_token={}", now() + delay, error.substr(0, 1500), now(),
        task.id, task.leaseToken), r, use_awaitable);
}

boost::asio::awaitable<bool> MySQLHandler::AckConversation(uint64_t owner,
    std::string conversation, int64_t upToSeq)
{
    if (!owner || conversation.empty() || upToSeq <= 0)
        throw std::invalid_argument("Invalid conversation acknowledgement");

    // conversation = "min_max"；owner 必须是会话中的一方。
    // 格式非法是协议错误（INVALID_ARGUMENT → 网关断连）；
    // 非参与者、会话已删除属于过期 ACK，按 no-op 成功处理，不断连（7.4）。
    uint64_t first = 0, second = 0;
    if (!parseConversationID(conversation, first, second))
        throw std::invalid_argument("Invalid conversation identifier");
    if (owner != first && owner != second)
        co_return true;

    auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    co_await conversationTransaction(*connection.operator->(), [&]() -> boost::asio::awaitable<bool> {
        results r;
        // 先裁剪到已提交 max_seq；会话不存在/未提交消息时 no-op。
        co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
            "SELECT max_seq FROM im_conversations WHERE owner_account={} AND conversation_id={}",
            owner, conversation), r, use_awaitable);
        if (r.rows().empty())
            co_return true;
        const int64_t maxSeq = static_cast<int64_t>(number(r.rows()[0][0]));
        const int64_t seq = std::min(upToSeq, maxSeq);
        if (seq <= 0)
            co_return true;

        // 原子推进水位：不再对“不存在的回执行”做 SELECT ... FOR UPDATE。
        // affected_rows: 1=新插入，2=水位推进，0=低水位/重复 ACK。
        results tx;
        co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
            "INSERT INTO im_conversation_receipts(owner_account,conversation_id,delivered_seq,updated_at) "
            "VALUES({},{},{},{}) ON DUPLICATE KEY UPDATE "
            "delivered_seq=GREATEST(delivered_seq,VALUES(delivered_seq)),updated_at=VALUES(updated_at)",
            owner, conversation, seq, now()), tx, use_awaitable);
        if (tx.affected_rows() == 0)
            co_return true;

        co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
            "INSERT INTO im_outbox(operation,dedupe_key,actor_account,peer_account,conversation_id,"
            "server_msg_id,seq,payload,created_at,updated_at) "
            "VALUES(3,{},{},{},{},'',{},'',{},{}) ON DUPLICATE KEY UPDATE id=im_outbox.id",
            "ack:" + std::to_string(owner) + ":" + conversation + ":" + std::to_string(seq),
            owner, owner, conversation, seq, now(), now()), tx, use_awaitable);
        co_return true;
    });
    co_return true;
}

// 按 operation 分批删除已完成的 outbox 行（11.1）：每批一条 DELETE ... LIMIT，
// 避免长事务与全表扫描。多实例同时删除同一行是幂等的（同一行只会被一个实例删到）。
boost::asio::awaitable<uint64_t> MySQLHandler::PurgeCompletedOutbox(int operation,
    int64_t cutoff, int batchLimit, int maxBatches)
{
    if (operation <= 0 || batchLimit <= 0 || maxBatches <= 0)
        co_return 0;
    auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    uint64_t deleted = 0;
    for (int batch = 0; batch < maxBatches; ++batch) {
        results r;
        co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
            "DELETE FROM im_outbox WHERE state IN (2,3) AND operation={} AND updated_at<{} "
            "ORDER BY id LIMIT {}", operation, cutoff, batchLimit), r, use_awaitable);
        const auto affected = r.affected_rows();
        deleted += affected;
        if (affected < static_cast<uint64_t>(batchLimit))
            break;      // 本轮已经没有更多可删的行
    }
    co_return deleted;
}

// 这个设计是为了把请求的执行变成数据库里面可恢复的流程
// 如果请求的流程（如redis写失败）因为某个环节中断了，可以通过各个流程组件的幂等性和task的状态进行恢复（重复执行）
boost::asio::awaitable<void> MySQLHandler::ProcessOutboxTask(OutboxTask task, IMessageProducer& producer, int pool)
{
    auto connection = co_await MySQLConnector::instance().get_pool(pool).async_get_connection(use_awaitable);
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
            task.traceparent = current.traceparent;
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

                // 异步投递尝试：新 trace + link 指回受理 span（traceparent 随 outbox 行落库）。
                ape::otel::SpanHandle publish_span;
                if (!persist)
                {
                    publish_span = ape::otel::StartLinkedSpan(
                        ape::otel::TraceContext::FromTraceParent(task.traceparent),
                        "outbox.publish.op1",
                        {{"im.operation", std::to_string(task.operation)},
                         {"im.server_msg_id", task.serverId},
                         {"im.conversation_id", task.conversation},
                         {"outbox.attempts", std::to_string(task.attempts)}},
                        opentelemetry::trace::SpanKind::kProducer);
                }

                // 设置消息为可投递
                co_await MongoHandler::PublishMessage(task.serverId);
                // 查询客户端是否已经发送这条消息的ACK
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "SELECT 1 FROM im_conversation_receipts WHERE owner_account={} "
                    "AND conversation_id={} AND delivered_seq>={}",
                    task.peer, task.conversation, task.seq), r, use_awaitable);
                if (r.rows().empty()) {
                    if (!co_await RedisHandler::SaveMsg(message))
                        throw std::runtime_error("Redis message caching failed");
                    // 带确认投递成功状态的投递；Kafka value 携带投递尝试的 traceparent。
                    const auto kafka_value =
                        ape::otel::EncodeKafkaValue(publish_span.context(), task.serverId);
                    if (!co_await producer.deliverConfirmed(task.conversation, kafka_value))
                        throw std::runtime_error("Kafka broker did not confirm delivery");
                    // 更新任务状态为待执行（可以被worker领取），因为还没确认消息是否完成分发（进入离线流程/收到ACK），保留重试的能力
                    co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                        "UPDATE im_outbox SET state=0,available_at={},lease_token='',last_error='',updated_at={} WHERE id={}",
                        now() + 5000, now(), task.id), r, use_awaitable);
                    publish_span.EndOk();
                    co_return true;
                }
            } else if (task.operation == 2) {       // 删除好友后的历史清理
                co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                    "UPDATE im_outbox SET state=3,payload='',lease_token='',last_error='Friend relationship removed',updated_at={} "
                    "WHERE operation=1 AND conversation_id={} AND seq<={}", now(), task.conversation, task.seq), r, use_awaitable);
                auto ids = co_await MongoHandler::GetSingleChatMessageIDsAsync(task.conversation, task.seq);
                co_await RedisHandler::DeleteSingleChatHistory(task.actor, task.peer, std::move(ids));
                co_await MongoHandler::DeleteSingleChatHistoryAsync(task.conversation, task.seq);
            } else if (task.operation == 3) {       // 客户端 ACK 后的投递状态清理
                if (!co_await MongoHandler::MarkConversationDeliveredUpTo(
                        task.peer, task.conversation, task.seq))
                    throw std::runtime_error("MongoDB acknowledgement update failed");
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

boost::asio::awaitable<bool> MySQLHandler::MessageDispatchDone(uint64_t owner,
    std::string serverId, std::string conversationID, int64_t seq)
{
    co_return co_await boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(1),
        [owner, serverId = std::move(serverId), conversationID = std::move(conversationID),
         seq]() -> boost::asio::awaitable<bool> {
            auto connection = co_await MySQLConnector::instance().get_pool(1).async_get_connection(use_awaitable);
            results r;
            co_await connection->async_execute(boost::mysql::format_sql(connection->format_opts().value(),
                "SELECT 1 FROM im_outbox WHERE operation=1 AND server_msg_id={} AND (state=3 OR dispatch_done=1) "
                "UNION ALL SELECT 1 FROM im_conversation_receipts WHERE owner_account={} "
                "AND conversation_id={} AND delivered_seq>={} LIMIT 1",
                serverId, owner, conversationID, seq), r, use_awaitable);
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
