#include "services/backbon_service/server.h"
#include <etcd/KeepAlive.hpp>
#include <etcd/Response.hpp>
#include <etcd/Value.hpp>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>
#include <util/etcdconnector.h>
#include <util/redisconnector.h>

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
    if (!method.empty()) { // 过滤空字符串（避免末尾逗号导致的空元素）
      vec.push_back(method);
    }
  }
  return vec;
}

// 检查用户是否在线
::grpc::ServerUnaryReactor *
BackbonServiceImpl::CheckUserOnline(::grpc::CallbackServerContext* context,
                const ::backbon::CheckUserOnlineReq *request,
                ::backbon::CheckUserOnlineResp *response) {
// 输入用户ID列表，返回这些列表的全部用户状态（包括离线与在线）。先查redis，再查持久化数据库
// 持久化数据库查到了放redis缓存


  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

// 注册服务，先存储服务信息到 etcd，再存redis
::grpc::ServerUnaryReactor *
BackbonServiceImpl::RegisterService(::grpc::CallbackServerContext* context,
                const ::backbon::RegisterServiceReq *request,
                ::backbon::RegisterServiceResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

  // 服务-IP:Port 存入etcd
  EtcdConnector &etcd_connector = EtcdConnector::GetInstance();
  std::string service_name = "/services/" + request->service();
  std::vector<std::string> ip_ports(request->ipport().begin(),
                                    request->ipport().end());

  etcd::Client& etcd_client = etcd_connector.get_etcd_client();

  // 1. 创建租约
  etcd::Response lease_resp =
      etcd_client.leasegrant(etcd_connector.get_lease_ttl()).get();
  if (!lease_resp.is_ok()) {
    std::string error_message =
        "创建租约失败: error_code: " + std::to_string(lease_resp.error_code()) +
        ", error_message: " + lease_resp.error_message();
    response->set_success(false);
    response->set_error(error_message);
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
  int64_t lease_id = lease_resp.value().lease();
  spdlog::info("创建租约成功，lease_id: {}", lease_id);

  // 2. 注册服务节点
  etcd::Response put_resp =
      etcd_client.put(service_name, serialize_vector(ip_ports), lease_id).get();
  if (!put_resp.is_ok()) {
    etcd_client.leaserevoke(lease_id).get(); // 注册失败时，及时释放租约
    std::string error_message = "注册服务节点失败: error_code: " +
                                std::to_string(put_resp.error_code()) +
                                ", error_message: " + put_resp.error_message();
    response->set_success(false);
    response->set_error(error_message);
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
  spdlog::info("注册服务节点成功：{}", service_name);



  // 3. 自动续约
  // 事件触发，
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

  // 服务-方法 存入redis
  Redis& redis = RedisConnector::get_instance().get_redis();
  std::vector<std::string> methods(request->methods().begin(),
                                   request->methods().end());
  try {
    redis.set(service_name, serialize_vector(methods), 60);
    spdlog::info("存入redis成功：{}", service_name);
  } catch (const std::exception &e) {
    std::string error_message = "存入redis失败";
    response->set_success(false);
    response->set_error(error_message);
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  response->set_success(true);
  response->set_error("");
  reactor->Finish(grpc::Status::OK);
  spdlog::info("Finish RegisterService: {}", service_name);
  return reactor;
}

// 注销服务
::grpc::ServerUnaryReactor *
BackbonServiceImpl::UnregisterService(::grpc::CallbackServerContext* context,
                  const ::backbon::UnregisterServiceReq *request,
                  ::backbon::UnregisterServiceResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

// 获取服务信息
::grpc::ServerUnaryReactor *
BackbonServiceImpl::GetService(::grpc::CallbackServerContext* context,
                                     const ::backbon::GetServiceReq *request,
                                     ::backbon::GetServiceResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

  EtcdConnector &etcd_connector = EtcdConnector::GetInstance();

  // 查询etcd服务信息
  std::string service_name = "/services/" + request->service();
  etcd::Client& etcd_client = etcd_connector.get_etcd_client();
  etcd::Response get_resp = etcd_client.get(service_name).get();
  if (get_resp.is_ok()) {
    std::string method_str = get_resp.value().as_string();
    std::vector<std::string> existing_methods = deserialize_vector(method_str);
    response->set_registered(true);
    response->set_service(method_str);
  } else {
    response->set_registered(false);
    response->set_service("");
  }

  reactor->Finish(grpc::Status::OK);
  return reactor;
}
