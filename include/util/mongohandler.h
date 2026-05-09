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

#include <spdlog/spdlog.h>
#include <string>
#include <vector>
#include <optional>

#include "sdkws.pb.h"
#include "util/mongoconnector.h"
#include "user/userinfo.h"

class MongoHandler{
private:
    mongocxx::collection get_collection(const std::string db_name, const std::string collection_name);
public:
    // User
    std::optional<bsoncxx::oid> insert_user(userInfo user);
    std::optional<std::string> find_user_by_id(uint16_t userid);
    bool update_user(userInfo user);
    bool delete_by_id(uint16_t userid);


    int64_t	AppendMsgToConvMsgList(std::string conversationid, std::string msgid);
	std::vector<sdkws::MsgData> GetConvMessageList(std::string conversationid, int64_t cursor, int64_t limit, bool forward);

	bool UpdateUserConvList(std::string userid, std::string conversationid);
	std::vector<std::string> GetUserConvList(std::string userid, int64_t cursor, int64_t limit, bool forward);

	bool SaveConversationInfo(std::string conversationid, sdkws::ConversationInfo conversation);
	std::optional<sdkws::ConversationInfo> GetConversationInfo(std::string conversationid);
};

#endif
