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

	bool SaveConversationInfo(std::string conversationid, sdkws::ConversationInfo conversation);
	std::optional<sdkws::ConversationInfo> GetConversationInfo(std::string conversationid);

	static bool SaveMsgToMongo(const sdkws::MsgData& msg, bool committed = true);
    static boost::asio::awaitable<void> PublishMessage(std::string serverId);
	static bool GetMsgByServerMsgID(const std::string& serverMsgID, sdkws::MsgData& outMsg);

	// 按 seq 范围拉取用户消息（从 IM-System.msg，统一在线/离线）
	static std::vector<sdkws::MsgData> GetMsgsBySeqFromMongo(const uint64_t userId, int64_t afterSeq, int limit = 100);
	// 标记消息为已送达（status = 2）
	static bool MarkMsgsAsDelivered(const uint64_t userId, const std::vector<std::string>& msgIds);

	// ── 异步版本（IOCPool worker 卸载，co_await 调用） ──
	static boost::asio::awaitable<bool> SaveMsgToMongoAsync(sdkws::MsgData msg, bool committed = true);
	static boost::asio::awaitable<std::optional<sdkws::MsgData>> GetMsgByServerMsgIDAsync(
      std::string serverMsgID);
	static boost::asio::awaitable<std::vector<sdkws::MsgData>> GetMsgsBySeqFromMongoAsync(uint64_t userId, int64_t afterSeq, int limit = 100);
	static boost::asio::awaitable<bool> MarkMsgsAsDeliveredAsync(uint64_t userId, std::vector<std::string> msgIds);

};

#endif
