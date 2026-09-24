#include "services/backbon_service/server.h"
#include <etcd/KeepAlive.hpp>
#include <etcd/Response.hpp>
#include <etcd/Value.hpp>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>
#include <storage/etcdconnector.h>
#include <storage/mysqlconnector.h>
#include <storage/mysqlhandler.h>
#include <storage/redisconnector.h>

#include <memory>
#include <string>

BackbonServiceImpl::BackbonServiceImpl(const std::string &service_name,
                                       const std::string &listen_address)
    : BaseServiceServer<BackbonServiceImpl>(service_name, listen_address) {}

// 序列化 vector<string> 为逗号分隔的字符串
std::string serialize_vector(const std::vector<std::string> &vec) {
    std::ostringstream oss;
    for (size_t i = 0; i < vec.size(); ++i) {
        if (i > 0) {
            oss << ",";
        }
        oss << vec[i];
    }
    return oss.str();
}

// 反序列化逗号分隔的字符串为 vector<string>
std::vector<std::string> deserialize_vector(const std::string &str) {
    std::vector<std::string> vec;
    std::istringstream iss(str);
    std::string method;
    while (std::getline(iss, method, ',')) {
        if (!method.empty()) {
            vec.push_back(method);
        }
    }
    return vec;
}

// ──────────────────────────────────────────
// 检查用户是否在线
// 对每个 userID: 先异步查 Redis，未命中再查持久化数据库
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *BackbonServiceImpl::CheckUserOnline(
    ::grpc::CallbackServerContext *context,
    const ::backbon::CheckUserOnlineReq *request,
    ::backbon::CheckUserOnlineResp *response) {

    grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

    // 拷贝 userIDs 列表到协程
    std::vector<uint64_t> user_ids(request->userids().begin(),
                                      request->userids().end());

    auto &redis = RedisConnector::instance();
    boost::asio::co_spawn(
        redis.get_executor(),
        [reactor, response, user_ids = std::move(user_ids)]()
            -> boost::asio::awaitable<void> {
            for (const auto &userID : user_ids) {
                auto *status = response->add_statuses();
                status->set_userid(userID);
                status->set_isonline(false);
                status->set_connectioncount(0);

                // 1. 异步查 Redis 在线状态
                try {
                    auto onlineVal = co_await RedisConnector::instance().get(
                        "user:" + std::to_string(userID) + ":online");
                    if (onlineVal && *onlineVal == "1") {
                        status->set_isonline(true);
                        status->set_connectioncount(1);
                        spdlog::debug(
                            "BackbonService::CheckUserOnline: user {} is online (from Redis)",
                            userID);
                        continue;
                    }
                } catch (const std::exception &e) {
                    spdlog::error(
                        "BackbonService::CheckUserOnline: Redis error for user {}: {}",
                        userID, e.what());
                }

                // 2. Redis 未命中 -> 查持久化数据库（异步）
                try {
                    auto userInfo = co_await MySQLHandler::find_user_by_id(userID);
                    if (userInfo) {
                        // 用户存在于 DB 中，缓存到 Redis 并标记离线
                        co_await RedisConnector::instance().setex(
                            "user:" + std::to_string(userID) + ":online", 300, "0");
                        spdlog::debug(
                            "BackbonService::CheckUserOnline: user {} found in DB (offline)",
                            userID);
                    }
                } catch (const std::exception &e) {
                    spdlog::error(
                        "BackbonService::CheckUserOnline: MySQL error for user {}: {}",
                        userID, e.what());
                }

                spdlog::debug(
                    "BackbonService::CheckUserOnline: user {} is offline",
                    userID);
            }

            reactor->Finish(grpc::Status::OK);
        },
        boost::asio::detached);

    return reactor;
}

// ──────────────────────────────────────────
// 注册服务：先存储服务信息到 etcd，再异步存 Redis
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *BackbonServiceImpl::RegisterService(
    ::grpc::CallbackServerContext *context,
    const ::backbon::RegisterServiceReq *request,
    ::backbon::RegisterServiceResp *response) {

    grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

    EtcdConnector &etcd_connector = EtcdConnector::GetInstance();
    std::string service_name = "/services/" + request->service();
    std::vector<std::string> ip_ports(request->ipport().begin(),
                                      request->ipport().end());

    etcd::Client &etcd_client = etcd_connector.get_etcd_client();

    // 1. 创建租约
    // TODO: co_spawn async — etcd leasegrant 是同步阻塞调用，后续应异步化
    etcd::Response lease_resp =
        etcd_client.leasegrant(etcd_connector.get_lease_ttl()).get();
    if (!lease_resp.is_ok()) {
        std::string error_message =
            "创建租约失败: error_code: " +
            std::to_string(lease_resp.error_code()) +
            ", error_message: " + lease_resp.error_message();
        response->set_success(false);
        response->set_error(error_message);
        reactor->Finish(grpc::Status::OK);
        return reactor;
    }
    int64_t lease_id = lease_resp.value().lease();
    spdlog::info("创建租约成功，lease_id: {}", lease_id);

    // 2. 注册服务节点
    // TODO: co_spawn async — etcd put 是同步阻塞调用，后续应异步化
    etcd::Response put_resp =
        etcd_client.put(service_name, serialize_vector(ip_ports), lease_id)
            .get();
    if (!put_resp.is_ok()) {
        etcd_client.leaserevoke(lease_id).get();
        std::string error_message =
            "注册服务节点失败: error_code: " +
            std::to_string(put_resp.error_code()) +
            ", error_message: " + put_resp.error_message();
        response->set_success(false);
        response->set_error(error_message);
        reactor->Finish(grpc::Status::OK);
        return reactor;
    }
    spdlog::info("注册服务节点成功：{}", service_name);

    // 3. 自动续约
    std::function<void(std::exception_ptr)> handler =
        [](std::exception_ptr eptr) {
            spdlog::info("自动续约触发");
            try {
                if (eptr) {
                    std::rethrow_exception(eptr);
                }
            } catch (const std::runtime_error &e) {
                spdlog::error("连接异常: {}", e.what());
            } catch (const std::out_of_range &e) {
                spdlog::error("租约异常: {}", e.what());
            }
        };
    this->keep_alive = std::move(std::make_unique<etcd::KeepAlive>(
        etcd_client, handler, etcd_connector.get_lease_ttl(), lease_id));
    spdlog::info("服务注册成功：{}", service_name);

    // 4. 异步存入 Redis（服务-方法映射）
    std::vector<std::string> methods(request->methods().begin(),
                                     request->methods().end());
    std::string serialized_methods = serialize_vector(methods);
    auto &redis = RedisConnector::instance();
    boost::asio::co_spawn(
        redis.get_executor(),
        [reactor, response, service_name,
         serialized_methods]() -> boost::asio::awaitable<void> {
            try {
                co_await RedisConnector::instance().setex(
                    service_name, 60, serialized_methods);
                spdlog::info("存入redis成功：{}", service_name);

                response->set_success(true);
                response->set_error("");
                reactor->Finish(grpc::Status::OK);
                spdlog::info("Finish RegisterService: {}", service_name);
            } catch (const std::exception &e) {
                std::string error_message = "存入redis失败";
                response->set_success(false);
                response->set_error(error_message);
                reactor->Finish(grpc::Status::OK);
            }
        },
        boost::asio::detached);

    return reactor;
}

// ──────────────────────────────────────────
// 注销服务：从 etcd 删除服务节点，从 redis 删除方法映射
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *BackbonServiceImpl::UnregisterService(
    ::grpc::CallbackServerContext *context,
    const ::backbon::UnregisterServiceReq *request,
    ::backbon::UnregisterServiceResp *response) {

    grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

    EtcdConnector &etcd_connector = EtcdConnector::GetInstance();
    std::string service_name = "/services/" + request->service();

    // 1. 从 etcd 删除服务节点
    // TODO: co_spawn async — etcd rm 是同步阻塞调用，后续应异步化
    try {
        etcd::Client &etcd_client = etcd_connector.get_etcd_client();
        etcd::Response del_resp = etcd_client.rm(service_name).get();
        if (del_resp.is_ok()) {
            spdlog::info("UnregisterService: 已从 etcd 删除 {}",
                         service_name);
        } else {
            spdlog::warn("UnregisterService: etcd 删除失败 {}: {}",
                         service_name, del_resp.error_message());
        }
    } catch (const std::exception &e) {
        spdlog::error("UnregisterService: etcd 异常 {}: {}", service_name,
                      e.what());
    }

    // 2. 异步从 redis 删除方法映射
    auto &redis = RedisConnector::instance();
    boost::asio::co_spawn(
        redis.get_executor(),
        [reactor, response, service_name]()
            -> boost::asio::awaitable<void> {
            try {
                co_await RedisConnector::instance().del(service_name);
                spdlog::info("UnregisterService: 已从 redis 删除 {}",
                             service_name);
            } catch (const std::exception &e) {
                spdlog::error(
                    "UnregisterService: redis 异常 {}: {}", service_name,
                    e.what());
            }

            response->set_success(true);
            response->set_error("");
            reactor->Finish(grpc::Status::OK);
        },
        boost::asio::detached);

    return reactor;
}

// ──────────────────────────────────────────
// 获取服务信息：从 etcd 查 ip:port，从 redis 异步查 methods
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *BackbonServiceImpl::GetService(
    ::grpc::CallbackServerContext *context,
    const ::backbon::GetServiceReq *request,
    ::backbon::GetServiceResp *response) {

    grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

    EtcdConnector &etcd_connector = EtcdConnector::GetInstance();
    std::string service_name = "/services/" + request->service();

    bool registered = false;

    // 1. 从 etcd 查询 ip:port 列表
    // TODO: co_spawn async — etcd get 是同步阻塞调用，后续应异步化
    etcd::Client &etcd_client = etcd_connector.get_etcd_client();
    etcd::Response get_resp = etcd_client.get(service_name).get();
    if (get_resp.is_ok()) {
        std::string ipport_str = get_resp.value().as_string();
        std::vector<std::string> ipports = deserialize_vector(ipport_str);
        for (const auto &ip : ipports) {
            response->add_ipport(ip);
        }
        registered = true;
    }

    // 2. 异步从 redis 查询 methods 列表
    auto &redis = RedisConnector::instance();
    boost::asio::co_spawn(
        redis.get_executor(),
        [reactor, response, service_name, registered,
         service_str = request->service()]()
            -> boost::asio::awaitable<void> {
            try {
                auto method_val =
                    co_await RedisConnector::instance().get(service_name);
                if (method_val) {
                    std::vector<std::string> methods =
                        deserialize_vector(*method_val);
                    for (const auto &m : methods) {
                        response->add_methods(m);
                    }
                }
            } catch (const std::exception &e) {
                spdlog::warn("GetService: redis 查询失败 {}: {}",
                             service_name, e.what());
            }

            response->set_registered(registered);
            response->set_service(service_str);
            reactor->Finish(grpc::Status::OK);
        },
        boost::asio::detached);

    return reactor;
}