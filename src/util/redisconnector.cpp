#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <spdlog/spdlog.h>

#include <string>

#include "util/redisconnector.h"

RedisConnector &RedisConnector::get_instance() {
  static RedisConnector instance;
  return instance;
}

RedisConnector::RedisConnector() : redis_(create_redis_instance()) {
  // 测试是否连接成功
  if (redis_.ping() == "PONG") {
    spdlog::info("Successfully connected to Redis!");
  } else {
    spdlog::error("Failed to connect to redis");
  }
}

sw::redis::Redis RedisConnector::create_redis_instance() {
  // 配置哨兵参数
  SentinelOptions sentinel_opts;
  // 哨兵节点列表
  sentinel_opts.nodes = {
      {"127.0.0.1", 26379}, // 哨兵1
      {"127.0.0.1", 26380}, // 哨兵2
      {"127.0.0.1", 26381}  // 哨兵3
  };

  // Optional. Timeout before we successfully connect to Redis Sentinel.
  // By default, the timeout is 100ms.
  sentinel_opts.connect_timeout = std::chrono::milliseconds(200);

  // Optional. Timeout before we successfully send request to or receive
  // response from Redis Sentinel. By default, the timeout is 100ms.
  sentinel_opts.socket_timeout = std::chrono::milliseconds(200);

  auto sentinel = std::make_shared<Sentinel>(sentinel_opts);

  ConnectionOptions connection_opts;
  connection_opts.password = "redis123"; // Optional. No password by default.
  connection_opts.connect_timeout = std::chrono::milliseconds(100); // Required.
  connection_opts.socket_timeout = std::chrono::milliseconds(100);  // Required.

  // 配置连接池
  sw::redis::ConnectionPoolOptions pool_opts;
  pool_opts.size = 3;
  pool_opts.connection_idle_time = std::chrono::milliseconds(60000);

  // 为了简单，这里永远返回的是主节点，可以通过修改角色来获取从节点。
  // 可以把实例分开，读和写分别用从和主
  return Redis(sentinel, "mymaster", Role::MASTER, connection_opts, pool_opts);
}
