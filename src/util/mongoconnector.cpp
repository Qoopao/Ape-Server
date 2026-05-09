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

MongoConnector::MongoConnector(){
    db_username = "root";
    db_password = "root";
    min_pool_size = 5;
    max_pool_size = 10;
    mongo_instance = mongocxx::instance{};

    std::string uri_string = "mongodb://" + db_username + ":" + db_password + "@localhost:27017/?authSource=admin&minPoolSize=" + std::to_string(min_pool_size) + "&maxPoolSize=" + std::to_string(max_pool_size);
    mongocxx::uri uri(uri_string);
    
    try{
        pool = std::make_shared<mongocxx::pool>(uri);

        // 这里取出一个client，测试连接是否成功
        auto client = pool->acquire();
        bsoncxx::builder::stream::document ping_cmd;
        ping_cmd << "ping" << 1;
        client["admin"].run_command(ping_cmd.view()); // 这里如果失败，也会抛出mongocxx::exception
        spdlog::info("Successfully connected to MongoDB!");
    }
    catch(mongocxx::exception& e){
        spdlog::error("Failed to connect to mongo: {}",e.what());
    }
}

