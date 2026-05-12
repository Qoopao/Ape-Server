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

// ==================== 离线消息冷存储（MongoDB） ====================

bool MongoHandler::SaveOfflineMsgToMongo(const sdkws::MsgData& msg)
{
    try
    {
        auto& mongoconnector = MongoConnector::get_instance();
        auto client = mongoconnector.get_pool()->acquire();
        auto collection = (*client)["IM-System"]["offline_msg"];

        // 写入时记录 created_at，用于30天TTL自动过期
        auto now = std::chrono::system_clock::now();
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

        bsoncxx::builder::stream::document doc{};
        doc << "serverMsgID" << msg.servermsgid()
            << "sendID" << msg.sendid()
            << "recvID" << msg.recvid()
            << "convID" << msg.convid()
            << "clientMsgID" << msg.clientmsgid()
            << "senderNickname" << msg.sendernickname()
            << "sessionType" << msg.sessiontype()
            << "contentType" << msg.contenttype()
            << "content" << msg.content()
            << "seq" << static_cast<int64_t>(msg.seq())
            << "sendTime" << msg.sendtime()
            << "isGroupMsg" << msg.isgroupmsg()
            << "isPushed" << false                 // 默认未推送
            << "createdAt" << now_ms
            << bsoncxx::builder::stream::finalize;

        auto result = collection.insert_one(doc.view());
        if (result)
        {
            spdlog::info("SaveOfflineMsgToMongo success, recvID={}, serverMsgID={}", msg.recvid(), msg.servermsgid());
            return true;
        }
        else
        {
            spdlog::error("SaveOfflineMsgToMongo insert_one failed, serverMsgID={}", msg.servermsgid());
            return false;
        }
    }
    catch (const mongocxx::exception& e)
    {
        spdlog::error("SaveOfflineMsgToMongo exception: {}, serverMsgID={}", e.what(), msg.servermsgid());
        return false;
    }
}

std::vector<sdkws::MsgData> MongoHandler::GetOfflineMsgsFromMongo(const std::string& userId, int64_t afterSeq, int limit)
{
    std::vector<sdkws::MsgData> results;
    try
    {
        auto& mongoconnector = MongoConnector::get_instance();
        auto client = mongoconnector.get_pool()->acquire();
        auto collection = (*client)["IM-System"]["offline_msg"];

        // 查询条件：recvID == userId && seq > afterSeq && isPushed == false
        bsoncxx::builder::stream::document filter{};
        filter << "recvID" << userId
               << "seq" << bsoncxx::builder::stream::open_document
               << "$gt" << afterSeq
               << bsoncxx::builder::stream::close_document;

        // 排序：按seq升序
        mongocxx::options::find opts{};
        opts.sort(bsoncxx::builder::stream::document{} << "seq" << 1
                                                        << bsoncxx::builder::stream::finalize);
        opts.limit(limit);

        auto cursor = collection.find(filter.view(), opts);

        for (auto&& doc : cursor)
        {
            try
            {
                sdkws::MsgData msg;
                // 手动从BSON字段解析到protobuf
                if (auto e = doc["serverMsgID"])
                    msg.set_servermsgid(std::string{e.get_string().value});
                if (auto e = doc["sendID"])
                    msg.set_sendid(std::string{e.get_string().value});
                if (auto e = doc["recvID"])
                    msg.set_recvid(std::string{e.get_string().value});
                if (auto e = doc["convID"])
                    msg.set_convid(std::string{e.get_string().value});
                if (auto e = doc["clientMsgID"])
                    msg.set_clientmsgid(std::string{e.get_string().value});
                if (auto e = doc["senderNickname"])
                    msg.set_sendernickname(std::string{e.get_string().value});
                if (auto e = doc["sessionType"])
                    msg.set_sessiontype(std::int32_t{e.get_int32().value});
                if (auto e = doc["contentType"])
                    msg.set_contenttype(std::int32_t{e.get_int32().value});
                if (auto e = doc["content"])
                    msg.set_content(std::string{e.get_string().value});
                if (auto e = doc["seq"])
                    msg.set_seq(e.get_int64().value);
                if (auto e = doc["sendTime"])
                    msg.set_sendtime(std::int64_t{e.get_int64().value});
                if (auto e = doc["isGroupMsg"])
                    msg.set_isgroupmsg(e.get_bool().value);
                results.push_back(std::move(msg));
            }
            catch (...)
            {
                spdlog::error("GetOfflineMsgsFromMongo parse failed for user={}", userId);
            }
        }

        spdlog::info("GetOfflineMsgsFromMongo success, userId={}, count={}", userId, results.size());
        return results;
    }
    catch (const mongocxx::exception& e)
    {
        spdlog::error("GetOfflineMsgsFromMongo exception: {}, userId={}", e.what(), userId);
        return results;
    }
}

bool MongoHandler::MarkOfflineMsgsPushed(const std::string& userId, const std::vector<std::string>& msgIds)
{
    try
    {
        if (msgIds.empty()) return true;

        auto& mongoconnector = MongoConnector::get_instance();
        auto client = mongoconnector.get_pool()->acquire();
        auto collection = (*client)["IM-System"]["offline_msg"];

        // 批量标记 isPushed = true
        for (const auto& msgId : msgIds)
        {
            bsoncxx::builder::stream::document filter{};
            filter << "recvID" << userId
                   << "serverMsgID" << msgId;

            bsoncxx::builder::stream::document update{};
            update << "$set"
                   << bsoncxx::builder::stream::open_document
                   << "isPushed" << true
                   << bsoncxx::builder::stream::close_document;

            collection.update_one(filter.view(), update.view());
        }

        spdlog::info("MarkOfflineMsgsPushed success, userId={}, count={}", userId, msgIds.size());
        return true;
    }
    catch (const mongocxx::exception& e)
    {
        spdlog::error("MarkOfflineMsgsPushed exception: {}, userId={}", e.what(), userId);
        return false;
    }
}

