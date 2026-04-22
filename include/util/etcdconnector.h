#ifndef ETCD_CONNECTOR_H
#define ETCD_CONNECTOR_H

#include <cstdint>
#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp> // 补充 KeepAlive 完整头文件，解决不完整类型问题
#include <memory>
#include <string>
#include <sys/stat.h>
class EtcdConnector {
public:
  static const int64_t DEFAULT_LEASE_TTL;
  static const std::string DEFAULT_ETCD_ENDPOINT;

  // 懒加载
  static EtcdConnector &
  GetInstance(int64_t lease_ttl = DEFAULT_LEASE_TTL,
              const std::string &etcd_endpoint = DEFAULT_ETCD_ENDPOINT) {
    static EtcdConnector instance(lease_ttl, etcd_endpoint);
    return instance;
  }

  // 禁用拷贝构造，赋值运算符
  EtcdConnector(const EtcdConnector &) = delete;
  EtcdConnector &operator=(const EtcdConnector &) = delete;

  int64_t get_lease_ttl() const { return lease_ttl; }
  std::string get_etcd_endpoint() const { return etcd_endpoint; }

  etcd::Client &get_etcd_client() { return etcd_client; }
  const etcd::Client &get_etcd_client() const { return etcd_client; }




private:
  EtcdConnector(int64_t lease_ttl, const std::string &etcd_endpoint)
      : lease_ttl(lease_ttl), etcd_endpoint(etcd_endpoint),
        etcd_client(etcd_endpoint) {}

  ~EtcdConnector() { }

  int64_t lease_ttl;
  std::string etcd_endpoint;

  etcd::Client etcd_client;
};

const int64_t EtcdConnector::DEFAULT_LEASE_TTL = 30;
const std::string EtcdConnector::DEFAULT_ETCD_ENDPOINT = "http://127.0.0.1:2379";

#endif // ETCD_CONNECTOR_H