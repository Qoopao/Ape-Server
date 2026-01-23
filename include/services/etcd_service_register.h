#pragma once
#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp>

struct ServiceInfo {
    std::string service_name;
    std::string service_ip;
    int service_port;
    std::string service_key;
    std::unique_ptr<etcd::KeepAlive> keep_alive_ptr; 
};

class EtcdServiceRegistry {
public:
    EtcdServiceRegistry(const std::string& etcd_endpoint = "http://127.0.0.1:2379", int lease_ttl = 10)
        : etcd_client(etcd_endpoint), lease_ttl(lease_ttl) {}

    ~EtcdServiceRegistry() {
        StopAllRegister();
    }

    // 注册单个服务（创建租约+绑定endpoint+自动续约）
    bool RegisterService(const std::string& service_name,
                        const std::string& service_ip,
                        int service_port);

    // 注销单个服务
    bool UnregisterService(const std::string& service_name, const std::string& service_ip, int service_port);

    // 停止所有服务注册
    void StopAllRegister();

private:
    etcd::Client etcd_client;                           // etcd客户端
    int lease_ttl;                                      // 租约超时时间
};