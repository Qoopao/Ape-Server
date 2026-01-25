#pragma once
#include <cstdint>
#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp>
#include <spdlog/spdlog.h>

#define ETCD_SERVER_ADDR "http://127.0.0.1:2379"

class EtcdServiceRegistry {
public:
  EtcdServiceRegistry(std::string service_name, std::string service_ip,
                      uint16_t service_port, int lease_ttl = 20,
                      std::string etcd_endpoint = ETCD_SERVER_ADDR)
      : etcd_client(etcd_endpoint), lease_ttl(lease_ttl),
        service_name(service_name), service_ip(service_ip),
        service_port(service_port) {}

  ~EtcdServiceRegistry() = default;

  // 注册服务（创建租约+绑定endpoint+自动续约）
  void RegisterService();

  // 注销服务
  void UnregisterService();

private:
  etcd::Client etcd_client;                    // etcd客户端
  int lease_ttl;                               // 租约超时时间
  std::string service_name;                    // 服务名称
  std::string service_ip;                      // 服务IP
  uint16_t service_port;                       // 服务端口
  std::unique_ptr<etcd::KeepAlive> keep_alive; // 自动续约对象
};