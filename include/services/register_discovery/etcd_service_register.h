#ifndef ETCD_SERVICE_REGISTER_H
#define ETCD_SERVICE_REGISTER_H

#include <cstdint>
#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp>
#include <spdlog/spdlog.h>

// 服务配置结构体
struct ServiceConfig {
  std::string service_name;
  std::string service_ip;
  uint16_t service_port;
  int64_t lease_ttl = 30;
  std::string etcd_endpoint = "http://127.0.0.1:2379";
};


class EtcdServiceRegistry {
public:
  EtcdServiceRegistry(const ServiceConfig &config)
      : etcd_client(config.etcd_endpoint), sconfig(config) {}

  ~EtcdServiceRegistry() = default;

  // 注册服务（创建租约+绑定endpoint+自动续约）
  void RegisterService();
  // 注销服务
  void UnregisterService();

  // 禁拷贝/移动，单例模式
  EtcdServiceRegistry(const EtcdServiceRegistry &) = delete;
  EtcdServiceRegistry &operator=(const EtcdServiceRegistry &) = delete;
  EtcdServiceRegistry(EtcdServiceRegistry &&) = delete;
  EtcdServiceRegistry &operator=(EtcdServiceRegistry &&) = delete;

private:
  etcd::Client etcd_client;                    // etcd客户端
  ServiceConfig sconfig;                       // 服务配置
  std::unique_ptr<etcd::KeepAlive> keep_alive; // 自动续约对象
};

#endif
