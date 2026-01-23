#include "services/etcd_service_register.h"
#include <etcd/Response.hpp>
#include <spdlog/spdlog.h>

// 注册单个服务（创建租约+注册服务节点+自动续约）
bool EtcdServiceRegistry::RegisterService(const std::string &service_name,
                                          const std::string &service_ip,
                                          int service_port) {
  std::string service_key = "/services/" + service_name + "/" + service_ip +
                            ":" + std::to_string(service_port);
  etcd::Response get_resp = etcd_client.get(service_key).get();
  if (get_resp.is_ok()) {
    spdlog::info("服务重复注册：{}", service_key);
    return true;
  }

  // 1. 创建租约
  etcd::Response lease_resp = etcd_client.leasegrant(this->lease_ttl).get();
  if (!lease_resp.is_ok()) {
    spdlog::error("创建租约失败：{}", lease_resp.error_message());
    return false;
  }
  int lease_id = lease_resp.value().lease();

  // 2. 注册服务节点
  std::string service_value = "{\"ip\":\"" + service_ip +
                              "\",\"port\":" + std::to_string(service_port) +
                              ",\"name\":\"" + service_name + "\"}";
  etcd::Response put_resp =
      etcd_client.put(service_key, service_value, lease_id).get();
  if (!put_resp.is_ok()) {
    spdlog::error("注册服务节点失败：{}", put_resp.error_message());
    return false;
  }

  // 3. 自动续约
  auto keep_alive = std::make_unique<etcd::KeepAlive>(etcd_client, lease_id);

  spdlog::info("服务注册成功：{}", service_key);
  return true;
}

// 注销单个服务
bool EtcdServiceRegistry::UnregisterService(const std::string &service_name,
                                            const std::string &service_ip,
                                            int service_port) {
    std::string service_key = "/services/" + service_name + "/" + service_ip +
                            ":" + std::to_string(service_port);
    etcd::Response get_resp = etcd_client.get(service_key).get();
    if (!get_resp.is_ok()) {
        spdlog::info("服务未注册：{}", service_key);
        return false;
    }
    int lease_id = get_resp.value().lease();

    // 停止续约
    etcd::Response del_resp = etcd_client.leaserevoke(lease_id).get();
    if (!del_resp.is_ok()) {
        spdlog::error("注销服务节点失败：{}", del_resp.error_message());
        return false;
    }
    spdlog::info("注销成功：{}", service_key);
    return true;
}

// 停止所有服务注册
void EtcdServiceRegistry::StopAllRegister() {
    etcd::Response getAllResp = etcd_client.ls("/services",0).get();
    if (!getAllResp.is_ok()) {
        spdlog::error("获取所有服务节点失败：{}", getAllResp.error_message());
        return;
    }
    for (const auto &val : getAllResp.values()) {
        etcd::Response del_resp = etcd_client.leaserevoke(val.lease()).get();
        if (!del_resp.is_ok()) {
            spdlog::error("注销服务节点失败：{}", del_resp.error_message());
            continue;
        }
    }
    spdlog::info("所有服务注册已停止");
}
