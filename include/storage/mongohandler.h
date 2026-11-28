#ifndef MONGOHANDLER_H
#define MONGOHANDLER_H

#include <cstdint>
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
    // 群消息正文与单聊共用 IM-System.msg；群文档 isGroupMsg=true、recvID=0、convID=groupID。
    enum class MsgCommitState { Missing, Uncommitted, Committed };

    static sdkws::MsgData DecodeMessage(bsoncxx::document::view document);
    static boost::asio::awaitable<std::vector<std::string>> DeleteSingleChatHistoryAsync(
        std::string conversation, int64_t cutoff = INT64_MAX);
    static boost::asio::awaitable<std::vector<std::string>> GetSingleChatMessageIDsAsync(
        std::string conversation, int64_t cutoff = INT64_MAX);
    static boost::asio::awaitable<std::vector<sdkws::MsgData>> GetSingleChatHistoryAsync(
        std::string conversation, int64_t afterSeq, int limit);

    int64_t	AppendMsgToConvMsgList(std::string conversationid, std::string msgid);
	std::vector<sdkws::MsgData> GetConvMessageList(std::string conversationid, int64_t cursor, int64_t limit, bool forward);

	bool UpdateUserConvList(uint64_t userid, std::string conversationid);
	std::vector<std::string> GetUserConvList(uint64_t userid, int64_t cursor, int64_t limit, bool forward);


	static bool SaveMsgToMongo(const sdkws::MsgData& msg, bool committed = true);
    static boost::asio::awaitable<void> PublishMessage(std::string serverId);
	static bool GetMsgByServerMsgID(const std::string& serverMsgID, sdkws::MsgData& outMsg);

	// 按 seq 范围拉取用户消息（从 IM-System.msg，统一在线/离线）
	static std::vector<sdkws::MsgData> GetMsgsBySeqFromMongo(const uint64_t userId, int64_t afterSeq, int limit = 100);
	// 标记消息为已送达（status = 2）

	// ── 异步版本（IOCPool worker 卸载，co_await 调用） ──
	static boost::asio::awaitable<bool> SaveMsgToMongoAsync(sdkws::MsgData msg, bool committed = true);
	static boost::asio::awaitable<std::optional<sdkws::MsgData>> GetMsgByServerMsgIDAsync(
      std::string serverMsgID);
	static boost::asio::awaitable<std::vector<sdkws::MsgData>> GetMsgsBySeqFromMongoAsync(uint64_t userId, int64_t afterSeq, int limit = 100);
	// PR-8：某会话在 afterSeq 之后最近 limit 条（倒序取再反转，保证升序）
	static boost::asio::awaitable<std::vector<sdkws::MsgData>> LatestAfterMessagesAsync(
		uint64_t owner, std::string conversationID, int64_t afterSeq, int limit);
	// 把某会话中 recvID=owner 且 seq<=upToSeq 的消息批量标记为已送达（status=2）
	static boost::asio::awaitable<bool> MarkConversationDeliveredUpTo(
		uint64_t owner, std::string conversationID, int64_t upToSeq);

	// ── 群聊（245 / 210 / outbox op=4） ──
	// 群历史向后：joinSeq < seq、seq>afterSeq、seq<=toSeq，升序取 limit+1（多取一条判断 hasMore）。
	static boost::asio::awaitable<std::vector<sdkws::MsgData>> GetGroupChatHistoryAsync(
		std::string groupID, int64_t afterSeq, int64_t toSeq, int limit);
	// 群历史向前：joinSeq < seq < beforeSeq、seq<=toSeq，倒序取 limit+1 后反转为升序。
	static boost::asio::awaitable<std::vector<sdkws::MsgData>> GetGroupLatestMessagesAsync(
		std::string groupID, int64_t joinSeq, int64_t beforeSeq, int64_t toSeq, int limit);
	// 只读 committed 字段：Missing / Uncommitted / Committed（缺失字段视为可见）。
	static boost::asio::awaitable<MsgCommitState> GetMsgCommitStateAsync(std::string serverMsgID);
	// outbox 放弃 Mongo 写：写 isDeleted=true、committed=true 的 tombstone，供 245 跳过并推进 cursor。
	static boost::asio::awaitable<bool> SaveGroupTombstoneAsync(std::string groupID, int64_t seq);
};

#endif
