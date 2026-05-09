#ifndef REDISCONNECTOR_H
#define REDISCONNECTOR_H

#include <memory>
#include <sw/redis++/redis++.h>
#include <bsoncxx/json.hpp>
#include <spdlog/spdlog.h>

#include <string>
#include <sw/redis++/redis.h>

using Redis = sw::redis::Redis;
using ConnectionOptions = sw::redis::ConnectionOptions;
using ConnectionPoolOptions = sw::redis::ConnectionPoolOptions;
using SentinelOptions = sw::redis::SentinelOptions;
using Sentinel = sw::redis::Sentinel;
using Role = sw::redis::Role;



class RedisConnector
{
private:
    RedisConnector();
    ~RedisConnector() = default;

    Redis redis_;

    sw::redis::Redis create_redis_instance();
public:
    RedisConnector &operator=(const RedisConnector &rc) = delete;
    RedisConnector(const RedisConnector &rc) = delete;

    static RedisConnector &get_instance();
    Redis& get_redis() { return redis_; }
};

#endif
