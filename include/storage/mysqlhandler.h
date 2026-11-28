#ifndef MYSQLHANDLER_H
#define MYSQLHANDLER_H

#include "user/userinfo.h"
#include "friend.pb.h"
#include "conversation.pb.h"
#include "groupchat.pb.h"
#include "sdkws.pb.h"
#include <boost/mysql/connection_pool.hpp>
#include <stdexcept>
#include <functional>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
class IMessageProducer;

// ============================================================================
// MySQLHandler — MySQL 异步操作封装
//
// 所有方法均为协程方法（boost::asio::awaitable<T>），
// ============================================================================

class MySQLHandler
{
public:
    // ── 群聊（v3）：元数据/成员/seq 在 MySQL，消息正文在 Mongo ──
    struct GroupRecord
    {
        uint64_t id = 0;
        std::string name;
        uint64_t owner = 0;
        uint64_t memberCount = 0;
        bool dismissed = false;
        bool joined = false;
        uint64_t joinSeq = 0;
        uint64_t nextSeq = 0;
        uint64_t publishedSeq = 0;   // 已发布（含 tombstone）最大 seq
        uint64_t epoch = 0;          // 权威值在 im_group_stream
        uint64_t lastMsgTime = 0;
        std::string number;
    };

    struct GroupAccessResult
    {
        std::string errorCode;       // "0" = 可用
        std::string errorMessage;
        GroupRecord group;
    };

    struct GroupSendResult
    {
        std::string errorCode;
        std::string errorMessage;
        sdkws::MsgData msg;
    };

    struct GroupFanoutInfo
    {
        bool found = false;
        bool dismissed = false;
        uint64_t publishedSeq = 0;
        uint64_t epoch = 0;
        uint64_t memberCount = 0;
    };

    struct GroupMemberJoinSeq
    {
        uint64_t account = 0;
        uint64_t joinSeq = 0;
    };

    static boost::asio::awaitable<void> InitializeGroups();
    static boost::asio::awaitable<void> EnsureGroupNumbers();
    static boost::asio::awaitable<groupchat::Reply> ExecuteGroupCommand(groupchat::Command command);
    // 246：成员锁 → stream seq → staging+outbox(op=4) 单事务；提交即返回，不等 Mongo。
    static boost::asio::awaitable<GroupSendResult> SendGroupMessage(
        groupchat::SendGroupMessageReq request, std::string traceparent = {});
    // 245 访问控制：群不存在/解散/非成员分别返回错误码，成功时带 joinSeq/publishedSeq/epoch。
    static boost::asio::awaitable<GroupAccessResult> GroupAccess(std::string groupID, uint64_t account);
    // 210 群同步：active 群按 last_msg_time DESC，limit+1 用于判断 hasMore。
    static boost::asio::awaitable<std::vector<GroupRecord>> ListGroupsForSync(uint64_t account, uint32_t limit);
    // 群扇出：读 stream 的 dismissed/epoch/published_seq，不读消息正文。
    static boost::asio::awaitable<GroupFanoutInfo> GroupFanoutState(std::string groupID);
    // 群扇出：active=1 成员的 joinSeq（缓存 miss 时回源）。
    static boost::asio::awaitable<std::vector<GroupMemberJoinSeq>> GroupMembersWithJoinSeq(std::string groupID);

    struct OutboxTask
    {
        uint64_t id = 0, actor = 0, peer = 0;
        int64_t seq = 0;
        int operation = 0, state = 0, stage = 0, attempts = 0;
        bool dispatchDone = false;
        std::string conversation, serverId, payload, leaseToken, traceparent;
        // Outbox 字段说明：
        // operation：任务类型
        //   1 = 单聊发送；2 = 删除好友后的历史清理；3 = ACK 后的投递状态与缓存清理；
        //   4 = 群聊发送（正文在 im_group_send_staging，payload 只存 serverMsgID 引用）。
        // stage：正文保存阶段
        //   0 = Mongo 正文尚未保存；1 = Mongo 正文已保存（committed=false）。
        // state：任务生命周期状态
        //   0 = 待执行（到 available_at 后可领取）。
        //   1 = 已领取（由 lease_token 标识本次领取，lease_until 为租约到期时间）。
        //   2 = 任务完成（不代表用户已读）。
        //   3 = 已放弃/已取消（群任务只有在可拉取空洞 tombstone 写成功后才允许进入）。
    };
    static boost::asio::awaitable<void> InitializeOutbox();
    static boost::asio::awaitable<OutboxTask> QueueSingleMessage(
        sdkws::MsgData message, std::string traceparent = {});
    struct OutboxBacklog
    {
        int operation = 0;
        int64_t pending = 0;
        int64_t oldest_age_seconds = 0;
    };
    // 未完成 outbox 的积压量与最老年龄；供 30s 轮询的指标采集使用。
    static boost::asio::awaitable<std::vector<OutboxBacklog>> CollectOutboxBacklog(int pool = 0);
    static boost::asio::awaitable<void> QueueConversationCleanup(boost::mysql::any_connection& connection,
        uint64_t actor, uint64_t peer, int64_t cutoff);
    // 按会话水位确认：owner 在 conversation 中已收到 <= upToSeq 的消息
    static boost::asio::awaitable<bool> AckConversation(uint64_t owner, std::string conversationID, int64_t upToSeq);
    // operations：本服务负责的任务类型。MsgService {1,2,3}，GroupService {4}。
    static boost::asio::awaitable<std::optional<OutboxTask>> ClaimOutboxTask(
        const std::vector<int>& operations, int pool = 0);
    static boost::asio::awaitable<OutboxTask> GetOutboxTask(uint64_t id, int pool = 0);
    static boost::asio::awaitable<void> ProcessOutboxTask(OutboxTask task, IMessageProducer& producer, int pool = 0);
    // 群聊 op=4：stage 0→1（写 Mongo/删 staging）→ 发布 + published_seq + Kafka → state=2；
    // 超过 GROUP_PUBLISH_MAX_ATTEMPTS 时按 Mongo 现状发布或写 tombstone，绝不留下不可见空洞。
    static boost::asio::awaitable<void> ProcessGroupOutboxTask(OutboxTask task, IMessageProducer& producer, int pool = 0);
    static boost::asio::awaitable<void> RetryOutboxTask(OutboxTask task, std::string error, int pool = 0,
        int64_t delayMs = 0);
    // 已完成的 outbox 任务按 operation 分批清理（docs/ack 11.1）：
    // 只删 state IN (2,3) 且 updated_at < cutoff 的行，每批 LIMIT batchLimit，
    // 最多 maxBatches 批；未完成/租约中的任务（state 0/1）永不删除。
    // 返回本轮删除的行数。
    static boost::asio::awaitable<uint64_t> PurgeCompletedOutbox(int operation, int64_t cutoff,
        int batchLimit, int maxBatches);
    // 群正文暂存孤儿清理（worker 成功后立即删除，这里只兜底崩溃残留）。
    static boost::asio::awaitable<uint64_t> PurgeOrphanGroupStaging(int64_t cutoff, int batchLimit);
    // 成员命令幂等表按保留期清理（GROUP_REQUEST_RETENTION_DAYS）。
    static boost::asio::awaitable<uint64_t> PurgeGroupRequests(int64_t cutoff, int batchLimit);

    static boost::asio::awaitable<void> FilterVisibleMessages(std::vector<sdkws::MsgData>& messages);
    static boost::asio::awaitable<int64_t> FriendDeletionSeq(uint64_t owner, uint64_t peer);
    static boost::asio::awaitable<bool> MessageDispatchDone(uint64_t owner, std::string serverId,
        std::string conversationID, int64_t seq);
    static boost::asio::awaitable<void> ConfirmMessageDispatch(std::string serverId);
    struct RegistrationError : std::runtime_error
    {
        std::string code;
        RegistrationError(std::string code, std::string message)
            : std::runtime_error(std::move(message)), code(std::move(code)) {}
    };

    struct RegistrationInput
    {
        std::string request_id;
        userInfo user;
        std::string payload;
    };

    struct RegistrationResult
    {
        uint64_t user_id;
        uint64_t account;
    };

    static std::string registration_payload(const std::string &phone,
        const std::string &nickname, const std::string &password);

    static boost::asio::awaitable<RegistrationResult>
        register_user(boost::mysql::connection_pool &pool, const RegistrationInput &input);

    static boost::asio::awaitable<void>
        refill_accounts(boost::mysql::connection_pool &pool, uint64_t batch_size);

    static boost::asio::awaitable<bool> WithFriendMessage(const sdkws::MsgData &message,
        std::function<boost::asio::awaitable<void>()> action, bool requireUndelivered = false);

    static boost::asio::awaitable<void> InitializeConversations();
    static boost::asio::awaitable<void> RecordConversationMessage(
        boost::mysql::any_connection &connection, const sdkws::MsgData &message);
    static boost::asio::awaitable<void> DeleteConversationPair(
        boost::mysql::any_connection &connection, const std::string &id);
    static boost::asio::awaitable<std::vector<conversation::Conversation>> ListConversations(uint64_t owner);
    // 登录同步只取最近的 K 个会话（latest_time desc），避免全量扫描
    static boost::asio::awaitable<std::vector<conversation::Conversation>>
        ListRecentConversations(uint64_t owner, uint32_t limit);
    static boost::asio::awaitable<bool> MarkConversationRead(uint64_t owner, std::string id, int64_t seq);
    static boost::asio::awaitable<bool> UpdateConversationSettings(uint64_t owner, std::string id,
        bool hasPinned, bool pinned, bool hasRecvMsgOpt, int32_t recvMsgOpt);
    static boost::asio::awaitable<bool> DeleteConversation(uint64_t owner, std::string id);

    static boost::asio::awaitable<friendpb::Reply>
        ExecuteFriendCommand(const friendpb::Command &command);

    static boost::asio::awaitable<std::optional<userInfo>>
        find_user_by_account(boost::mysql::any_connection &connection, uint64_t account);

    static boost::asio::awaitable<uint64_t>
        insert_user(boost::mysql::any_connection &connection, const userInfo &user, uint64_t account);

    // ── 用户相关 ──
    static boost::asio::awaitable<std::optional<uint64_t>>
        insert_user(const userInfo &user);

    static boost::asio::awaitable<std::optional<userInfo>>
        find_user_by_account(const uint64_t account);

    static boost::asio::awaitable<std::optional<userInfo>>
        find_user_by_id(const uint64_t user_id);

    static boost::asio::awaitable<bool>
        update_last_login(const uint64_t user_id);

    // 把 account 写回 users 表，返回是否成功
    static boost::asio::awaitable<bool>
        update_account(uint64_t user_id, uint64_t account);

    static boost::asio::awaitable<bool>
        account_exists(const uint64_t account);


};

#endif
