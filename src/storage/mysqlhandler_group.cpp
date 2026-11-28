// 群聊 v3 存储层：群元数据 / 每群稠密 seq / 成员 / 发送事务 / outbox(op=4)。
//
// 不变量（docs/group/README.md §3）：
//   G1  每群 seq 从 1 严格 +1；stream 单行分配，回滚一并回滚
//   G2  群消息正文在 Mongo 只有一份（isGroupMsg=true，recvID=0）
//   G3  发送事务提交 = 已接受；Mongo/发布由 outbox worker 完成
//   G7  joinSeq = 入群时 next_seq；重入重置
//   G8  epoch 权威在 im_group_stream，成员变更 +1
//   G13 published_seq = 已发布（含 tombstone）的最大 seq
//   G14 放弃 Mongo 写必须先写 tombstone，不能留下不可见空洞
//   G16 outbox payload 只存 serverMsgID 引用，正文只在 staging

#include "storage/mysqlhandler.h"
#include "storage/mysqlconnector.h"
#include "storage/mongohandler.h"
#include "messagequeue/message_producer.h"
#include "om/obs_metrics.h"
#include "om/otel_trace_propagation.h"
#include "om/trace_context.h"
#include "util/account_pool.h"
#include "util/snowflake.h"
#include "util/uuid.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql/common_server_errc.hpp>
#include <boost/mysql/error_with_diagnostics.hpp>
#include <boost/mysql/format_sql.hpp>
#include <google/protobuf/util/json_util.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using boost::asio::awaitable;
using boost::asio::use_awaitable;
using boost::mysql::results;
using Connection = boost::mysql::any_connection;

constexpr uint32_t kGroupInlineMembers = 200;

struct GroupError : std::runtime_error {
    std::string code;
    GroupError(std::string c, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(c)) {}
};

uint64_t number(boost::mysql::field_view f) {
    if (f.is_uint64())
        return f.as_uint64();
    if (f.is_int64())
        return static_cast<uint64_t>(f.as_int64());
    if (f.is_double())
        return static_cast<uint64_t>(f.as_double());
    if (f.is_string()) {
        try {
            return std::stoull(std::string(f.as_string()));
        } catch (const std::exception &) {
        }
    }
    throw std::runtime_error("Invalid MySQL numeric field in group storage");
}

std::string str(boost::mysql::field_view f) {
    return std::string(f.as_string());
}

int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t EnvInt(const char *name, int64_t fallback, int64_t minimum, int64_t maximum) {
    const char *value = std::getenv(name);
    if (!value || !*value)
        return fallback;
    try {
        return std::clamp<int64_t>(std::stoll(value), minimum, maximum);
    } catch (const std::exception &) {
        spdlog::warn("Group config: invalid {}={}, using {}", name, value, fallback);
        return fallback;
    }
}

uint32_t InlineMaxMembers() {
    return static_cast<uint32_t>(EnvInt("GROUP_INLINE_MAX_MEMBERS", kGroupInlineMembers, 1, 100000));
}

uint64_t MaxGroupMembers() {
    return static_cast<uint64_t>(EnvInt("GROUP_MAX_MEMBERS", 2000, 2, 1000000));
}

uint64_t MaxGroupMessageBytes() {
    return static_cast<uint64_t>(EnvInt("GROUP_MAX_MSG_BYTES", 65536, 1, 1024 * 1024));
}

int64_t PublishMaxAttempts() {
    return EnvInt("GROUP_PUBLISH_MAX_ATTEMPTS", 30, 1, 100000);
}

template <class... Args>
awaitable<results> query(Connection &c, boost::mysql::constant_string_view sql, Args &&...args) {
    results r;
    co_await c.async_execute(boost::mysql::format_sql(c.format_opts().value(), sql,
        std::forward<Args>(args)...), r, use_awaitable);
    co_return r;
}

// 失败时回滚并重新抛出；群发送依赖“seq 自增与 staging/outbox 同事务”的原子性。
template <class Action>
awaitable<void> groupTransaction(Connection &c, Action action) {
    results r;
    co_await c.async_execute("SET TRANSACTION ISOLATION LEVEL READ COMMITTED", r, use_awaitable);
    co_await c.async_execute("START TRANSACTION", r, use_awaitable);
    std::exception_ptr failure;
    try {
        co_await action();
        co_await c.async_execute("COMMIT", r, use_awaitable);
        co_return;
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        co_await c.async_execute("ROLLBACK", r, use_awaitable);
    } catch (const std::exception &e) {
        spdlog::error("Group storage rollback failed: {}", e.what());
    }
    std::rethrow_exception(failure);
}

bool duplicateKey(const std::exception_ptr &failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const boost::mysql::error_with_diagnostics &e) {
        return e.code() == boost::mysql::common_server_errc::er_dup_entry;
    } catch (...) {
    }
    return false;
}

uint64_t groupId(const std::string &id) {
    if (id.empty() || id.size() > 20 ||
        id.find_first_not_of("0123456789") != std::string::npos)
        throw GroupError("INVALID_GROUP", "请输入有效的数字群号");
    try {
        const auto value = std::stoull(id);
        if (value)
            return value;
    } catch (const std::exception &) {
    }
    throw GroupError("INVALID_GROUP", "请输入有效的数字群号");
}

bool validClientId(const std::string &id) {
    return !id.empty() && id.size() <= 64 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) { return c >= 33 && c <= 126; });
}

bool blankContent(const std::string &content) {
    return std::all_of(content.begin(), content.end(), [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    });
}

sdkws::MsgData newGroupMessage(uint64_t gid, uint64_t actor, const std::string &nickname,
    const groupchat::SendGroupMessageReq &request, int64_t seq, int64_t sendTime) {
    sdkws::MsgData message;
    message.set_sendid(actor);
    message.set_recvid(0);
    message.set_convid(std::to_string(gid));
    message.set_clientmsgid(request.clientid());
    message.set_servermsgid(uuid::newone_str());
    message.set_sendernickname(nickname);
    message.set_sessiontype(2);
    message.set_msgfrom(100);
    message.set_contenttype(request.contenttype() == 0 ? 101 : request.contenttype());
    message.set_content(request.content());
    message.set_seq(seq);
    message.set_sendtime(static_cast<double>(sendTime));
    message.set_status(1);
    message.set_isgroupmsg(true);
    message.set_isdeleted(false);
    message.set_ext(request.ext());
    return message;
}

bool sameRequest(const sdkws::MsgData &existing, const groupchat::SendGroupMessageReq &request) {
    const int32_t contentType = request.contenttype() == 0 ? 101 : request.contenttype();
    return existing.content() == request.content() &&
        existing.contenttype() == contentType &&
        existing.ext() == request.ext();
}

// ── group 行读取 ──

awaitable<std::optional<MySQLHandler::GroupRecord>> loadGroup(Connection &c, uint64_t gid) {
    auto r = co_await query(c,
        "SELECT g.id,g.name,g.owner,g.member_count,g.dismissed,"
        "CAST(COALESCE(s.next_seq,0) AS UNSIGNED),CAST(COALESCE(s.published_seq,0) AS UNSIGNED),"
        "CAST(COALESCE(s.epoch,1) AS UNSIGNED),CAST(COALESCE(s.last_msg_time,0) AS UNSIGNED),"
        "CAST(COALESCE(s.dismissed,0) AS UNSIGNED),CAST(COALESCE(n.number,0) AS UNSIGNED) "
        "FROM im_groups g LEFT JOIN im_group_stream s ON s.group_id=g.id "
        "LEFT JOIN im_group_numbers n ON n.group_id=g.id WHERE g.id={}", gid);
    if (r.rows().empty())
        co_return std::nullopt;
    const auto row = r.rows()[0];
    MySQLHandler::GroupRecord group;
    group.id = number(row[0]);
    group.name = str(row[1]);
    group.owner = number(row[2]);
    group.memberCount = number(row[3]);
    group.dismissed = number(row[4]) != 0 || number(row[9]) != 0;
    group.nextSeq = number(row[5]);
    group.publishedSeq = number(row[6]);
    group.epoch = number(row[7]);
    group.lastMsgTime = number(row[8]);
    group.number = std::to_string(number(row[10]));
    co_return group;
}

awaitable<void> fillMember(Connection &c, MySQLHandler::GroupRecord &group, uint64_t account) {
    auto r = co_await query(c,
        "SELECT active,join_seq FROM im_group_members WHERE group_id={} AND account={}",
        group.id, account);
    if (r.rows().empty())
        co_return;
    const bool active = number(r.rows()[0][0]) != 0;
    group.joinSeq = number(r.rows()[0][1]);
    group.joined = active && !group.dismissed;
}

groupchat::Group toProto(const MySQLHandler::GroupRecord &group) {
    groupchat::Group result;
    result.set_id(std::to_string(group.id));
    result.set_name(group.name);
    result.set_owner(group.owner);
    result.set_membercount(group.memberCount);
    result.set_dismissed(group.dismissed);
    result.set_joined(group.joined);
    result.set_joinseq(group.joinSeq);
    result.set_lastseq(group.publishedSeq);
    result.set_large(group.memberCount > InlineMaxMembers());
    result.set_epoch(group.epoch);
    result.set_number(group.number);
    result.set_lastmsgtime(group.lastMsgTime);
    return result;
}

// ── 群号 ──

awaitable<uint64_t> allocateGroupNumber(Connection &c, uint64_t gid) {
    auto cursor = co_await query(c,
        "SELECT next_number FROM im_group_number_cursor WHERE id=1 FOR UPDATE");
    if (cursor.rows().empty())
        throw GroupError("DB_ERROR", "群号分配器未初始化");
    const auto value = number(cursor.rows()[0][0]);
    if (value > AccountPoolAllocator::kMaxAccount)
        throw GroupError("GROUP_NUMBER_EXHAUSTED", "群号已用尽");
    co_await query(c, "INSERT INTO im_group_numbers(group_id,number) VALUES({},{})", gid, value);
    co_await query(c, "UPDATE im_group_number_cursor SET next_number={} WHERE id=1", value + 1);
    co_return value;
}

awaitable<void> refreshMemberCount(Connection &c, uint64_t gid) {
    co_await query(c,
        "UPDATE im_groups SET member_count=(SELECT COUNT(*) FROM im_group_members "
        "WHERE group_id={} AND active=1),updated_at={} WHERE id={}", gid, now(), gid);
}

// ── 群命令（250：只保留成员/元数据/申请） ──

awaitable<groupchat::Reply> perform(Connection &c, const groupchat::Command &cmd) {
    groupchat::Reply reply;
    reply.set_errorcode("0");
    reply.set_groupid(cmd.groupid());

    const int op = static_cast<int>(cmd.operation());
    const uint64_t actor = cmd.actor();
    // 旧 250 Send/Pull/Ack/Read（11..14）已删除，显式拒绝而不是悄然成功。
    if (op >= 11 && op <= 14)
        throw GroupError("UNSUPPORTED_OPERATION", "旧群消息操作已废弃，请使用 246/245");
    if (!actor || op < static_cast<int>(groupchat::List) ||
        op > static_cast<int>(groupchat::MyApplications))
        throw GroupError("INVALID_PARAM", "无效群操作");
    if (cmd.text().size() > 512)
        throw GroupError("INVALID_PARAM", "内容过长");
    const auto limit = std::clamp<uint32_t>(cmd.limit(), 1, 100);

    auto user = co_await query(c, "SELECT nickname FROM users WHERE account={}", actor);
    if (user.rows().empty())
        throw GroupError("INVALID_ACCOUNT", "账号不存在");
    const auto nickname = str(user.rows()[0][0]);

    if (op == static_cast<int>(groupchat::MyApplications)) {
        auto r = co_await query(c,
            "SELECT a.id,a.group_id,g.name,a.reason,a.status,a.created_time,n.number "
            "FROM im_group_applications a JOIN im_groups g ON a.group_id=g.id "
            "LEFT JOIN im_group_numbers n ON n.group_id=g.id "
            "WHERE a.account={} AND a.id>{} ORDER BY a.id LIMIT {}", actor, cmd.after(), limit + 1);
        reply.set_hasmore(r.rows().size() > limit);
        for (size_t i = 0; i < std::min<size_t>(limit, r.rows().size()); ++i) {
            const auto row = r.rows()[i];
            auto *application = reply.add_applications();
            application->set_id(number(row[0]));
            application->set_groupid(std::to_string(number(row[1])));
            application->set_groupname(str(row[2]));
            application->set_account(actor);
            application->set_nickname(nickname);
            application->set_reason(str(row[3]));
            application->set_status(static_cast<int32_t>(number(row[4])));
            application->set_time(static_cast<int64_t>(number(row[5])));
            if (!row[6].is_null())
                application->set_groupnumber(std::to_string(number(row[6])));
            reply.set_cursor(application->id());
        }
        co_return reply;
    }

    if (op == static_cast<int>(groupchat::List)) {
        auto r = co_await query(c,
            "SELECT g.id,g.name,g.owner,g.member_count,g.dismissed,"
            "s.next_seq,s.published_seq,s.epoch,s.last_msg_time,"
            "CAST(COALESCE(n.number,0) AS UNSIGNED),m.join_seq "
            "FROM im_group_members m JOIN im_group_stream s ON s.group_id=m.group_id "
            "JOIN im_groups g ON g.id=m.group_id "
            "LEFT JOIN im_group_numbers n ON n.group_id=g.id "
            "WHERE m.account={} AND m.active=1 AND g.id>{} "
            "ORDER BY g.id LIMIT {}", actor, cmd.after(), limit + 1);
        reply.set_hasmore(r.rows().size() > limit);
        for (size_t i = 0; i < std::min<size_t>(limit, r.rows().size()); ++i) {
            const auto row = r.rows()[i];
            MySQLHandler::GroupRecord group;
            group.id = number(row[0]);
            group.name = str(row[1]);
            group.owner = number(row[2]);
            group.memberCount = number(row[3]);
            group.dismissed = number(row[4]) != 0;
            group.nextSeq = number(row[5]);
            group.publishedSeq = number(row[6]);
            group.epoch = number(row[7]);
            group.lastMsgTime = number(row[8]);
            group.number = std::to_string(number(row[9]));
            group.joinSeq = number(row[10]);
            // 解散群仍返回给客户端（dismissed=true），由客户端从 active 列表移除；
            // 登录同步 210 不包含解散群（SYNC-010）。
            group.joined = !group.dismissed;
            reply.set_cursor(group.id);
            *reply.add_groups() = toProto(group);
        }
        co_return reply;
    }

    if (op == static_cast<int>(groupchat::Create)) {
        if (cmd.text().empty() || cmd.text().size() > 256)
            throw GroupError("INVALID_NAME", "群名不能为空且不能超过256字节");
        const auto gid = static_cast<uint64_t>(Snowflake::instance().nextId());
        const auto timestamp = now();
        co_await query(c,
            "INSERT INTO im_groups(id,name,owner,member_count,dismissed,created_time,updated_at) "
            "VALUES({},{},{},1,0,{},{})", gid, cmd.text(), actor, timestamp, timestamp);
        co_await query(c,
            "INSERT INTO im_group_stream(group_id,next_seq,published_seq,last_msg_time,"
            "epoch,dismissed,updated_at) VALUES({},0,0,0,1,0,{})", gid, timestamp);
        co_await allocateGroupNumber(c, gid);
        co_await query(c,
            "INSERT INTO im_group_members(group_id,account,active,epoch,join_seq,joined_time,left_time) "
            "VALUES({},{},1,1,0,{},0)", gid, actor, timestamp);
        auto group = co_await loadGroup(c, gid);
        if (!group)
            throw GroupError("DB_ERROR", "群创建失败");
        group->joined = true;
        *reply.add_groups() = toProto(*group);
        reply.set_groupid(std::to_string(gid));
        reply.set_epoch(group->epoch);
        co_return reply;
    }

    uint64_t gid = groupId(cmd.groupid());
    if (op == static_cast<int>(groupchat::Search)) {
        auto lookup = co_await query(c, "SELECT group_id FROM im_group_numbers WHERE number={}", gid);
        if (!lookup.rows().empty())
            gid = number(lookup.rows()[0][0]);
        else if (cmd.groupid().size() <= 10)
            throw GroupError("GROUP_NOT_FOUND", "未找到该群号");
    }

    auto group = co_await loadGroup(c, gid);
    if (!group)
        throw GroupError("GROUP_NOT_FOUND", "群不存在");
    co_await fillMember(c, *group, actor);
    reply.set_epoch(group->epoch);
    reply.set_groupid(std::to_string(gid));

    if (op == static_cast<int>(groupchat::Search)) {
        *reply.add_groups() = toProto(*group);
        co_return reply;
    }
    if (group->dismissed) {
        if (op == static_cast<int>(groupchat::Dismiss) && group->owner == actor)
            co_return reply;
        throw GroupError("GROUP_DISMISSED", "群已解散");
    }
    if (op == static_cast<int>(groupchat::Apply)) {
        if (group->joined)
            throw GroupError("ALREADY_JOINED", "你已在群内");
        auto r = co_await query(c,
            "SELECT id FROM im_group_applications WHERE group_id={} AND account={} AND status=0 "
            "ORDER BY id DESC LIMIT 1", gid, actor);
        if (r.rows().empty())
            co_await query(c,
                "INSERT INTO im_group_applications(group_id,account,reason,status,created_time) "
                "VALUES({},{},{},0,{})", gid, actor, cmd.text(), now());
        co_return reply;
    }
    if (!group->joined) {
        if (op == static_cast<int>(groupchat::Leave))
            co_return reply;
        throw GroupError("NOT_MEMBER", "你已不在群内");
    }
    if (cmd.epoch() && cmd.epoch() != group->epoch)
        throw GroupError("MEMBERSHIP_CHANGED", "成员身份已变化，请刷新");
    if (op == static_cast<int>(groupchat::Applications) ||
        op == static_cast<int>(groupchat::Review) ||
        op == static_cast<int>(groupchat::Rename) ||
        op == static_cast<int>(groupchat::Kick) ||
        op == static_cast<int>(groupchat::Dismiss)) {
        if (group->owner != actor)
            throw GroupError("OWNER_REQUIRED", "仅群主可以执行此操作");
    }

    if (op == static_cast<int>(groupchat::Members)) {
        auto r = co_await query(c,
            "SELECT m.account,u.nickname FROM im_group_members m JOIN users u ON u.account=m.account "
            "WHERE m.group_id={} AND m.active=1 AND m.account>{} ORDER BY m.account LIMIT {}",
            gid, cmd.after(), limit + 1);
        reply.set_hasmore(r.rows().size() > limit);
        for (size_t i = 0; i < std::min<size_t>(limit, r.rows().size()); ++i) {
            auto *member = reply.add_members();
            member->set_account(number(r.rows()[i][0]));
            member->set_nickname(str(r.rows()[i][1]));
            member->set_owner(member->account() == group->owner);
            reply.set_cursor(member->account());
        }
    } else if (op == static_cast<int>(groupchat::Applications)) {
        auto r = co_await query(c,
            "SELECT a.id,a.account,u.nickname,a.reason,a.status,a.created_time "
            "FROM im_group_applications a JOIN users u ON u.account=a.account "
            "WHERE a.group_id={} AND a.id>{} AND a.status=0 ORDER BY a.id LIMIT {}",
            gid, cmd.after(), limit + 1);
        reply.set_hasmore(r.rows().size() > limit);
        for (size_t i = 0; i < std::min<size_t>(limit, r.rows().size()); ++i) {
            const auto row = r.rows()[i];
            auto *application = reply.add_applications();
            application->set_id(number(row[0]));
            application->set_groupid(std::to_string(gid));
            application->set_account(number(row[1]));
            application->set_nickname(str(row[2]));
            application->set_reason(str(row[3]));
            application->set_status(static_cast<int32_t>(number(row[4])));
            application->set_time(static_cast<int64_t>(number(row[5])));
            reply.set_cursor(application->id());
        }
    } else if (op == static_cast<int>(groupchat::Review)) {
        auto r = co_await query(c,
            "SELECT account,status FROM im_group_applications WHERE id={} AND group_id={} FOR UPDATE",
            cmd.applicationid(), gid);
        if (r.rows().empty())
            throw GroupError("APPLICATION_NOT_FOUND", "申请不存在");
        const auto target = number(r.rows()[0][0]);
        const auto status = number(r.rows()[0][1]);
        if (status)
            throw GroupError("APPLICATION_HANDLED", "申请已处理");
        if (!status) {
            if (cmd.accept()) {
                // 锁顺序：申请行 → 成员行（可能无）→ stream 行（ADR-11）。
                auto member = co_await query(c,
                    "SELECT active FROM im_group_members WHERE group_id={} AND account={} FOR UPDATE",
                    gid, target);
                const bool alreadyActive = !member.rows().empty() && number(member.rows()[0][0]) != 0;
                auto stream = co_await query(c,
                    "SELECT next_seq,epoch FROM im_group_stream WHERE group_id={} FOR UPDATE", gid);
                if (stream.rows().empty())
                    throw GroupError("GROUP_NOT_FOUND", "群不存在");
                // stream 行锁已串行化入群，再读一次成员数才是准确值。
                auto count = co_await query(c,
                    "SELECT member_count FROM im_groups WHERE id={}", gid);
                const auto currentCount = count.rows().empty() ? 0 : number(count.rows()[0][0]);
                if (!alreadyActive && currentCount >= MaxGroupMembers())
                    throw GroupError("GROUP_FULL", "群成员已满");
                const auto joinSeq = number(stream.rows()[0][0]);
                const auto epoch = number(stream.rows()[0][1]) + 1;
                const auto timestamp = now();
                co_await query(c,
                    "UPDATE im_group_stream SET epoch={},updated_at={} WHERE group_id={}",
                    epoch, timestamp, gid);
                co_await query(c,
                    "INSERT INTO im_group_members(group_id,account,active,epoch,join_seq,joined_time,left_time) "
                    "VALUES({},{},1,{},{},{},0) ON DUPLICATE KEY UPDATE active=1,epoch=VALUES(epoch),"
                    "join_seq=VALUES(join_seq),joined_time=VALUES(joined_time),left_time=0",
                    gid, target, epoch, joinSeq, timestamp);
                co_await refreshMemberCount(c, gid);
                group->epoch = epoch;
                group->joinSeq = joinSeq;
                reply.set_epoch(epoch);
            }
            co_await query(c, "UPDATE im_group_applications SET status={} WHERE id={}",
                cmd.accept() ? 1 : 2, cmd.applicationid());
        }
    } else if (op == static_cast<int>(groupchat::Rename)) {
        if (cmd.text().empty() || cmd.text().size() > 256)
            throw GroupError("INVALID_NAME", "群名不能为空且不能超过256字节");
        if (cmd.text() == group->name)
            throw GroupError("NAME_UNCHANGED", "新群名称不能与当前名称相同");
        co_await query(c, "UPDATE im_groups SET name={},updated_at={} WHERE id={}",
            cmd.text(), now(), gid);
    } else if (op == static_cast<int>(groupchat::Leave) || op == static_cast<int>(groupchat::Kick)) {
        const auto target = op == static_cast<int>(groupchat::Leave) ? actor : cmd.target();
        if (target == group->owner)
            throw GroupError("OWNER_CANNOT_LEAVE", "群主不能退出或被踢出，请解散群");
        auto member = co_await query(c,
            "SELECT active FROM im_group_members WHERE group_id={} AND account={} FOR UPDATE",
            gid, target);
        if (member.rows().empty() || number(member.rows()[0][0]) == 0)
            co_return reply;                    // 重复退群/踢人幂等
        auto stream = co_await query(c,
            "SELECT epoch FROM im_group_stream WHERE group_id={} FOR UPDATE", gid);
        if (stream.rows().empty())
            throw GroupError("GROUP_NOT_FOUND", "群不存在");
        const auto epoch = number(stream.rows()[0][0]) + 1;
        const auto timestamp = now();
        co_await query(c,
            "UPDATE im_group_stream SET epoch={},updated_at={} WHERE group_id={}", epoch, timestamp, gid);
        co_await query(c,
            "UPDATE im_group_members SET active=0,epoch={},left_time={} WHERE group_id={} AND account={}",
            epoch, timestamp, gid, target);
        co_await refreshMemberCount(c, gid);
        group->epoch = epoch;
        reply.set_epoch(epoch);
    } else if (op == static_cast<int>(groupchat::Dismiss)) {
        auto stream = co_await query(c,
            "SELECT dismissed,epoch FROM im_group_stream WHERE group_id={} FOR UPDATE", gid);
        if (stream.rows().empty())
            throw GroupError("GROUP_NOT_FOUND", "群不存在");
        if (number(stream.rows()[0][0]) != 0)
            co_return reply;                    // 重复解散幂等
        const auto epoch = number(stream.rows()[0][1]) + 1;
        const auto timestamp = now();
        co_await query(c,
            "UPDATE im_group_stream SET dismissed=1,epoch={},updated_at={} WHERE group_id={}",
            epoch, timestamp, gid);
        co_await query(c, "UPDATE im_groups SET dismissed=1,updated_at={} WHERE id={}", timestamp, gid);
        co_await query(c,
            "UPDATE im_group_applications SET status=3 WHERE group_id={} AND status=0", gid);
        reply.set_epoch(epoch);
    }
    co_return reply;
}

awaitable<groupchat::Reply> executeCommandOnPool(groupchat::Command cmd) {
    auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    auto &c = *connection.operator->();
    const bool mutation = cmd.operation() == groupchat::Create || cmd.operation() == groupchat::Apply ||
        cmd.operation() == groupchat::Review || cmd.operation() == groupchat::Rename ||
        cmd.operation() == groupchat::Leave || cmd.operation() == groupchat::Kick ||
        cmd.operation() == groupchat::Dismiss;
    groupchat::Reply reply;
    // 请求与响应原子落库；hex 编码避免二进制字符集转换。
    const auto toHex = [](const std::string &bytes) {
        static const char *digits = "0123456789abcdef";
        std::string result;
        for (unsigned char ch : bytes) {
            result += digits[ch >> 4];
            result += digits[ch & 15];
        }
        return result;
    };
    co_await groupTransaction(c, [&]() -> awaitable<void> {
        if (mutation) {
            if (!cmd.actor() || cmd.clientid().empty() || cmd.clientid().size() > 64)
                throw GroupError("INVALID_REQUEST_ID", "缺少有效请求标识");
            const auto payload = toHex(cmd.SerializeAsString());
            co_await query(c,
                "INSERT INTO im_group_requests(actor,client_id,payload,reply,created_time,updated_at) "
                "VALUES({},{},{},'',{},{}) ON DUPLICATE KEY UPDATE client_id=im_group_requests.client_id",
                cmd.actor(), cmd.clientid(), payload, now(), now());
            auto old = co_await query(c,
                "SELECT payload,reply FROM im_group_requests WHERE actor={} AND client_id={} FOR UPDATE",
                cmd.actor(), cmd.clientid());
            if (old.rows().empty() || str(old.rows()[0][0]) != payload)
                throw GroupError("IDEMPOTENCY_CONFLICT", "请求标识重复且内容不同");
            if (!str(old.rows()[0][1]).empty()) {
                const auto &hex = str(old.rows()[0][1]);
                if (hex.size() % 2)
                    throw GroupError("CACHE_ERROR", "无法恢复请求结果");
                std::string bytes;
                bytes.reserve(hex.size() / 2);
                for (size_t i = 0; i < hex.size(); i += 2) {
                    const auto digit = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
                    bytes += static_cast<char>(digit(hex[i]) * 16 + digit(hex[i + 1]));
                }
                if (!reply.ParseFromString(bytes))
                    throw GroupError("CACHE_ERROR", "无法恢复请求结果");
                co_return;
            }
        }
        reply = co_await perform(c, cmd);
        if (mutation)
            co_await query(c,
                "UPDATE im_group_requests SET reply={},updated_at={} WHERE actor={} AND client_id={}",
                toHex(reply.SerializeAsString()), now(), cmd.actor(), cmd.clientid());
    });
    co_return reply;
}

// ── 发送（246） ──

struct ExistingOutbox {
    std::string serverId;
    int64_t seq = 0;
    int state = 0;
};

awaitable<std::optional<ExistingOutbox>> findGroupOutbox(Connection &c, const std::string &key) {
    auto r = co_await query(c,
        "SELECT server_msg_id,seq,state FROM im_outbox WHERE operation=4 AND dedupe_key={}", key);
    if (r.rows().empty())
        co_return std::nullopt;
    ExistingOutbox existing;
    existing.serverId = str(r.rows()[0][0]);
    existing.seq = static_cast<int64_t>(number(r.rows()[0][1]));
    existing.state = static_cast<int>(number(r.rows()[0][2]));
    co_return existing;
}

awaitable<std::optional<sdkws::MsgData>> loadStagedMessage(Connection &c, const std::string &serverId) {
    auto r = co_await query(c,
        "SELECT payload FROM im_group_send_staging WHERE server_msg_id={}", serverId);
    if (r.rows().empty())
        co_return std::nullopt;
    sdkws::MsgData message;
    if (!google::protobuf::util::JsonStringToMessage(str(r.rows()[0][0]), &message).ok())
        throw GroupError("CACHE_ERROR", "群消息正文暂存损坏");
    co_return message;
}

awaitable<sdkws::MsgData> resolveExisting(const ExistingOutbox &existing,
    const groupchat::SendGroupMessageReq &request, Connection &c) {
    auto message = co_await loadStagedMessage(c, existing.serverId);
    if (!message)
        message = co_await MongoHandler::GetMsgByServerMsgIDAsync(existing.serverId);
    if (!message)
        throw GroupError("DB_ERROR", "群消息正文暂不可用，请稍后重试");
    if (!sameRequest(*message, request))
        throw GroupError("IDEMPOTENCY_CONFLICT", "消息标识重复且内容不同");
    co_return *message;
}

awaitable<sdkws::MsgData> insertGroupMessage(Connection &c, const groupchat::SendGroupMessageReq &request,
    uint64_t gid, const std::string &dedupeKey, const std::string &traceparent) {
    sdkws::MsgData message;
    co_await groupTransaction(c, [&]() -> awaitable<void> {
        // 1) 成员行锁：踢人/退群与发送串行化。
        auto member = co_await query(c,
            "SELECT active FROM im_group_members WHERE group_id={} AND account={} FOR UPDATE",
            gid, request.actor());
        if (member.rows().empty() || number(member.rows()[0][0]) == 0) {
            auto stream = co_await query(c,
                "SELECT dismissed FROM im_group_stream WHERE group_id={}", gid);
            if (stream.rows().empty())
                throw GroupError("GROUP_NOT_FOUND", "群不存在");
            throw GroupError("NOT_MEMBER", "你已不在群内");
        }

        // 2) seq + 解散准入：单行原子分配，失败回滚。
        const auto timestamp = now();
        auto updated = co_await query(c,
            "UPDATE im_group_stream SET next_seq=LAST_INSERT_ID(next_seq+1),"
            "last_msg_time={},updated_at={} WHERE group_id={} AND dismissed=0",
            timestamp, timestamp, gid);
        if (updated.affected_rows() == 0) {
            auto state = co_await query(c, "SELECT dismissed FROM im_group_stream WHERE group_id={}", gid);
            if (state.rows().empty())
                throw GroupError("GROUP_NOT_FOUND", "群不存在");
            throw GroupError("GROUP_DISMISSED", "群已解散");
        }
        auto allocated = co_await query(c, "SELECT LAST_INSERT_ID()");
        const auto seq = static_cast<int64_t>(number(allocated.rows()[0][0]));

        auto user = co_await query(c, "SELECT nickname FROM users WHERE account={}", request.actor());
        const auto nickname = user.rows().empty() ? std::string() : str(user.rows()[0][0]);

        message = newGroupMessage(gid, request.actor(), nickname, request, seq, timestamp);
        std::string body;
        if (!google::protobuf::util::MessageToJsonString(message, &body).ok())
            throw GroupError("DB_ERROR", "消息序列化失败");

        // 3) 正文暂存 + outbox 引用，同一事务；提交即返回，不等 Mongo/Kafka。
        co_await query(c,
            "INSERT INTO im_group_send_staging(server_msg_id,group_id,seq,payload,created_at,updated_at) "
            "VALUES({},{},{},{},{},{})",
            message.servermsgid(), gid, seq, body, timestamp, timestamp);
        co_await query(c,
            "INSERT INTO im_outbox(operation,dedupe_key,actor_account,peer_account,conversation_id,"
            "server_msg_id,seq,payload,traceparent,stage,state,available_at,created_at,updated_at) "
            "VALUES(4,{},{},0,{},{},{},{},{},0,0,0,{},{})",
            dedupeKey, request.actor(), std::to_string(gid), message.servermsgid(), seq,
            "{\"serverMsgID\":\"" + message.servermsgid() + "\"}", traceparent, timestamp, timestamp);
    });
    co_return message;
}

awaitable<MySQLHandler::GroupSendResult> sendOnPool(groupchat::SendGroupMessageReq request,
    std::string traceparent) {
    MySQLHandler::GroupSendResult result;
    const auto groupID = request.groupid();
    try {
        const auto gid = groupId(groupID);
        if (!request.actor())
            throw GroupError("INVALID_PARAM", "缺少发送者");
        if (!validClientId(request.clientid()))
            throw GroupError("INVALID_REQUEST_ID", "缺少有效请求标识");
        const int32_t contentType = request.contenttype() == 0 ? 101 : request.contenttype();
        if (contentType != 101)
            throw GroupError("INVALID_PARAM", "本期仅支持 contentType=101");
        request.set_contenttype(contentType);
        if (blankContent(request.content()))
            throw GroupError("EMPTY_MESSAGE", "消息不能为空");
        if (static_cast<uint64_t>(request.content().size()) > MaxGroupMessageBytes())
            throw GroupError("MESSAGE_TOO_LARGE", "消息内容超过 64KiB");

        const auto dedupeKey = "g:" + std::to_string(gid) + ":" +
            std::to_string(request.actor()) + ":" + request.clientid();
        auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
        auto &c = *connection.operator->();

        // 幂等快路径：命中则返回同一 seq/serverMsgID（正文来自 staging，已删则回读 Mongo）。
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (auto existing = co_await findGroupOutbox(c, dedupeKey)) {
                result.msg = co_await resolveExisting(*existing, request, c);
                result.errorCode = "0";
                co_return result;
            }
            try {
                result.msg = co_await insertGroupMessage(c, request, gid, dedupeKey, traceparent);
                result.errorCode = "0";
                co_return result;
            } catch (...) {
                // 并发同 clientID：唯一键冲突后事务整体回滚（seq 一并回滚），再读取获胜者。
                if (duplicateKey(std::current_exception()) && attempt < 3)
                    continue;
                throw;
            }
        }
        throw GroupError("DB_ERROR", "群消息发送冲突，请稍后重试");
    } catch (const GroupError &e) {
        result.errorCode = e.code;
        result.errorMessage = e.what();
    } catch (const std::exception &e) {
        spdlog::error("Group send failed: {}", e.what());
        result.errorCode = "DB_ERROR";
        result.errorMessage = "群服务暂时不可用，请稍后重试";
    }
    co_return result;
}

// ── outbox op=4 ──

awaitable<void> advancePublished(Connection &c, uint64_t gid, int64_t seq) {
    co_await query(c,
        "UPDATE im_group_stream SET published_seq=GREATEST(published_seq,{}),updated_at={} "
        "WHERE group_id={}", seq, now(), gid);
}

awaitable<void> finishGroupOutbox(Connection &c, uint64_t id, int state, const std::string &error = {}) {
    co_await query(c,
        "UPDATE im_outbox SET state={},lease_token='',last_error={},updated_at={} WHERE id={}",
        state, error, now(), id);
}

awaitable<void> markStageOne(Connection &c, uint64_t id) {
    co_await query(c, "UPDATE im_outbox SET stage=1,updated_at={} WHERE id={}", now(), id);
}

awaitable<void> deleteStaging(Connection &c, const std::string &serverId) {
    co_await query(c, "DELETE FROM im_group_send_staging WHERE server_msg_id={}", serverId);
}

awaitable<void> abandonGroupOutbox(Connection &c, const MySQLHandler::OutboxTask &task) {
    const auto state = co_await MongoHandler::GetMsgCommitStateAsync(task.serverId);
    if (state == MongoHandler::MsgCommitState::Missing) {
        // 正文从未落库：必须写 tombstone，否则 seq 已分配而 Mongo 永久不可见。
        if (!co_await MongoHandler::SaveGroupTombstoneAsync(task.conversation, task.seq))
            throw std::runtime_error("Group tombstone persistence failed");
        co_await advancePublished(c, std::stoull(task.conversation), task.seq);
        co_await deleteStaging(c, task.serverId);
        co_await finishGroupOutbox(c, task.id, 3,
            "GROUP_PUBLISH_MAX_ATTEMPTS exceeded: tombstone written");
        ape::obs::RecordOutboxTombstone(4);
        spdlog::warn("GroupOutbox: seq={} group={} abandoned as tombstone after {} attempts",
            task.seq, task.conversation, task.attempts);
        co_return;
    }
    if (state == MongoHandler::MsgCommitState::Uncommitted) {
        co_await MongoHandler::PublishMessage(task.serverId);
        co_await advancePublished(c, std::stoull(task.conversation), task.seq);
        co_await deleteStaging(c, task.serverId);
        co_await finishGroupOutbox(c, task.id, 2);
        spdlog::info("GroupOutbox: seq={} group={} published after {} attempts (body already stored)",
            task.seq, task.conversation, task.attempts);
        co_return;
    }
    co_await advancePublished(c, std::stoull(task.conversation), task.seq);
    co_await deleteStaging(c, task.serverId);
    co_await finishGroupOutbox(c, task.id, 3);
    spdlog::warn("GroupOutbox: seq={} group={} already committed; outbox abandoned after {} attempts",
        task.seq, task.conversation, task.attempts);
}

} // namespace

// ── MySQLHandler 群聊公开接口 ──

boost::asio::awaitable<void> MySQLHandler::InitializeGroups() {
    // schema/DDL 与 EnsureGroupNumbers 分两次取连接：pool_size=1 时也不会自锁。
    {
    auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    auto &c = *connection.operator->();
    results r;

    const auto hasTable = [&](const std::string &name) -> awaitable<bool> {
        auto found = co_await query(c,
            "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
            "AND table_name={}", name);
        co_return number(found.rows()[0][0]) != 0;
    };
    const auto hasColumn = [&](const std::string &table, const std::string &column) -> awaitable<bool> {
        auto found = co_await query(c,
            "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema=DATABASE() "
            "AND table_name={} AND column_name={}", table, column);
        co_return number(found.rows()[0][0]) != 0;
    };

    // v3 是破坏性切换：检测到旧群 schema（MySQL 存消息 / last_seq / ack_seq / 没有 stream）
    // 就整组删除重建。mock 数据不迁移，避免新旧语义混在一张表里。
    bool legacy = co_await hasTable("im_group_messages");
    if (!legacy && co_await hasTable("im_groups") && !co_await hasTable("im_group_stream"))
        legacy = true;
    if (!legacy && co_await hasColumn("im_groups", "last_seq"))
        legacy = true;
    if (!legacy && co_await hasColumn("im_group_members", "ack_seq"))
        legacy = true;
    if (legacy) {
        spdlog::warn("InitializeGroups: legacy group schema detected, dropping group tables (v3 reset)");
        const std::vector<std::string> drops = {
            "DROP TABLE IF EXISTS im_group_send_staging",
            "DROP TABLE IF EXISTS im_group_messages",
            "DROP TABLE IF EXISTS im_group_requests",
            "DROP TABLE IF EXISTS im_group_applications",
            "DROP TABLE IF EXISTS im_group_numbers",
            "DROP TABLE IF EXISTS im_group_members",
            "DROP TABLE IF EXISTS im_group_stream",
            "DROP TABLE IF EXISTS im_groups"};
        for (const auto &sql : drops)
            co_await c.async_execute(sql, r, use_awaitable);
    }

    const std::vector<std::string> statements = {
        R"sql(CREATE TABLE IF NOT EXISTS im_groups (
            id BIGINT UNSIGNED PRIMARY KEY,
            name VARCHAR(256) NOT NULL,
            owner BIGINT UNSIGNED NOT NULL,
            member_count BIGINT UNSIGNED NOT NULL DEFAULT 1,
            dismissed TINYINT NOT NULL DEFAULT 0,
            created_time BIGINT NOT NULL DEFAULT 0,
            updated_at BIGINT NOT NULL DEFAULT 0,
            KEY group_owner(owner)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)sql",
        R"sql(CREATE TABLE IF NOT EXISTS im_group_stream (
            group_id BIGINT UNSIGNED PRIMARY KEY,
            next_seq BIGINT UNSIGNED NOT NULL DEFAULT 0,
            published_seq BIGINT UNSIGNED NOT NULL DEFAULT 0,
            last_msg_time BIGINT UNSIGNED NOT NULL DEFAULT 0,
            epoch BIGINT UNSIGNED NOT NULL DEFAULT 1,
            dismissed TINYINT NOT NULL DEFAULT 0,
            updated_at BIGINT NOT NULL,
            CONSTRAINT fk_stream_group FOREIGN KEY(group_id) REFERENCES im_groups(id)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)sql",
        R"sql(CREATE TABLE IF NOT EXISTS im_group_members (
            group_id BIGINT UNSIGNED NOT NULL,
            account BIGINT UNSIGNED NOT NULL,
            active TINYINT NOT NULL DEFAULT 1,
            epoch BIGINT UNSIGNED NOT NULL DEFAULT 1,
            join_seq BIGINT UNSIGNED NOT NULL DEFAULT 0,
            joined_time BIGINT NOT NULL DEFAULT 0,
            left_time BIGINT NOT NULL DEFAULT 0,
            PRIMARY KEY(group_id,account),
            KEY account_active(account,active,group_id),
            CONSTRAINT fk_member_group FOREIGN KEY(group_id) REFERENCES im_groups(id)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)sql",
        R"sql(CREATE TABLE IF NOT EXISTS im_group_applications (
            id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
            group_id BIGINT UNSIGNED NOT NULL,
            account BIGINT UNSIGNED NOT NULL,
            reason VARCHAR(512) NOT NULL,
            status TINYINT NOT NULL DEFAULT 0,
            created_time BIGINT NOT NULL,
            KEY group_page(group_id,id),
            KEY applicant(account,id),
            CONSTRAINT fk_application_group FOREIGN KEY(group_id) REFERENCES im_groups(id)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)sql",
        R"sql(CREATE TABLE IF NOT EXISTS im_group_send_staging (
            server_msg_id VARCHAR(64) PRIMARY KEY,
            group_id BIGINT UNSIGNED NOT NULL,
            seq BIGINT UNSIGNED NOT NULL,
            payload MEDIUMTEXT NOT NULL,
            created_at BIGINT NOT NULL,
            updated_at BIGINT NOT NULL,
            KEY staging_retention(updated_at)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)sql",
        R"sql(CREATE TABLE IF NOT EXISTS im_group_requests (
            actor BIGINT UNSIGNED NOT NULL,
            client_id VARCHAR(64) NOT NULL,
            payload MEDIUMTEXT NOT NULL,
            reply MEDIUMTEXT NOT NULL,
            created_time BIGINT NOT NULL DEFAULT 0,
            updated_at BIGINT NOT NULL DEFAULT 0,
            PRIMARY KEY(actor,client_id),
            KEY request_retention(updated_at)
        ) ENGINE=InnoDB DEFAULT CHARSET=ascii)sql",
        R"sql(CREATE TABLE IF NOT EXISTS im_group_numbers (
            group_id BIGINT UNSIGNED PRIMARY KEY,
            number BIGINT UNSIGNED NOT NULL UNIQUE,
            CONSTRAINT fk_number_group FOREIGN KEY(group_id) REFERENCES im_groups(id)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)sql",
        R"sql(CREATE TABLE IF NOT EXISTS im_group_number_cursor (
            id TINYINT PRIMARY KEY,
            next_number BIGINT UNSIGNED NOT NULL
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)sql"};
    for (const auto &sql : statements)
        co_await c.async_execute(sql, r, use_awaitable);
    co_await c.async_execute("INSERT IGNORE INTO im_group_number_cursor VALUES(1,1000000000)", r, use_awaitable);
    }
    co_await EnsureGroupNumbers();
}

boost::asio::awaitable<void> MySQLHandler::EnsureGroupNumbers() {
    auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    auto &c = *connection.operator->();
    auto missing = co_await query(c,
        "SELECT g.id FROM im_groups g LEFT JOIN im_group_numbers n ON g.id=n.group_id "
        "WHERE n.group_id IS NULL ORDER BY g.id");
    std::vector<uint64_t> ids;
    for (const auto row : missing.rows())
        ids.push_back(number(row[0]));
    for (const auto id : ids) {
        co_await groupTransaction(c, [&]() -> awaitable<void> {
            co_await query(c, "SELECT next_number FROM im_group_number_cursor WHERE id=1 FOR UPDATE");
            auto exists = co_await query(c, "SELECT number FROM im_group_numbers WHERE group_id={}", id);
            if (exists.rows().empty())
                co_await allocateGroupNumber(c, id);
        });
    }
}

boost::asio::awaitable<groupchat::Reply> MySQLHandler::ExecuteGroupCommand(groupchat::Command cmd) {
    const auto groupID = cmd.groupid();
    try {
        co_return co_await boost::asio::co_spawn(
            MySQLConnector::instance().get_pool_executor(0),
            executeCommandOnPool(std::move(cmd)), use_awaitable);
    } catch (const GroupError &e) {
        groupchat::Reply reply;
        reply.set_errorcode(e.code);
        reply.set_errormessage(e.what());
        reply.set_groupid(groupID);
        co_return reply;
    } catch (const std::exception &e) {
        spdlog::error("Group command failed: {}", e.what());
        groupchat::Reply reply;
        reply.set_errorcode("DB_ERROR");
        reply.set_errormessage("群服务暂时不可用，请稍后重试");
        reply.set_groupid(groupID);
        co_return reply;
    }
}

boost::asio::awaitable<MySQLHandler::GroupSendResult>
MySQLHandler::SendGroupMessage(groupchat::SendGroupMessageReq request, std::string traceparent) {
    co_return co_await boost::asio::co_spawn(
        MySQLConnector::instance().get_pool_executor(0),
        sendOnPool(std::move(request), std::move(traceparent)), use_awaitable);
}

boost::asio::awaitable<MySQLHandler::GroupAccessResult>
MySQLHandler::GroupAccess(std::string groupID, uint64_t account) {
    GroupAccessResult result;
    try {
        if (!account)
            throw GroupError("INVALID_PARAM", "缺少账号");
        const auto gid = groupId(groupID);
        auto connection = co_await MySQLConnector::instance().get_pool(1).async_get_connection(use_awaitable);
        auto group = co_await loadGroup(*connection.operator->(), gid);
        if (!group) {
            result.errorCode = "GROUP_NOT_FOUND";
            result.errorMessage = "群不存在";
            co_return result;
        }
        co_await fillMember(*connection.operator->(), *group, account);
        if (group->dismissed) {
            result.errorCode = "GROUP_DISMISSED";
            result.errorMessage = "群已解散";
            co_return result;
        }
        if (!group->joined) {
            result.errorCode = "NOT_MEMBER";
            result.errorMessage = "你已不在群内";
            co_return result;
        }
        result.errorCode = "0";
        result.group = *group;
    } catch (const GroupError &e) {
        result.errorCode = e.code;
        result.errorMessage = e.what();
    } catch (const std::exception &e) {
        spdlog::error("Group access failed: {}", e.what());
        result.errorCode = "DB_ERROR";
        result.errorMessage = "群服务暂时不可用，请稍后重试";
    }
    co_return result;
}

boost::asio::awaitable<std::vector<MySQLHandler::GroupRecord>>
MySQLHandler::ListGroupsForSync(uint64_t account, uint32_t limit) {
    std::vector<GroupRecord> result;
    if (!account || !limit)
        co_return result;
    auto connection = co_await MySQLConnector::instance().get_pool(1).async_get_connection(use_awaitable);
    auto r = co_await query(*connection.operator->(),
        "SELECT g.id,g.name,g.owner,g.member_count,g.dismissed,"
        "s.next_seq,s.published_seq,s.epoch,s.last_msg_time,"
        "CAST(COALESCE(n.number,0) AS UNSIGNED),m.join_seq "
        "FROM im_group_members m JOIN im_group_stream s ON s.group_id=m.group_id "
        "JOIN im_groups g ON g.id=m.group_id LEFT JOIN im_group_numbers n ON n.group_id=g.id "
        "WHERE m.account={} AND m.active=1 AND g.dismissed=0 "
        "ORDER BY s.last_msg_time DESC,g.id DESC LIMIT {}", account, limit);
    for (const auto row : r.rows()) {
        GroupRecord group;
        group.id = number(row[0]);
        group.name = str(row[1]);
        group.owner = number(row[2]);
        group.memberCount = number(row[3]);
        group.dismissed = number(row[4]) != 0;
        group.nextSeq = number(row[5]);
        group.publishedSeq = number(row[6]);
        group.epoch = number(row[7]);
        group.lastMsgTime = number(row[8]);
        group.number = std::to_string(number(row[9]));
        group.joinSeq = number(row[10]);
        group.joined = true;
        result.push_back(std::move(group));
    }
    co_return result;
}

boost::asio::awaitable<MySQLHandler::GroupFanoutInfo>
MySQLHandler::GroupFanoutState(std::string groupID) {
    GroupFanoutInfo state;
    try {
        if (groupID.empty() || groupID.size() > 20 ||
            groupID.find_first_not_of("0123456789") != std::string::npos)
            co_return state;
        const auto gid = std::stoull(groupID);
        auto connection = co_await MySQLConnector::instance().get_pool(1).async_get_connection(use_awaitable);
        auto r = co_await query(*connection.operator->(),
            "SELECT s.published_seq,s.epoch,s.dismissed,g.member_count "
            "FROM im_group_stream s JOIN im_groups g ON g.id=s.group_id WHERE s.group_id={}", gid);
        if (r.rows().empty())
            co_return state;
        state.found = true;
        state.publishedSeq = number(r.rows()[0][0]);
        state.epoch = number(r.rows()[0][1]);
        state.dismissed = number(r.rows()[0][2]) != 0;
        state.memberCount = number(r.rows()[0][3]);
    } catch (const std::exception &e) {
        spdlog::warn("GroupFanoutState({}) failed: {}", groupID, e.what());
    }
    co_return state;
}

boost::asio::awaitable<std::vector<MySQLHandler::GroupMemberJoinSeq>>
MySQLHandler::GroupMembersWithJoinSeq(std::string groupID) {
    std::vector<GroupMemberJoinSeq> result;
    try {
        const auto gid = groupId(groupID);
        auto connection = co_await MySQLConnector::instance().get_pool(1).async_get_connection(use_awaitable);
        auto r = co_await query(*connection.operator->(),
            "SELECT account,join_seq FROM im_group_members WHERE group_id={} AND active=1 "
            "ORDER BY account", gid);
        for (const auto row : r.rows()) {
            GroupMemberJoinSeq member;
            member.account = number(row[0]);
            member.joinSeq = number(row[1]);
            result.push_back(member);
        }
    } catch (const GroupError &) {
    } catch (const std::exception &e) {
        spdlog::warn("GroupMembersWithJoinSeq({}) failed: {}", groupID, e.what());
    }
    co_return result;
}

boost::asio::awaitable<void> MySQLHandler::ProcessGroupOutboxTask(OutboxTask task,
    IMessageProducer &producer, int pool) {
    auto connection = co_await MySQLConnector::instance().get_pool(pool).async_get_connection(use_awaitable);
    auto &c = *connection.operator->();
    const auto maxAttempts = PublishMaxAttempts();
    auto gid = std::stoull(task.conversation);

    if (task.stage == 0 && task.attempts > maxAttempts) {
        co_await abandonGroupOutbox(c, task);
        co_return;
    }

    if (task.stage == 0) {
        auto message = co_await loadStagedMessage(c, task.serverId);
        if (!message) {
            // staging 丢失时回读 Mongo：可能上次已写正文但未标记 stage=1。
            const auto state = co_await MongoHandler::GetMsgCommitStateAsync(task.serverId);
            if (state == MongoHandler::MsgCommitState::Missing)
                throw std::runtime_error("Group message body is missing from staging and Mongo");
            if (state == MongoHandler::MsgCommitState::Committed) {
                co_await advancePublished(c, gid, task.seq);
                co_await finishGroupOutbox(c, task.id, 3);
                co_return;
            }
        } else {
            if (!co_await MongoHandler::SaveMsgToMongoAsync(*message, false))
                throw std::runtime_error("MongoDB group message persistence failed");
            co_await markStageOne(c, task.id);
            co_await deleteStaging(c, task.serverId);
        }
        task.stage = 1;
    }

    // stage=1：先发布正文再发 Kafka；published_seq 只由已发布（含 tombstone）推进。
    // 投递尝试 = 新 trace + link 指回受理 span；traceparent 来自 im_outbox 行。
    auto publish_span = ape::otel::StartLinkedSpan(
        ape::otel::TraceContext::FromTraceParent(task.traceparent),
        "outbox.publish.op4",
        {{"im.operation", std::to_string(task.operation)},
         {"im.server_msg_id", task.serverId},
         {"im.group_id", task.conversation},
         {"outbox.attempts", std::to_string(task.attempts)}},
        opentelemetry::trace::SpanKind::kProducer);
    co_await MongoHandler::PublishMessage(task.serverId);
    co_await advancePublished(c, gid, task.seq);

    if (task.attempts > maxAttempts) {
        // 正文已 committed，245 可拉；只是推送失败，不再无限重试 Kafka。
        co_await finishGroupOutbox(c, task.id, 3, "GROUP_PUBLISH_MAX_ATTEMPTS exceeded: published without push");
        spdlog::warn("GroupOutbox: seq={} group={} published but not pushed after {} attempts",
            task.seq, task.conversation, task.attempts);
        co_return;
    }

    const auto kafka_value = ape::otel::EncodeKafkaValue(publish_span.context(), task.serverId);
    if (!co_await producer.deliverConfirmed(task.conversation, kafka_value))
        throw std::runtime_error("Kafka broker did not confirm group delivery");
    co_await finishGroupOutbox(c, task.id, 2);
    publish_span.EndOk();
    spdlog::info("GroupOutbox: seq={} group={} published (state=2)", task.seq, task.conversation);
}

boost::asio::awaitable<uint64_t> MySQLHandler::PurgeOrphanGroupStaging(int64_t cutoff, int batchLimit) {
    if (batchLimit <= 0)
        co_return 0;
    auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    // 只清“孤儿”：还有 state 0/1 的 op=4 任务时，staging 是唯一正文来源，不能删。
    auto r = co_await query(*connection.operator->(),
        "DELETE FROM im_group_send_staging WHERE updated_at<{} AND server_msg_id NOT IN "
        "(SELECT server_msg_id FROM im_outbox WHERE operation=4 AND state IN (0,1)) "
        "ORDER BY updated_at LIMIT {}", cutoff, batchLimit);
    co_return r.affected_rows();
}

boost::asio::awaitable<uint64_t> MySQLHandler::PurgeGroupRequests(int64_t cutoff, int batchLimit) {
    if (batchLimit <= 0)
        co_return 0;
    auto connection = co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    auto r = co_await query(*connection.operator->(),
        "DELETE FROM im_group_requests WHERE updated_at<{} ORDER BY updated_at LIMIT {}",
        cutoff, batchLimit);
    co_return r.affected_rows();
}
