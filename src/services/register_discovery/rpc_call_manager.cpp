#include "services/register_discovery/rpc_call_manager.h"
#include <cstddef>

std::pair<std::shared_ptr<grpc::Channel>, std::string> RpcCallManager::GetAvailableChannelWithIpPort() {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    if(healthy_ip_ports_.empty()){
        spdlog::error("无可用节点");
        return {nullptr, ""};
    }
    // 轮询获取可用channel
    size_t idx = round_robin_idx_.fetch_add(1, std::memory_order_relaxed) % healthy_ip_ports_.size();
    const std::string& ip_port = healthy_ip_ports_[idx];
    auto it = ip_port_to_conn_.find(ip_port);
    if (it != ip_port_to_conn_.end()) {
        return {it->second, ip_port};
    }
    return {nullptr, ""};
}

void RpcCallManager::MarkNodeFault(const std::string& ip_port){
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto it = std::find(healthy_ip_ports_.begin(), healthy_ip_ports_.end(), ip_port);
    if (it != healthy_ip_ports_.end()) {
        healthy_ip_ports_.erase(it);
        spdlog::warn("节点[{}]标记为故障，剩余健康节点数: {}", ip_port, healthy_ip_ports_.size());
    }
}



void RpcCallManager::RefreshNodes(){
    auto nodes = discovery_.DiscoverServices();
    UpdateConnPool(nodes);
}

void RpcCallManager::UpdateConnPool(const std::vector<ServiceNode>& new_nodes){
    std::lock_guard<std::mutex> lock(conn_mutex_);
    
    // 1. 构建新节点的ip_port集合
    std::unordered_set<std::string> new_ip_ports;
    for (const auto& node : new_nodes) {
        std::string ip_port = node.ip + ":" + std::to_string(node.port);
        new_ip_ports.insert(ip_port);
    }

    // 2. 删除已下线的节点连接
    std::vector<std::string> to_remove;
    for (const auto& [ip_port, channel] : ip_port_to_conn_) {
        if (new_ip_ports.find(ip_port) == new_ip_ports.end()) {
            to_remove.push_back(ip_port);
        }
    }
    for (const auto& ip_port : to_remove) {
        // 从健康列表删除
        auto it = std::find(healthy_ip_ports_.begin(), healthy_ip_ports_.end(), ip_port);
        if (it != healthy_ip_ports_.end()) {
            healthy_ip_ports_.erase(it);
        }
        // 从连接池删除
        ip_port_to_conn_.erase(ip_port);
        spdlog::info("Remove offline node [{}]", ip_port);
    }

    // 3. 添加新增的节点连接（故障节点重新上线也会被重新添加）
    for (const auto& node : new_nodes) {
        std::string ip_port = node.ip + ":" + std::to_string(node.port);
        if (ip_port_to_conn_.find(ip_port) == ip_port_to_conn_.end()) {
            auto channel = grpc::CreateChannel(ip_port, grpc::InsecureChannelCredentials());
            ip_port_to_conn_[ip_port] = channel;
            healthy_ip_ports_.push_back(ip_port);
            spdlog::info("Add new node [{}]", ip_port);
        } else if (std::find(healthy_ip_ports_.begin(), healthy_ip_ports_.end(), ip_port) == healthy_ip_ports_.end()) {
            // 故障节点重新上线，恢复到健康列表
            healthy_ip_ports_.push_back(ip_port);
            spdlog::info("Recover fault node [{}]", ip_port);
        }
    }

    // 重置轮询索引
    round_robin_idx_.store(0);
}
