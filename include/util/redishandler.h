#ifndef REDISHANDLER_H
#define REDISHANDLER_H


#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/array.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/json.hpp>

#include <spdlog/spdlog.h>
#include <string>
#include <vector>
#include <optional>

#include "sdkws.pb.h"
#include "util/redisconnector.h"
#include "user/userinfo.h"

using UpdateType = sw::redis::UpdateType;

class RedisHandler{
private:
    Redis& get_redis() { return RedisConnector::get_instance().get_redis(); }
public:

    static bool MsgToMQ(std::string key, std::string msgid);

    static bool SaveMsgInfo(sdkws::MsgData msg);
    static std::optional<sdkws::MsgData> GetMsgInfo(std::string msgid);



    static int64_t	AppendMsgToConvMsgList(std::string conversationid, std::string msgid);
	static std::vector<sdkws::MsgData> GetConvMessageList(std::string conversationid, int64_t cursor, int64_t limit, bool forward);

	static bool UpdateUserConvList(std::string userid, std::string conversationid);
	static std::vector<std::string> GetUserConvList(std::string userid, int64_t cursor, int64_t limit, bool forward);

	static bool SaveConversationInfo(std::string conversationid, sdkws::ConversationInfo conversation);
	static std::optional<sdkws::ConversationInfo> GetConversationInfo(std::string conversationid);
};

#endif
