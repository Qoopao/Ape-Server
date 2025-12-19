#include <mongocxx/instance.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/uri.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <mongocxx/exception/exception.hpp>
#include <spdlog/spdlog.h>

#include <string>

#include "util/mongoconnector.h"

MongoConnector& MongoConnector::get_instance(){
    static MongoConnector instance;
    return instance;
}

const mongocxx::client & MongoConnector::get_client()
{
    return client;
}

MongoConnector::MongoConnector(){
    db_username = "root";
    db_password = "root";

    std::string uri_string = "mongodb://" + db_username + ":" + db_password + "@localhost:27017/?authSource=admin";
    mongocxx::uri uri(uri_string);

    try{
        client = mongocxx::client(uri);
        bsoncxx::builder::stream::document ping_cmd;
        ping_cmd << "ping" << 1;
        client["admin"].run_command(ping_cmd.view()); // 这里如果失败，也会抛出mongocxx::exception
        spdlog::info("Successfully connected to MongoDB!");
    }
    catch(mongocxx::exception& e){
        spdlog::error("Failed to connect to mongo: {}",e.what());
    }
}

