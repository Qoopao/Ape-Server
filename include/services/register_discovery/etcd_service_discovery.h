#ifndef ETCD_SERVICE_DISCOVERY_H
#define ETCD_SERVICE_DISCOVERY_H

#include "services/register_discovery/etcd_service_register.h"
#include <etcd/Watcher.hpp>
#include <optional>

struct ServiceNode{
    std::string service_name;
    std::string ip;
    uint16_t port;
};

class EtcdServiceDiscovery {
public:
  EtcdServiceDiscovery(const std::string &service_name,
                       const std::string &etcd_endpoint = "http://127.0.0.1:2379") :
                       etcd_client(etcd_endpoint),
                       service_name_(service_name),
                       is_watching_(false),
                       is_destroyed_(false){};

  ~EtcdServiceDiscovery(){
    is_destroyed_.store(true);
    StopWatch();
  }

  // 获取当前可用节点
  std::vector<ServiceNode> DiscoverServices();

  // 开始监听服务节点变更
  void StartWatch(const std::function<void(const std::vector<ServiceNode>&)>& callback);

  // 停止监听服务节点变更
  void StopWatch();

  // 禁拷贝/移动
  EtcdServiceDiscovery(const EtcdServiceDiscovery&) = delete;
  EtcdServiceDiscovery& operator=(const EtcdServiceDiscovery&) = delete;
  EtcdServiceDiscovery(EtcdServiceDiscovery&&) = delete;
  EtcdServiceDiscovery& operator=(EtcdServiceDiscovery&&) = delete;


private:
    etcd::Client etcd_client;                  // etcd客户端
    std::string service_name_;                 // 要发现的服务名称
    std::vector<ServiceNode> current_nodes_;   // 当前可用节点
    std::mutex nodes_mutex_;                   // 节点列表线程安全锁
    std::atomic<bool> is_watching_;            // 监听状态标记
    std::atomic<bool> is_destroyed_;           // 析构标记（防止回调访问已销毁对象）
    std::mutex watch_mutex_;                   // 保护Watcher创建/销毁的线程安全
    std::mutex callback_mutex_;               // 保护回调函数线程安全
    std::unique_ptr<etcd::Watcher> watcher_;     // etcd监听对象
    std::function<void(const std::vector<ServiceNode>&)> change_callback_;    // 节点变更回调

    // 将etcd的值转换为ServiceNode
    std::optional<ServiceNode> ParseServiceNode(const etcd::Value& value);
    // 获取服务前缀路径
    std::string GetServicePrefixPath(){
        return "/services/" + service_name_ + "/";
    }
};

#endif