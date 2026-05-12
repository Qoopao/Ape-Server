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
#include <optional>

#include "util/mongoconnector.h"
#include "util/mongohandler.h"
#include "user/userinfo.h"



// mongodb仅存消息记录，不存用户
mongocxx::collection MongoHandler::get_collection(const std::string db_name, const std::string collection_name)
{
    MongoConnector &mongoconnector = MongoConnector::get_instance();
    try{
        auto client = mongoconnector.get_pool()->acquire();
        return (*client)[db_name][collection_name];
    }
    catch (...)
    {
        throw;
    }
}

std::optional<bsoncxx::oid> MongoHandler::insert_user(userInfo user)
{
    try
    {
        auto collection = get_collection("IM-System", "user");

        bsoncxx::document::value doc = bsoncxx::builder::stream::document{}
                                       << "userid" << user.user_id
                                       << "username" << user.username
                                       << bsoncxx::builder::stream::finalize;

        auto result = collection.insert_one(doc.view());

        if (result)
        {
            auto insert_id = result->inserted_id().get_oid().value;
            spdlog::info("Successfully insert user: {}", insert_id.to_string());
            return insert_id;
        }
        else
        {
            spdlog::error("Failed to insert user");
            return std::nullopt;
        }
    }
    catch (const mongocxx::exception &e)
    {
        spdlog::error("Insert user exception: {}", e.what());
        return std::nullopt;
    }
}

std::optional<std::string> MongoHandler::find_user_by_id(uint16_t userid)
{
    try
    {
        auto collection = get_collection("IM-System", "user");

        bsoncxx::document::value filter = bsoncxx::builder::stream::document{}
                                          << "userid" << userid
                                          << bsoncxx::builder::stream::finalize;

        auto result = collection.find_one(filter.view());

        if (result)
        {
            // 将BSON文档转为JSON字符串返回
            std::string json_str = bsoncxx::to_json(*result);
            return json_str;
        }
        else
        {
            spdlog::warn("user_id: {} not found", userid);
            return std::nullopt;
        }
    }
    catch (const mongocxx::exception &e)
    {
        spdlog::error("find userid {} exception: ", e.what());
        return std::nullopt;
    }
}

bool MongoHandler::update_user(userInfo user)
{
    auto collection = get_collection("IM-System", "user");
    return true;
}

bool MongoHandler::delete_by_id(uint16_t userid)
{
    auto collection = get_collection("IM-System", "user");
    return true;
}

bool MongoHandler::SaveMsgToMongo(const sdkws::MsgData& msg)
{
    try
    {
        // 直接获取 pool entry 并保持其生命周期直到 insert_one 完成，
        // 避免 get_collection() 返回后 pool entry 析构导致 client 失效
        auto& mongoconnector = MongoConnector::get_instance();
        auto client = mongoconnector.get_pool()->acquire();
        auto collection = (*client)["IM-System"]["msg"];

        bsoncxx::builder::stream::document doc{};
        doc << "serverMsgID" << msg.servermsgid()
            << "sendID" << msg.sendid()
            << "recvID" << msg.recvid()
            << "convID" << msg.convid()
            << "clientMsgID" << msg.clientmsgid()
            << "senderPlatformID" << msg.senderplatformid()
            << "senderNickname" << msg.sendernickname()
            << "senderFaceURL" << msg.senderfaceurl()
            << "sessionType" << msg.sessiontype()
            << "msgFrom" << msg.msgfrom()
            << "contentType" << msg.contenttype()
            << "content" << msg.content()
            << "seq" << msg.seq()
            << "sendTime" << msg.sendtime()
            << "status" << msg.status()
            << "isRead" << msg.isread()
            << "isDeleted" << msg.isdeleted()
            << "isRecalled" << msg.isrecalled()
            << "isPinned" << msg.ispinned()
            << "isGroupMsg" << msg.isgroupmsg()
            << "ext" << msg.ext()
            << bsoncxx::builder::stream::finalize;

        auto result = collection.insert_one(doc.view());
        if (result)
        {
            spdlog::info("SaveMsgToMongo success, serverMsgID={}", msg.servermsgid());
            return true;
        }
        else
        {
            spdlog::error("SaveMsgToMongo insert_one failed, serverMsgID={}", msg.servermsgid());
            return false;
        }
    }
    catch (const mongocxx::exception& e)
    {
        spdlog::error("SaveMsgToMongo exception: {}, serverMsgID={}", e.what(), msg.servermsgid());
        return false;
    }
}


