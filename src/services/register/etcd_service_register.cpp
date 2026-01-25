#include "services/register/etcd_service_register.h"
#include <etcd/Response.hpp>
#include <etcd/Value.hpp>
#include <spdlog/spdlog.h>

// 注册单个服务（创建租约+注册服务节点+自动续约）
void EtcdServiceRegistry::RegisterService() {
  std::string service_key = "/services/" + this->service_name + "/" +
                            this->service_ip + ":" +
                            std::to_string(this->service_port);

  std::string service_value =
      "{\"ip\":\"" + this->service_ip +
      "\",\"port\":" + std::to_string(this->service_port) + ",\"name\":\"" +
      this->service_name + "\"}";

  etcd::Response get_resp = this->etcd_client.get(service_key).get();
  if (get_resp.is_ok()) {
    if (get_resp.value().as_string() == service_value) {
      spdlog::info("服务已注册，信息一致：{}", service_key);
    } else {
      spdlog::warn("服务key已存在但信息不一致，覆盖注册: {}", service_key);
    }
  } else {
    spdlog::info("注册服务节点: key: {}, value: {}", service_key,
                 service_value);
  }

  // 1. 创建租约
  etcd::Response lease_resp =
      this->etcd_client.leasegrant(this->lease_ttl).get();
  if (!lease_resp.is_ok()) {
    spdlog::error("创建租约失败: error_code: {}, error_message: {}",
                  lease_resp.error_code(), lease_resp.error_message());
    throw std::runtime_error("创建租约失败");
  }
  int64_t lease_id = lease_resp.value().lease();

  // 2. 注册服务节点
  etcd::Response put_resp =
      this->etcd_client.put(service_key, service_value, lease_id).get();
  if (!put_resp.is_ok()) {
    this->etcd_client.leaserevoke(lease_id).get(); // 注册失败时，及时释放租约
    throw std::runtime_error(spdlog::fmt_lib::format(
        "注册服务节点失败: error_code: {}, error_message: {}",
        put_resp.error_code(), put_resp.error_message()));
  }

  // 3. 自动续约
  // 事件触发，
  std::function<void(std::exception_ptr)> handler =
      [](std::exception_ptr eptr) {
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
  this->keep_alive = std::make_unique<etcd::KeepAlive>(
      this->etcd_client, handler, this->lease_ttl, lease_id);
  spdlog::info("服务注册成功：{}", service_key);
}

// 注销服务
void EtcdServiceRegistry::UnregisterService() {

  // 初始化关键变量
  std::string service_key = "/services/" + this->service_name + "/" +
                            this->service_ip + ":" +
                            std::to_string(this->service_port);
  int64_t lease_id = 0;

  // 1. 查询服务对应的key和租约ID
  etcd::Response get_resp = this->etcd_client.get(service_key).get();
  if (!get_resp.is_ok()) {
    // 服务未注册，释放续约资源，抛出异常
    this->keep_alive.reset();
    throw std::runtime_error(spdlog::fmt_lib::format(
        "服务未注册: error_code: {}, error_message: {}", get_resp.error_code(),
        get_resp.error_message()));
  }
  lease_id = get_resp.value().lease();

  // 2. 注销租约
  if (this->keep_alive) {
    this->keep_alive->Cancel(); // 停止续约
    this->keep_alive.reset();
    etcd::Response del_resp = this->etcd_client.leaserevoke(lease_id).get();  //同时删除key
    if (!del_resp.is_ok()) {
      throw std::runtime_error(spdlog::fmt_lib::format(
          "注销租约失败: error_code: {}, error_message: {}",
          del_resp.error_code(), del_resp.error_message()));
    }
    spdlog::info("删除服务成功: {}", service_key);
  } else {
    spdlog::warn("keep_alive为空");
  }
}
