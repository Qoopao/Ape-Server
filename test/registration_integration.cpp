#include "storage/registration_store.h"
#include "util/account_pool.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql/results.hpp>
#include <cstdlib>
#include <iostream>
#include <set>
#include <vector>

using boost::asio::awaitable;
using boost::asio::use_awaitable;
using Pool = boost::mysql::connection_pool;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
registration::Input input(int i, const std::string& id = "") {
    registration::Input in;
    in.request_id = id.empty() ? "test-" + std::to_string(i) : id;
    in.user.phone = std::to_string(13800000000ULL + i);
    in.user.nickname = "tester";
    in.user.password_salt = "random-test-salt";
    in.user.password_hash = "test-hash";
    in.payload = registration::payload(in.user.phone, in.user.nickname, "abc123");
    return in;
}
awaitable<void> sql(Pool& pool, std::string query) {
    auto c = co_await pool.async_get_connection(use_awaitable);
    boost::mysql::results r;
    co_await c->async_execute(query, r, use_awaitable);
}
awaitable<uint64_t> scalar(Pool& pool, std::string query) {
    auto c = co_await pool.async_get_connection(use_awaitable);
    boost::mysql::results r;
    co_await c->async_execute(query, r, use_awaitable);
    auto f = r.rows()[0][0];
    co_return f.is_uint64() ? f.as_uint64() : static_cast<uint64_t>(f.as_int64());
}
awaitable<void> expect(Pool& pool, registration::Input in, std::string code) {
    bool caught = false;
    try { co_await registration::create(pool, in); }
    catch (const registration::Error& e) { caught = e.code == code; if (!caught) throw; }
    check(caught, "expected registration error missing");
}
awaitable<std::vector<registration::Result>> concurrent(Pool& pool, bool same, int base) {
    auto executor = co_await boost::asio::this_coro::executor;
    std::vector<registration::Result> results(12);
    std::exception_ptr failure;
    int pending = 12;
    for (int i = 0; i < 12; ++i) {
        auto task = [](Pool& p, registration::Input in) -> awaitable<registration::Result> {
            co_return co_await registration::create(p,in);
        };
        boost::asio::co_spawn(executor, task(pool,input(same ? base : base+i)),
            [&,i](std::exception_ptr e, registration::Result r) {
                if (e) failure = e;
                results[i] = r; --pending;
            });
    }
    while (pending) {
        boost::asio::steady_timer timer(executor);
        timer.expires_after(std::chrono::milliseconds(5));
        co_await timer.async_wait(use_awaitable);
    }
    if (failure) std::rethrow_exception(failure);
    co_return results;
}
awaitable<void> tests(Pool& pool, bool lostCommit, bool unconfirmed) {
    if (lostCommit) {
        // TCP fault proxy drops the first registration COMMIT acknowledgement.
        auto in = input(unconfirmed ? 901 : 900);
        if(unconfirmed) {
            co_await expect(pool,in,"REGISTRATION_UNCERTAIN");
            check(co_await scalar(pool,"SELECT COUNT(*) FROM users WHERE phone='13800000901'")==0,
                  "undelivered COMMIT created user");
        }
        auto a = co_await registration::create(pool,in);
        auto b = co_await registration::create(pool,in);
        check(a.account == b.account && a.user_id == b.user_id, "lost COMMIT replay changed result");
        check(co_await scalar(pool,"SELECT COUNT(*) FROM users WHERE phone='"+in.user.phone+"'") == 1, "lost COMMIT duplicated user");
        std::cout << "PASS lost-COMMIT acknowledgement recovery\n";
        co_return;
    }
    auto first = input(1);
    auto a = co_await registration::create(pool,first);
    auto b = co_await registration::create(pool,first);
    check(a.account == b.account && a.user_id == b.user_id, "replay mismatch");
    first.payload += "changed";
    co_await expect(pool,first,"IDEMPOTENCY_CONFLICT");
    co_await expect(pool,input(1,"different-id"),"PHONE_EXISTS");
    {
        unsigned pending=8, successes=0, conflicts=0;
        std::exception_ptr failure;
        auto task=[](Pool& p,registration::Input in)->awaitable<registration::Result>{
            co_return co_await registration::create(p,in);
        };
        for(unsigned i=0;i<8;++i)
            boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
                task(pool,input(5,"phone-race-"+std::to_string(i))),
                [&](std::exception_ptr e,registration::Result){
                    if(!e)++successes;
                    else try{std::rethrow_exception(e);}catch(const registration::Error& x){
                        if(x.code=="PHONE_EXISTS")++conflicts;else failure=e;
                    }catch(...){failure=e;}
                    --pending;
                });
        while(pending){boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
            t.expires_after(std::chrono::milliseconds(5));co_await t.async_wait(use_awaitable);}
        if(failure)std::rethrow_exception(failure);
        check(successes==1 && conflicts==7,"concurrent phone uniqueness failed");
    }
    auto repeated = co_await concurrent(pool,true,10);
    for (const auto& r : repeated) check(r.account == repeated[0].account,"concurrent replay mismatch");
    auto unique = co_await concurrent(pool,false,30);
    std::set<uint64_t> accounts;
    for (const auto& r : unique) accounts.insert(r.account);
    check(accounts.size() == unique.size(), "duplicate account allocation");
    check(co_await scalar(pool,"SELECT COUNT(*) FROM registration_requests WHERE user_id IS NULL") == 0, "pending ledger committed");
    std::cout << "PASS replay, payload conflict, phone uniqueness, concurrent registration\n";

    // Force INSERT failure AFTER an account has been locked.
    const auto before = co_await scalar(pool,"SELECT COUNT(*) FROM account_pool WHERE status=1");
    auto bad = input(80); bad.user.nickname = std::string(1000,'x');
    bool failed = false;
    try { co_await registration::create(pool,bad); } catch (...) { failed = true; }
    check(failed,"forced insert failure missing");
    check(co_await scalar(pool,"SELECT COUNT(*) FROM account_pool WHERE status=1") == before,"failed insert consumed account");
    check(co_await scalar(pool,"SELECT COUNT(*) FROM registration_requests WHERE request_id='test-80'") == 0,"failed insert left ledger");
    std::cout << "PASS transaction rollback on insert failure\n";

    // Real row lock: wait until released, without refilling.
    auto holder = co_await pool.async_get_connection(use_awaitable);
    boost::mysql::results r;
    co_await holder->async_execute("START TRANSACTION",r,use_awaitable);
    co_await holder->async_execute("SELECT id FROM account_pool WHERE status=0 ORDER BY id LIMIT 1 FOR UPDATE",r,use_awaitable);
    const auto cursor = co_await scalar(pool,"SELECT next_start FROM account_pool_cursor WHERE id=1");
    auto delayed = [](boost::mysql::any_connection& c) -> awaitable<void> {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(150)); co_await timer.async_wait(use_awaitable);
        boost::mysql::results out; co_await c.async_execute("ROLLBACK",out,use_awaitable);
    };
    std::exception_ptr releaseError;
    bool released = false;
    boost::asio::co_spawn(co_await boost::asio::this_coro::executor,delayed(*holder.operator->()),
        [&](std::exception_ptr e){ releaseError=e; released=true; });
    co_await registration::create(pool,input(81));
    check(released && !releaseError,"holder release failed");
    check(co_await scalar(pool,"SELECT next_start FROM account_pool_cursor WHERE id=1") == cursor,"temporary lock triggered refill");
    // Keep lock through all three attempts, then verify the whole transaction rolled back.
    co_await holder->async_execute("START TRANSACTION",r,use_awaitable);
    co_await holder->async_execute("SELECT id FROM account_pool WHERE status=0 ORDER BY id LIMIT 1 FOR UPDATE",r,use_awaitable);
    co_await expect(pool,input(82),"REGISTRATION_BUSY");
    co_await holder->async_execute("ROLLBACK",r,use_awaitable);
    check(co_await scalar(pool,"SELECT COUNT(*) FROM registration_requests WHERE request_id='test-82'") == 0,"lock timeout left ledger");
    holder = {};
    std::cout << "PASS waiting on locks and bounded lock-timeout rollback\n";

    // Simulated server deadlock at INSERT; a nontransactional test counter observes retries.
    co_await sql(pool,"CREATE TABLE retry_counter(n INT NOT NULL) ENGINE=MyISAM");
    co_await sql(pool,"INSERT INTO retry_counter VALUES(0)");
    co_await sql(pool,"CREATE TRIGGER force_retry BEFORE INSERT ON users FOR EACH ROW BEGIN "
        "IF NEW.phone='13800000083' THEN UPDATE retry_counter SET n=n+1; "
        "SIGNAL SQLSTATE '40001' SET MYSQL_ERRNO=1213,MESSAGE_TEXT='test deadlock'; END IF; END");
    co_await expect(pool,input(83),"REGISTRATION_BUSY");
    check(co_await scalar(pool,"SELECT n FROM retry_counter") == 3,"deadlock retry bound wrong");
    co_await sql(pool,"DROP TRIGGER force_retry");
    co_await sql(pool,"DROP TABLE retry_counter");
    std::cout << "PASS bounded deadlock-error retries\n";

    // Small low-stock pool; concurrent refills must recheck under the cursor lock.
    co_await sql(pool,"DELETE FROM account_pool WHERE status=0"); // isolated fixture only
    const auto start = co_await scalar(pool,"SELECT next_start FROM account_pool_cursor WHERE id=1");
    co_await sql(pool,"UPDATE account_pool_cursor SET next_start=1000000000 WHERE id=1");
    bool unsafe = false;
    try { co_await registration::refill(pool,10000); }
    catch (const registration::Error& e) { unsafe = e.code=="POOL_INCONSISTENT"; }
    check(unsafe,"unsafe cursor was accepted");
    co_await sql(pool,"UPDATE account_pool_cursor SET next_start="+std::to_string(start)+" WHERE id=1");
    co_await sql(pool,"CREATE TRIGGER fail_refill BEFORE INSERT ON account_pool FOR EACH ROW "
        "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='test refill failure'");
    bool refillFailed = false;
    try { co_await registration::refill(pool,10000); } catch (...) { refillFailed=true; }
    check(refillFailed,"forced refill failure missing");
    check(co_await scalar(pool,"SELECT next_start FROM account_pool_cursor WHERE id=1")==start,"failed refill advanced cursor");
    check(co_await scalar(pool,"SELECT COUNT(*) FROM account_pool WHERE status=0")==0,"failed refill left accounts");
    co_await sql(pool,"DROP TRIGGER fail_refill");
    std::cout << "PASS unsafe-cursor refusal and refill transaction rollback\n";
    unsigned pending = 8; std::exception_ptr failure;
    for (unsigned i=0;i<8;++i)
        boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
            registration::refill(pool,AccountPoolAllocator::kRefillBatch),
            [&](std::exception_ptr e){ if(e)failure=e; --pending; });
    while(pending) { boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
        t.expires_after(std::chrono::milliseconds(5)); co_await t.async_wait(use_awaitable); }
    if(failure)std::rethrow_exception(failure);
    check(co_await scalar(pool,"SELECT next_start FROM account_pool_cursor WHERE id=1") == start+10000,"duplicate refill");
    check(co_await scalar(pool,"SELECT COUNT(*) FROM account_pool WHERE status=0") == 10000,"refill inventory mismatch");
    std::cout << "PASS concurrent refill coordination\n";
    co_await sql(pool,"ALTER TABLE account_pool MODIFY account BIGINT UNSIGNED NOT NULL");
    co_await sql(pool,"ALTER TABLE users MODIFY account BIGINT UNSIGNED NOT NULL");
    const auto unsignedResult=co_await registration::create(pool,input(85));
    check(unsignedResult.account>=1000000000ULL,"unsigned account read failed");
    std::cout << "PASS signed and unsigned MySQL integer columns\n";
}
int main(int argc,char** argv) {
    // Hard-coded isolated test schema/port: never use application configuration.
    const bool unconfirmed = argc>1 && std::string(argv[1])=="--unconfirmed-commit";
    const bool fault = unconfirmed || (argc>1 && std::string(argv[1])=="--lost-commit");
    boost::asio::io_context ioc;
    boost::mysql::pool_params params;
    params.server_address.emplace_host_and_port("127.0.0.1",fault ? 33317 : 33316);
    params.username="root"; params.password="registration-test-only";
    params.database="ape_registration_test"; params.ssl=boost::mysql::ssl_mode::disable;
    params.initial_size=1; params.max_size=20;
    Pool pool(ioc,std::move(params)); pool.async_run(boost::asio::detached);
    int status=0;
    boost::asio::co_spawn(ioc,tests(pool,fault,unconfirmed),[&](std::exception_ptr e){
        if(e){ status=1; try{std::rethrow_exception(e);}catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';} }
        pool.cancel();
    });
    ioc.run(); return status;
}