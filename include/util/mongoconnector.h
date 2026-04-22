#ifndef MONGOCONNECTOR_H
#define MONGOCONNECTOR_H

#include <bsoncxx/json.hpp>
#include <memory>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>
#include <spdlog/spdlog.h>

#include <string>

class MongoConnector {
private:
  MongoConnector();
  ~MongoConnector() = default;

  std::string db_username;
  std::string db_password;
  mongocxx::instance mongo_instance;

  uint32_t min_pool_size;
  uint32_t max_pool_size;
  std::shared_ptr<mongocxx::pool> pool;

public:
  MongoConnector &operator=(const MongoConnector &mc) = delete;
  MongoConnector(const MongoConnector &mc) = delete;

  static MongoConnector &get_instance();
  std::shared_ptr<mongocxx::pool> get_pool() {
    if (pool) {
      return pool;
    }
    throw std::runtime_error("Call Constructor before get mongo pool.");
  }
};

#endif
