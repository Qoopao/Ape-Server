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

#include "util/mongoconnector.h"
#include "util/mongohandler.h"
#include "user/userinfo.h"

mongocxx::collection MongoHandler::get_collection(const std::string db_name, const std::string collection_name)
{
    MongoConnector &mongoconnector = MongoConnector::get_instance();
    const mongocxx::client &client = mongoconnector.get_client();
    return client[db_name][collection_name];
}

// Create
std::optional<bsoncxx::oid> MongoHandler::insert_user(userInfo user)
{
    try
    {
        auto collection = get_collection("IM-System", "user");

        bsoncxx::document::value doc = bsoncxx::builder::stream::document{}
                                       << "userid" << user.userid
                                       << "username" << user.username
                                       << "email" << user.email
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

// Read
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

// Update
bool MongoHandler::update_user(userInfo user)
{
    auto collection = get_collection("IM-System", "user");
}

// Delete
bool MongoHandler::delete_by_id(uint16_t userid)
{
    auto collection = get_collection("IM-System", "user");
}