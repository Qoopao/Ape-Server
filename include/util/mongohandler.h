#pragma once
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
#include "user/userinfo.h"

class MongoHandler{
private:
    mongocxx::collection get_collection(const std::string db_name, const std::string collection_name);
public:
    //Create
    std::optional<bsoncxx::oid> insert_user(userInfo user);

    //Read
    std::optional<std::string> find_user_by_id(uint16_t userid);

    //Update
    bool update_user(userInfo user);

    //Delete
    bool delete_by_id(uint16_t userid);
};