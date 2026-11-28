// 群聊 v3 集成测试（MySQL-only 部分）：
//   schema（无 im_group_messages / 无 last_seq / 无 ack_seq）
//   群号创建与回填、成员生命周期/epoch/joinSeq、246 发送事务（staging + outbox op=4）
//   幂等、并发 seq 连续、解散准入、旧 250 操作拒绝
// 需要 Mongo 的 245/210/tombstone 用例在 scripts/smoke/group_test.py 中，避免本测试依赖 Mongo。
#include "services/group_service/client.h"
#include "services/group_service/server.h"
#include "storage/mysqlconnector.h"
#include "storage/mysqlhandler.h"
#include "util/snowflake.h"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/mysql/format_sql.hpp>
#include <boost/mysql/results.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using boost::asio::awaitable;
using boost::asio::use_awaitable;
using boost::mysql::results;

namespace {

int serial = 0;

void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}

groupchat::Command command(groupchat::Operation op, uint64_t actor, const std::string &group = {}) {
    groupchat::Command cmd;
    cmd.set_operation(op);
    cmd.set_actor(actor);
    cmd.set_groupid(group);
    cmd.set_limit(50);
    cmd.set_clientid("test-" + std::to_string(++serial));
    return cmd;
}

groupchat::SendGroupMessageReq sendRequest(const std::string &group, uint64_t actor,
                                           const std::string &text, const std::string &clientID) {
    groupchat::SendGroupMessageReq request;
    request.set_groupid(group);
    request.set_actor(actor);
    request.set_clientid(clientID);
    request.set_content(text);
    request.set_contenttype(101);
    return request;
}

awaitable<groupchat::Reply> run(groupchat::Command cmd, const std::string &expected = "0") {
    auto reply = co_await MySQLHandler::ExecuteGroupCommand(std::move(cmd));
    if (reply.errorcode() != expected)
        throw std::runtime_error("expected " + expected + ", got " + reply.errorcode() +
                                 ": " + reply.errormessage());
    co_return reply;
}

awaitable<results> sql(const std::string &statement) {
    auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    results r;
    co_await connection->async_execute(statement, r, use_awaitable);
    co_return r;
}

awaitable<uint64_t> field(const std::string &statement) {
    auto r = co_await sql(statement);
    if (r.rows().empty())
        co_return 0;
    const auto value = r.rows()[0][0];
    co_return value.is_uint64() ? value.as_uint64() : static_cast<uint64_t>(value.as_int64());
}

awaitable<void> tests() {
    {
        auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
        results r;
        co_await connection->async_execute(
            "CREATE TABLE IF NOT EXISTS users(account BIGINT UNSIGNED PRIMARY KEY,"
            "nickname VARCHAR(256) NOT NULL) DEFAULT CHARSET=utf8mb4", r, use_awaitable);
        for (uint64_t i = 1; i <= 205; ++i)
            co_await connection->async_execute(
                boost::mysql::format_sql(connection->format_opts().value(),
                    "INSERT IGNORE INTO users VALUES({},'测试成员')", i), r, use_awaitable);
    }
    co_await MySQLHandler::InitializeOutbox();
    co_await MySQLHandler::InitializeGroups();

    // ── schema：v3 只保留元数据/seq/成员/staging ──
    check(co_await field("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
                         "AND table_name='im_group_messages'") == 0,
          "im_group_messages must not exist");
    check(co_await field("SELECT COUNT(*) FROM information_schema.columns WHERE table_schema=DATABASE() "
                         "AND table_name='im_groups' AND column_name='last_seq'") == 0,
          "im_groups.last_seq must not exist");
    check(co_await field("SELECT COUNT(*) FROM information_schema.columns WHERE table_schema=DATABASE() "
                         "AND table_name='im_group_members' AND column_name='ack_seq'") == 0,
          "im_group_members.ack_seq must not exist");
    check(co_await field("SELECT COUNT(*) FROM information_schema.columns WHERE table_schema=DATABASE() "
                         "AND table_name='im_group_stream' AND column_name='published_seq'") == 1,
          "im_group_stream.published_seq missing");
    check(co_await field("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
                         "AND table_name='im_group_send_staging'") == 1,
          "im_group_send_staging missing");
    std::cout << "PASS v3 schema (stream/published_seq/staging, no im_group_messages/ack_seq)\n";

    // ── 创建/群号/搜索/回填 ──
    auto create = command(groupchat::Create, 1);
    create.set_text("测试群");
    auto created = co_await run(create);
    const auto group = created.groups(0).id();
    const auto publicNumber = created.groups(0).number();
    check(publicNumber.size() == 10, "public group number must have ten digits");
    check(created.groups(0).lastseq() == 0 && created.groups(0).joinseq() == 0,
          "new group must start at seq 0");
    check(co_await field("SELECT epoch FROM im_group_stream WHERE group_id=" + group) == 1,
          "new group epoch must be 1");
    check(co_await field("SELECT member_count FROM im_groups WHERE id=" + group) == 1,
          "new group member_count must be 1");

    auto foundByNumber = co_await run(command(groupchat::Search, 1, publicNumber));
    check(foundByNumber.groups(0).id() == group, "public search changed conversation identity");

    {
        // 老群只有 im_groups 行：EnsureGroupNumbers 必须补群号且不改 identity。
        auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
        results r;
        co_await connection->async_execute(
            "INSERT INTO im_groups(id,name,owner,member_count,dismissed,created_time,updated_at) "
            "VALUES(987654321098765432,'legacy group',1,1,0,0,0)", r, use_awaitable);
        co_await connection->async_execute(
            "INSERT INTO im_group_stream(group_id,next_seq,published_seq,last_msg_time,epoch,dismissed,updated_at) "
            "VALUES(987654321098765432,0,0,0,1,0,0)", r, use_awaitable);
        co_await connection->async_execute(
            "INSERT INTO im_group_members(group_id,account,active,epoch,join_seq,joined_time,left_time) "
            "VALUES(987654321098765432,1,1,1,0,0,0)", r, use_awaitable);
    }
    co_await MySQLHandler::EnsureGroupNumbers();
    auto legacy = co_await run(command(groupchat::Search, 1, "987654321098765432"));
    check(legacy.groups(0).number().size() == 10 &&
              legacy.groups(0).id() == "987654321098765432",
          "legacy group numbering lost identity");
    co_await MySQLHandler::EnsureGroupNumbers();
    auto sameLegacy = co_await run(command(groupchat::Search, 1, legacy.groups(0).number()));
    check(sameLegacy.groups(0).id() == legacy.groups(0).id(),
          "group numbering backfill was not idempotent");
    std::cout << "PASS ten-digit numbers, public search, existing-group backfill\n";

    // ── GroupClient → GroupService Execute RPC 路径 ──
    {
        GroupServiceImpl service("group-structure-test", "127.0.0.1:0");
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        auto server = builder.BuildAndStart();
        check(server != nullptr, "GroupService test server failed");
        grpc::ChannelArguments arguments;
        arguments.SetInt("grpc.enable_http_proxy", 0);
        GroupClient client(grpc::CreateCustomChannel("127.0.0.1:" + std::to_string(port),
            grpc::InsecureChannelCredentials(), arguments));
        auto reply = co_await client.Execute(command(groupchat::Search, 1, group));
        check(reply.errorcode() == "0" && reply.groups_size() == 1 &&
                  reply.groups(0).id() == group,
              "GroupClient/GroupService Execute RPC failed");
        // 246 走 RPC：发送事务只写 MySQL，不需要 Mongo。
        auto sent = co_await client.SendGroupMessages(
            sendRequest(group, 1, "rpc message", "rpc-1"));
        check(sent.errorcode() == "0" && sent.msg().seq() == 1,
              "GroupClient/GroupService SendGroupMessages RPC failed");
        server->Shutdown();
        server->Wait();
    }
    std::cout << "PASS GroupClient -> GroupService Execute/Send RPC path\n";

    // ── 命令幂等 ──
    auto replay = co_await run(create);
    check(replay.groups(0).id() == group, "create replay duplicated group");
    create.set_text("changed");
    co_await run(create, "IDEMPOTENCY_CONFLICT");

    // ── 成员边界：非成员不能发送；入群 joinSeq=next_seq ──
    auto nonMember = co_await MySQLHandler::SendGroupMessage(sendRequest(group, 2, "before join", "before-join"));
    check(nonMember.errorCode == "NOT_MEMBER", "non-member send must be rejected");
    auto apply = command(groupchat::Apply, 2, group);
    apply.set_text("申请说明");
    co_await run(apply);
    co_await run(command(groupchat::Apply, 2, group));
    auto pending = co_await run(command(groupchat::Applications, 1, group));
    check(pending.applications_size() == 1, "duplicate applications created multiple pending rows");
    auto review = command(groupchat::Review, 2, group);
    review.set_applicationid(pending.applications(0).id());
    review.set_accept(true);
    co_await run(review, "NOT_MEMBER");
    review = command(groupchat::Review, 1, group);
    review.set_applicationid(pending.applications(0).id());
    review.set_accept(true);
    co_await run(review);
    co_await run(review);
    auto joined = co_await run(command(groupchat::Search, 2, group));
    check(joined.groups(0).joinseq() == 1 && joined.groups(0).membercount() == 2,
          "join sequence/member count wrong");
    const auto epochAfterJoin = joined.groups(0).epoch();
    auto memberAccess = co_await MySQLHandler::GroupAccess(group, 2);
    check(memberAccess.errorCode == "0" && memberAccess.group.joinSeq == 1,
          "GroupAccess returned wrong joinSeq for member");

    // ── 246 发送事务：staging 正文 + outbox 引用，提交即返回 ──
    auto sent = co_await MySQLHandler::SendGroupMessage(sendRequest(group, 2, "after join", "after-join"));
    check(sent.errorCode == "0" && sent.msg.seq() == 2, "member send failed");
    check(sent.msg.recvid() == 0 && sent.msg.sessiontype() == 2 && sent.msg.isgroupmsg(),
          "group MsgData must be recvID=0/sessionType=2/isGroupMsg");
    check(co_await field("SELECT COUNT(*) FROM im_group_send_staging WHERE server_msg_id='" +
                         sent.msg.servermsgid() + "'") == 1,
          "staging row missing after send");
    check(co_await field("SELECT COUNT(*) FROM im_outbox WHERE operation=4 AND server_msg_id='" +
                         sent.msg.servermsgid() + "' AND stage=0 AND state=0") == 1,
          "op=4 outbox row missing after send");
    check(co_await field("SELECT COUNT(*) FROM im_outbox WHERE operation=4 AND payload LIKE '%content%'") == 0,
          "outbox payload must only store a serverMsgID reference");
    check(co_await field("SELECT next_seq FROM im_group_stream WHERE group_id=" + group) == 2,
          "next_seq must advance on send");
    check(co_await field("SELECT published_seq FROM im_group_stream WHERE group_id=" + group) == 0,
          "published_seq must not advance before the outbox worker runs");

    auto idempotent = co_await MySQLHandler::SendGroupMessage(
        sendRequest(group, 2, "after join", "after-join"));
    check(idempotent.errorCode == "0" && idempotent.msg.seq() == sent.msg.seq() &&
              idempotent.msg.servermsgid() == sent.msg.servermsgid(),
          "idempotent replay returned a different message");
    check(co_await field("SELECT COUNT(*) FROM im_outbox WHERE operation=4 AND conversation_id='" + group + "'") == 2,
          "idempotent replay created a second outbox row");
    auto conflict = co_await MySQLHandler::SendGroupMessage(
        sendRequest(group, 2, "different", "after-join"));
    check(conflict.errorCode == "IDEMPOTENCY_CONFLICT", "clientID reuse with different content must conflict");

    auto empty = co_await MySQLHandler::SendGroupMessage(sendRequest(group, 1, "   ", "empty-1"));
    check(empty.errorCode == "EMPTY_MESSAGE", "blank content must be EMPTY_MESSAGE");
    auto badType = sendRequest(group, 1, "x", "type-1");
    badType.set_contenttype(102);
    check((co_await MySQLHandler::SendGroupMessage(badType)).errorCode == "INVALID_PARAM",
          "unsupported contentType must be INVALID_PARAM");
    check((co_await MySQLHandler::SendGroupMessage(sendRequest("abc", 1, "x", "bad-group"))).errorCode ==
              "INVALID_GROUP", "invalid groupID must be INVALID_GROUP");
    check((co_await MySQLHandler::SendGroupMessage(sendRequest(group, 1, "x", "has space"))).errorCode ==
              "INVALID_REQUEST_ID", "clientID with space must be INVALID_REQUEST_ID");

    // 旧 250 Send/Pull/Ack/Read 必须被拒绝（网关也直接拒绝，服务端兜底）。
    for (int legacyOperation = 11; legacyOperation <= 14; ++legacyOperation) {
        groupchat::Command old;
        old.set_operation(static_cast<groupchat::Operation>(legacyOperation));
        old.set_actor(1);
        old.set_groupid(group);
        old.set_clientid("legacy-" + std::to_string(legacyOperation));
        co_await run(old, "UNSUPPORTED_OPERATION");
    }
    std::cout << "PASS 246 transaction (staging+outbox), idempotency, validation, old-250 rejection\n";

    // ── 并发发送：seq 唯一且连续 ──
    std::vector<std::future<MySQLHandler::GroupSendResult>> futures;
    for (int i = 0; i < 12; ++i)
        futures.push_back(boost::asio::co_spawn(
            MySQLConnector::instance().get_pool_executor(0),
            MySQLHandler::SendGroupMessage(
                sendRequest(group, 1, "concurrent " + std::to_string(i), "concurrent-" + std::to_string(i))),
            boost::asio::use_future));
    std::set<int64_t> sequences;
    // 单线程 executor：不能阻塞 get()，否则被 spawn 的发送没有机会运行。
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor);
    for (auto &future : futures) {
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            timer.expires_after(std::chrono::milliseconds(5));
            co_await timer.async_wait(use_awaitable);
        }
        auto result = future.get();
        check(result.errorCode == "0", ("concurrent send failed: " + result.errorCode).c_str());
        sequences.insert(result.msg.seq());
    }
    check(sequences.size() == 12, "concurrent sends duplicated a seq");
    check(*sequences.begin() == 3 && *sequences.rbegin() == 14, "concurrent seqs are not contiguous");
    check(co_await field("SELECT next_seq FROM im_group_stream WHERE group_id=" + group) == 14,
          "next_seq mismatch after concurrent sends");
    std::cout << "PASS concurrent sends produce unique contiguous seq\n";

    // ── 成员生命周期：踢人/epoch/重入 joinSeq ──
    co_await run(command(groupchat::Dismiss, 2, group), "OWNER_REQUIRED");
    co_await run(command(groupchat::Leave, 1, group), "OWNER_CANNOT_LEAVE");
    auto rename = command(groupchat::Rename, 1, group);
    rename.set_text("测试群");
    co_await run(rename, "NAME_UNCHANGED");
    rename = command(groupchat::Rename, 2, group);
    rename.set_text("改名");
    co_await run(rename, "OWNER_REQUIRED");
    rename = command(groupchat::Rename, 1, group);
    rename.set_text("改名后的群");
    co_await run(rename);
    check((co_await run(command(groupchat::Search, 1, group))).groups(0).name() == "改名后的群",
          "Rename did not persist");

    auto staleLeave = command(groupchat::Leave, 2, group);
    staleLeave.set_epoch(epochAfterJoin - 1);
    co_await run(staleLeave, "MEMBERSHIP_CHANGED");
    auto kick = command(groupchat::Kick, 1, group);
    kick.set_target(2);
    co_await run(kick);
    co_await run(kick);   // 重复踢人幂等
    check((co_await MySQLHandler::SendGroupMessage(sendRequest(group, 2, "after kick", "after-kick"))).errorCode ==
              "NOT_MEMBER", "kicked member can still send");
    check((co_await MySQLHandler::GroupAccess(group, 2)).errorCode == "NOT_MEMBER",
          "kicked member can still read history");

    co_await run(command(groupchat::Apply, 2, group));
    pending = co_await run(command(groupchat::Applications, 1, group));
    auto rejoin = command(groupchat::Review, 1, group);
    rejoin.set_applicationid(pending.applications(0).id());
    rejoin.set_accept(true);
    co_await run(rejoin);
    auto rejoined = co_await run(command(groupchat::Search, 2, group));
    check(rejoined.groups(0).joinseq() == 14 && rejoined.groups(0).epoch() > epochAfterJoin,
          "rejoin must reset joinSeq to next_seq and bump epoch");
    std::cout << "PASS permissions, kick, rejoin joinSeq/epoch, rename\n";

    // ── 201 人群：large 口径、成员分页、解散 ──
    {
        auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
        results r;
        for (uint64_t account = 3; account <= 201; ++account)
            co_await connection->async_execute(
                boost::mysql::format_sql(connection->format_opts().value(),
                    "INSERT INTO im_group_members(group_id,account,active,epoch,join_seq,joined_time,left_time) "
                    "VALUES({},{},1,1,0,0,0) ON DUPLICATE KEY UPDATE active=1", std::stoull(group), account),
                r, use_awaitable);
        co_await connection->async_execute(
            boost::mysql::format_sql(connection->format_opts().value(),
                "UPDATE im_groups SET member_count=(SELECT COUNT(*) FROM im_group_members "
                "WHERE group_id={} AND active=1) WHERE id={}", std::stoull(group), std::stoull(group)),
            r, use_awaitable);
    }
    auto large = co_await run(command(groupchat::Search, 1, group));
    check(large.groups(0).large(), "201-member group must be large");
    auto page = command(groupchat::Members, 1, group);
    page.set_limit(17);
    uint64_t total = 0;
    do {
        auto members = co_await run(page);
        total += members.members_size();
        if (!members.hasmore())
            break;
        page.set_after(members.cursor());
    } while (true);
    check(total == 201, "member pagination lost members");

    co_await run(command(groupchat::Dismiss, 1, group));
    check((co_await MySQLHandler::SendGroupMessage(sendRequest(group, 1, "after dismiss", "dismiss-1"))).errorCode ==
              "GROUP_DISMISSED", "dismissed group still accepts sends");
    check((co_await MySQLHandler::GroupAccess(group, 1)).errorCode == "GROUP_DISMISSED",
          "dismissed group still readable");
    auto dismissed = co_await run(command(groupchat::Search, 1, group));
    check(dismissed.groups(0).dismissed() && !dismissed.groups(0).joined(),
          "dismissal not reflected in group state");
    std::cout << "PASS large threshold, pagination, dismissal truth\n";
}

} // namespace

int main() {
    try {
        Snowflake::init(1, 1);
        boost::mysql::pool_params settings;
        settings.server_address.emplace_host_and_port("127.0.0.1", 33079);
        settings.username = "root";
        settings.database = "im_group_test";
        settings.ssl = boost::mysql::ssl_mode::disable;
        MySQLConnector::instance().start(1, &settings);
        boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0), tests(),
                              boost::asio::use_future).get();
        {
            // 服务生命周期：outbox 由服务持有；无 producer 时 StartOutbox 是安全 no-op。
            GroupServiceImpl service("group-lifecycle-test", "127.0.0.1:0");
            service.StartOutbox();
            service.StopOutbox();
            service.StartOutbox();
            service.StopOutbox();
        }
        std::cout << "PASS service-owned outbox lifecycle\n";
        MySQLConnector::instance().stop();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << e.what() << "\n";
        MySQLConnector::instance().stop();
        return 1;
    }
}
