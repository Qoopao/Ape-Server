#ifndef MONGOHANDLER_H
#define MONGOHANDLER_H

#include <mongocxx/instance.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/uri.hpp>
#include <mongocxx/collection.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/array.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/json.hpp>

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>
#include <optional>

#include "sdkws.pb.h"
#include "storage/mongoconnector.h"
#include "user/userinfo.h"

class MongoHandler{
private:
    mongocxx::collection get_collection(const std::string db_name, const std::string collection_name);
public:

    int64_t	AppendMsgToConvMsgList(std::string conversationid, std::string msgid);
	std::vector<sdkws::MsgData> GetConvMessageList(std::string conversationid, int64_t cursor, int64_t limit, bool forward);

	bool UpdateUserConvList(std::string userid, std::string conversationid);
	std::vector<std::string> GetUserConvList(std::string userid, int64_t cursor, int64_t limit, bool forward);

	bool SaveConversationInfo(std::string conversationid, sdkws::ConversationInfo conversation);
	std::optional<sdkws::ConversationInfo> GetConversationInfo(std::string conversationid);

	static bool SaveMsgToMongo(const sdkws::MsgData& msg);
	static bool GetMsgByServerMsgID(const std::string& serverMsgID, sdkws::MsgData& outMsg);

	// 按 seq 范围拉取用户消息（从 IM-System.msg，统一在线/离线）
	static std::vector<sdkws::MsgData> GetMsgsBySeqFromMongo(const std::string& userId, int64_t afterSeq, int limit = 100);
	// 标记消息为已送达（status = 2）
	static bool MarkMsgsAsDelivered(const std::string& userId, const std::vector<std::string>& msgIds);

	// ── 异步版本（IOCPool worker 卸载，co_await 调用） ──
	static boost::asio::awaitable<bool> SaveMsgToMongoAsync(sdkws::MsgData msg);
	static boost::asio::awaitable<std::optional<sdkws::MsgData>> GetMsgByServerMsgIDAsync(
      std::string serverMsgID);
	static boost::asio::awaitable<std::vector<sdkws::MsgData>> GetMsgsBySeqFromMongoAsync(std::string userId, int64_t afterSeq, int limit = 100);
	static boost::asio::awaitable<bool> MarkMsgsAsDeliveredAsync(std::string userId, std::vector<std::string> msgIds);

	// 单条消息状态更新（对应 sdkws.MsgData.status / dStatus）
	static bool MarkMsgAsDelivered(const std::string &serverMsgID);         // status=2  在线推送成功
	static bool MarkMsgAsPushFailed(const std::string &serverMsgID);        // status=3, dStatus|=1  在线推送失败降级
	static bool MarkMsgAsOffline(const std::string &serverMsgID);           // dStatus|=1  接收者离线
	static boost::asio::awaitable<bool> MarkMsgAsDeliveredAsync(std::string serverMsgID);
	static boost::asio::awaitable<bool> MarkMsgAsPushFailedAsync(std::string serverMsgID);
	static boost::asio::awaitable<bool> MarkMsgAsOfflineAsync(std::string serverMsgID);

	// 查询消息是否已被消费（幂等兜底）
	static bool IsMsgAlreadyConsumed(const std::string &serverMsgID);
	static boost::asio::awaitable<bool> IsMsgAlreadyConsumedAsync(std::string serverMsgID);

	// 查询会话是否存在
	static bool DoesConversationExist(const std::string &convID);
	static boost::asio::awaitable<bool> DoesConversationExistAsync(
	    std::string convID);

	// 创建单聊会话（首次发消息时调用，为双方各创建一条 Conversation 记录）
	static bool CreateSingleChatConversations(const std::string &sendID,
	                                           const std::string &recvID,
	                                           const std::string &convID);
	static boost::asio::awaitable<bool> CreateSingleChatConversationsAsync(
	    std::string sendID, std::string recvID, std::string convID);

	// 创建群聊会话（为每个成员各创建一条 Conversation 记录，conversationType=2）
	static bool CreateGroupChatConversations(const std::string &groupID,
	                                          const std::vector<std::string> &userIDs);
	static boost::asio::awaitable<bool> CreateGroupChatConversationsAsync(
	    std::string groupID, std::vector<std::string> userIDs);

	// 按群 ID 列表批量拉取群消息（用于 PullMessageBySeqs 冷存储回退）
	static std::vector<sdkws::MsgData> GetGroupMsgsBySeqFromMongo(
	    const std::vector<std::string> &groupIDs, int64_t afterSeq, int limit = 100);
	static boost::asio::awaitable<std::vector<sdkws::MsgData>> GetGroupMsgsBySeqFromMongoAsync(
	    std::vector<std::string> groupIDs, int64_t afterSeq, int limit = 100);
};

#endif
