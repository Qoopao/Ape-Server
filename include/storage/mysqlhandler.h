#ifndef MYSQLHANDLER_H
#define MYSQLHANDLER_H

#include "user/userinfo.h"
#include "friend.pb.h"
#include "conversation.pb.h"
#include "groupchat.pb.h"
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

// Forward declarations for protobuf types
namespace sdkws {
class GroupInfo;
class GroupInfoForSet;
class GroupMemberFullInfo;
} // namespace sdkws

// ============================================================================
// MySQLHandler — MySQL 异步操作封装
//
// 所有方法均为协程方法（boost::asio::awaitable<T>），
// ============================================================================

class MySQLHandler
{
public:
    static boost::asio::awaitable<void> InitializeGroups();
    static boost::asio::awaitable<void> EnsureGroupNumbers();
    static boost::asio::awaitable<groupchat::Reply> ExecuteGroupCommand(groupchat::Command command);
    static boost::asio::awaitable<groupchat::Notice> GroupDeliveryBatch(uint64_t actor,std::string id);
    static boost::asio::awaitable<groupchat::Notice> WithGroupDelivery(uint64_t actor,std::string id,
        std::function<void(const groupchat::Notice&)> action);
    static boost::asio::awaitable<std::vector<uint64_t>> GroupNotificationAccounts(std::string id);
    static boost::asio::awaitable<std::vector<std::string>> PendingGroupsForDelivery(uint64_t actor);
    struct OutboxTask
    {
        uint64_t id = 0, actor = 0, peer = 0;
        int64_t seq = 0;
        int operation = 0, state = 0, stage = 0, attempts = 0;
        bool dispatchDone = false;
        std::string conversation, serverId, payload, leaseToken;
        // Outbox 字段说明：
        // operation：任务类型
        //   1 = 单聊发送；2 = 删除好友后的历史清理；3 = ACK 后的投递状态与缓存清理。
        // stage：单聊发送任务的处理阶段（operation=1）
        //   0 = 正文保存及会话事务尚未提交。
        //   1 = MongoDB 正文已保存，MySQL 会话更新与阶段标记已提交；不代表已投递。
        // state：任务生命周期状态
        //   0 = 待执行（到 available_at 后可领取）。
        //   1 = 已领取（由 lease_token 标识本次领取，lease_until 为租约到期时间）。
        //   2 = 任务完成（不代表用户已读）。
        //   3 = 已取消（如好友关系删除导致消息失效）。
    };
    static boost::asio::awaitable<void> InitializeOutbox();
    static boost::asio::awaitable<OutboxTask> QueueSingleMessage(sdkws::MsgData message);
    static boost::asio::awaitable<void> QueueConversationCleanup(boost::mysql::any_connection& connection,
        uint64_t actor, uint64_t peer, int64_t cutoff);
    static boost::asio::awaitable<bool> QueueMessageAck(uint64_t owner, std::string serverId);
    static boost::asio::awaitable<std::optional<OutboxTask>> ClaimOutboxTask();
    static boost::asio::awaitable<OutboxTask> GetOutboxTask(uint64_t id);
    static boost::asio::awaitable<void> ProcessOutboxTask(OutboxTask task, IMessageProducer& producer);
    static boost::asio::awaitable<void> RetryOutboxTask(OutboxTask task, std::string error);
    static boost::asio::awaitable<void> FilterVisibleMessages(std::vector<sdkws::MsgData>& messages);
    static boost::asio::awaitable<int64_t> FriendDeletionSeq(uint64_t owner, uint64_t peer);
    static boost::asio::awaitable<bool> MessageDispatchDone(uint64_t owner, std::string serverId);
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
    static boost::asio::awaitable<std::optional<conversation::Conversation>> FindConversation(
        uint64_t owner, std::string id);
    static boost::asio::awaitable<conversation::GetSortedConversationListResp> SortedConversations(
        conversation::GetSortedConversationListReq query);
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
