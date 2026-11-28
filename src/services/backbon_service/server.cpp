#include "services/backbon_service/server.h"
#include "storage/etcdconnector.h"
#include "storage/redisconnector.h"
#include "util/instance_id.h"

#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp>
#include <etcd/Response.hpp>
#include <etcd/Value.hpp>
#include <grpcpp/support/status.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace {

struct Registration {
  std::string instance_id;
  std::string address;
  std::vector<std::string> methods;
};

// etcd value 只有一种格式：{"instance_id":...,"address":...,"methods":[...]}
Registration parse_registration(const std::string &raw) {
  Registration out;
  const auto parsed = nlohmann::json::parse(raw, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object())
    return out;
  out.instance_id = parsed.value("instance_id", "");
  out.address = parsed.value("address", "");
  if (parsed.contains("methods") && parsed["methods"].is_array()) {
    for (const auto &method : parsed["methods"]) {
      if (method.is_string())
        out.methods.push_back(method.get<std::string>());
    }
  }
  return out;
}

} // namespace

BackbonServiceImpl::BackbonServiceImpl(const std::string &service_name,
                                       const std::string &listen_address)
    : BaseServiceServer<BackbonServiceImpl>(service_name, listen_address) {}

void BackbonServiceImpl::ReleaseKeepAlive() {
  std::lock_guard<std::mutex> lock(keep_alive_mutex_);
  keep_alives_.clear();
  leases_.clear();
}

void BackbonServiceImpl::ForgetKeepAlive(const std::string &key) {
  std::lock_guard<std::mutex> lock(keep_alive_mutex_);
  keep_alives_.erase(key);
  leases_.erase(key);
}

// ──────────────────────────────────────────
// 检查用户是否在线：以网关写入的路由表 gw:user:{account} 为准
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *BackbonServiceImpl::CheckUserOnline(
    ::grpc::CallbackServerContext *context,
    const ::backbon::CheckUserOnlineReq *request,
    ::backbon::CheckUserOnlineResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  std::vector<uint64_t> user_ids(request->userids().begin(),
                                 request->userids().end());

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, user_ids = std::move(user_ids)]()
          -> boost::asio::awaitable<void> {
        for (const auto userID : user_ids) {
          auto *status = response->add_statuses();
          status->set_userid(userID);
          bool online = false;
          try {
            const auto route = co_await RedisConnector::instance().get(
                "gw:user:" + std::to_string(userID));
            online = route.has_value() && !route->empty();
          } catch (const std::exception &e) {
            spdlog::error(
                "BackbonService::CheckUserOnline: Redis error for user {}: {}",
                userID, e.what());
          }
          status->set_isonline(online);
          status->set_connectioncount(online ? 1 : 0);
        }
        reactor->Finish(grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

// ──────────────────────────────────────────
// 注册服务：/services/{service}/{instanceId} -> JSON{instance_id,address,methods}
// 每个实例一个 key，不再互相覆盖（C-4）；不再写 Redis methods 镜像。
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *BackbonServiceImpl::RegisterService(
    ::grpc::CallbackServerContext *context,
    const ::backbon::RegisterServiceReq *request,
    ::backbon::RegisterServiceResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

  const std::string service = request->service();
  const std::string instance_id = request->instance_id();
  const std::string address = request->address();
  if (service.empty() || instance_id.empty() || address.empty()) {
    response->set_success(false);
    response->set_error("service, instance_id and address are required");
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  nlohmann::json payload;
  payload["instance_id"] = instance_id;
  payload["address"] = address;
  payload["methods"] =
      std::vector<std::string>(request->methods().begin(),
                               request->methods().end());

  const std::string key = "/services/" + service + "/" + instance_id;

  EtcdConnector &etcd_connector = EtcdConnector::GetInstance();
  etcd::Client &etcd_client = etcd_connector.get_etcd_client();

  etcd::Response lease_resp =
      etcd_client.leasegrant(etcd_connector.get_lease_ttl()).get();
  if (!lease_resp.is_ok()) {
    response->set_success(false);
    response->set_error("创建租约失败: " + lease_resp.error_message());
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
  const int64_t lease_id = lease_resp.value().lease();

  etcd::Response put_resp = etcd_client.put(key, payload.dump(), lease_id).get();
  if (!put_resp.is_ok()) {
    etcd_client.leaserevoke(lease_id).get();
    response->set_success(false);
    response->set_error("注册服务节点失败: " + put_resp.error_message());
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  std::function<void(std::exception_ptr)> handler = [](std::exception_ptr eptr) {
    try {
      if (eptr)
        std::rethrow_exception(eptr);
    } catch (const std::exception &e) {
      spdlog::error("etcd keepalive: {}", e.what());
    }
  };
  {
    std::lock_guard<std::mutex> lock(keep_alive_mutex_);
    keep_alives_.erase(key);
    keep_alives_[key] = std::make_unique<etcd::KeepAlive>(
        etcd_client, handler, etcd_connector.get_lease_ttl(), lease_id);
    leases_[key] = lease_id;
  }

  spdlog::info("注册服务成功：{} = {} (instance={})", key, payload.dump(),
               instance_id);
  response->set_success(true);
  response->set_error("");
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

// ──────────────────────────────────────────
// 注销服务：删除本实例的独立 key，并释放租约记录。
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *BackbonServiceImpl::UnregisterService(
    ::grpc::CallbackServerContext *context,
    const ::backbon::UnregisterServiceReq *request,
    ::backbon::UnregisterServiceResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

  const std::string service = request->service();
  const std::string instance_id = request->instance_id();
  if (service.empty() || instance_id.empty()) {
    response->set_success(false);
    response->set_error("service and instance_id are required");
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  const std::string key = "/services/" + service + "/" + instance_id;

  try {
    etcd::Client &etcd_client = EtcdConnector::GetInstance().get_etcd_client();
    etcd::Response del_resp = etcd_client.rm(key).get();
    if (del_resp.is_ok()) {
      spdlog::info("UnregisterService: 已从 etcd 删除 {}", key);
    } else {
      spdlog::warn("UnregisterService: etcd 删除失败 {}: {}", key,
                   del_resp.error_message());
    }
  } catch (const std::exception &e) {
    spdlog::error("UnregisterService: etcd 异常 {}: {}", key, e.what());
  }
  ForgetKeepAlive(key);

  response->set_success(true);
  response->set_error("");
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

// ──────────────────────────────────────────
// 获取服务信息：前缀列出全部实例，地址只从 instances 返回。
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *BackbonServiceImpl::GetService(
    ::grpc::CallbackServerContext *context,
    const ::backbon::GetServiceReq *request,
    ::backbon::GetServiceResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

  const std::string service = request->service();
  const std::string base = "/services/" + service;
  EtcdConnector &etcd_connector = EtcdConnector::GetInstance();
  etcd::Client &etcd_client = etcd_connector.get_etcd_client();

  bool registered = false;
  std::set<std::string> methods;
  std::set<std::string> seen_instances;

  auto add_instance = [&](const std::string &instance_id,
                          const std::string &address,
                          const std::vector<std::string> &parsed_methods) {
    if (instance_id.empty() || address.empty() ||
        !seen_instances.insert(instance_id).second) {
      return;
    }
    auto *instance = response->add_instances();
    instance->set_instance_id(instance_id);
    instance->set_address(address);
    for (const auto &method : parsed_methods)
      methods.insert(method);
    registered = true;
  };

  try {
    etcd::Response ls_resp = etcd_client.ls(base + "/").get();
    if (ls_resp.is_ok()) {
      for (const auto &value : ls_resp.values()) {
        const Registration parsed = parse_registration(value.as_string());
        std::string instance_id = parsed.instance_id;
        if (instance_id.empty()) {
          // value 里没写 instance_id 时退回 key 后缀（同一份契约的兜底，不涉及旧格式）
          const std::string key = value.key();
          instance_id = key.compare(0, base.size() + 1, base + "/") == 0
                            ? key.substr(base.size() + 1)
                            : key;
        }
        add_instance(instance_id, parsed.address, parsed.methods);
      }
    }
  } catch (const std::exception &e) {
    spdlog::warn("GetService: etcd ls 失败 {}: {}", base, e.what());
  }

  for (const auto &method : methods)
    response->add_methods(method);

  response->set_registered(registered);
  response->set_service(service);
  reactor->Finish(grpc::Status::OK);
  return reactor;
}
