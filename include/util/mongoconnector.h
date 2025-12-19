#pragma once
#include <mongocxx/instance.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/uri.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/exception/exception.hpp>
#include <spdlog/spdlog.h>

#include <string>

class MongoConnector
{
private:
    MongoConnector();
    ~MongoConnector() = default;

    std::string db_username;
    std::string db_password;
    mongocxx::instance instance;
    mongocxx::client client;

public:
    MongoConnector &operator=(const MongoConnector &mc) = delete;
    MongoConnector(const MongoConnector &mc) = delete;

    static MongoConnector &get_instance();

    const mongocxx::client &get_client();
};