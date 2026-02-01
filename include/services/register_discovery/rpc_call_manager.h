#ifndef RPC_CALL_MANAGER_H
#define RPC_CALL_MANAGER_H

#include <services/register_discovery/etcd_service_discovery.h>
#include <grpcpp/grpcpp.h>


class RpcCallManager {
public:
    RpcCallManager(const std::string& service_name, const std::string& etcd_endpoint = "http://127.0.0.1:2379")
    : discovery_(service_name, etcd_endpoint), round_robin_idx_(0) {
        RefreshNodes(); //刷新
        discovery_.StartWatch([this](const std::vector<ServiceNode>& nodes){
            UpdateConnPool(nodes);      // 节点变更时触发
        });
    };
    ~RpcCallManager(){
        discovery_.StopWatch();
    };

    // 获取可用grpc channel
    std::pair<std::shared_ptr<grpc::Channel>, std::string> GetAvailableChannelWithIpPort();

    // 标记节点故障
    void MarkNodeFault(const std::string& ip_port); // 用ip和port作为channel的唯一标识



private:
    EtcdServiceDiscovery discovery_;                  // 服务发现实例
    std::unordered_map<std::string, std::shared_ptr<grpc::Channel>> ip_port_to_conn_; // ip:port -> Channel
    std::vector<std::string> healthy_ip_ports_;                      // 健康Channel列表
    std::atomic<size_t> round_robin_idx_;                                           // 轮询索引
    std::mutex conn_mutex_;                                                          // 连接池锁

    // 刷新节点列表
    void RefreshNodes();

    // 更新连接池
    void UpdateConnPool(const std::vector<ServiceNode>& new_nodes);
};
#endif
