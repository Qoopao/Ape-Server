#include "services/register_discovery/etcd_service_discovery.h"
#include <cstdint>
#include <etcd/Response.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

std::vector<ServiceNode> EtcdServiceDiscovery::DiscoverServices() {
  if (is_destroyed_.load()) {
    spdlog::warn("DiscoverServices调用时对象已析构，返回空列表");
    return {};
  }

  std::string prefix = GetServicePrefixPath();
  try {
    etcd::Response resp = etcd_client.ls(prefix).get();
    std::vector<ServiceNode> nodes;
    if (!resp.is_ok()) {
      spdlog::error("发现服务[{}]失败: {}", service_name_,
                    resp.error_message());
      return nodes;
    }

    for (const auto &value : resp.values()) {
      auto node_opt = ParseServiceNode(value);
      if (node_opt) { // 仅添加解析成功的节点
        nodes.push_back(*node_opt);
      } else {
        spdlog::warn("跳过解析失败的节点: {}", value.key());
      }
    }

    // 更新本地缓存
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    current_nodes_ = nodes;
    spdlog::info("发现服务[{}]可用节点数: {}", service_name_, nodes.size());
    return nodes;
  } catch (const std::exception &e) {
    spdlog::error("发现服务[{}]失败: {}", service_name_, e.what());
    return {};
  }
}

std::optional<ServiceNode>
EtcdServiceDiscovery::ParseServiceNode(const etcd::Value &value) {
  try {
    nlohmann::json node_json = nlohmann::json::parse(value.as_string());
    ServiceNode node;
    node.service_name = node_json["name"];
    node.ip = node_json["ip"];
    node.port = node_json["port"];
    return node;
  } catch (const std::exception &e) {
    spdlog::error("解析服务节点[{}]失败: {}", value.key(), e.what());
    return std::nullopt; // 返回空，不抛异常
  }
}

void EtcdServiceDiscovery::StartWatch(
    const std::function<void(const std::vector<ServiceNode> &)> &callback) {
  std::lock_guard<std::mutex> lock(watch_mutex_);
  if (is_destroyed_.load()) {
    spdlog::warn("对象已析构，无法启动监听");
    return;
  }
  if (is_watching_.load()) {
    spdlog::warn("服务{}已启动监听，无需重复调用", service_name_);
    return;
  }

  change_callback_ = callback;
  std::string prefix = GetServicePrefixPath();

  spdlog::info("开始监听服务[{}]", service_name_);
  try {
    watcher_ = std::make_unique<etcd::Watcher>(
        etcd_client, prefix,
        [this](const etcd::Response &resp) {
          // 避免析构后再调用回调
          if (is_destroyed_.load()) {
            return;
          }

          if (!resp.is_ok()) {
            spdlog::error("监听服务[{}]变更失败: {}", service_name_,
                          resp.error_message());
            return;
          }
          // 触发节点变更回调
          std::vector<ServiceNode> new_nodes = DiscoverServices();
          {
            std::lock_guard<std::mutex> cb_lock(callback_mutex_);
            if (change_callback_) {
              change_callback_(new_nodes);
            }
          }
        },
        true // 开启递归监听
    );
    is_watching_.store(true);
    // watcher还活着就一直在监控，不需要主动调用wait，如果要调用wait，那么给他发一个Cancel信号就能解除阻塞
    // std::thread cancel_thread([this]() {
    //   std::this_thread::sleep_for(std::chrono::seconds(3));
    //   watcher_->Cancel();
    // });
    // cancel_thread.detach();
    // watcher_->Wait();
    spdlog::info("服务{}启动监听，前缀为{}", service_name_, prefix);
  } catch (const std::exception &e) {
    spdlog::error("创建Watcher失败: {}", e.what());
    change_callback_ = nullptr; // 回滚回调
    // 不设置is_watching_，保持false
  }
}
void EtcdServiceDiscovery::StopWatch() {
  std::lock_guard<std::mutex> lock(watch_mutex_);

  is_watching_.store(false);
  if (watcher_) {
    watcher_->Cancel();
    watcher_.reset();
    spdlog::info("停止监听服务[{}]", service_name_);
  }

  // 清空回调函数
  {
    std::lock_guard<std::mutex> cb_lock(callback_mutex_);
    change_callback_ = nullptr;
  }
}
