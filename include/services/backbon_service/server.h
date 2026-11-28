#ifndef BACKBON_SERVICE_SERVER_H
#define BACKBON_SERVICE_SERVER_H

#include "backbon.grpc.pb.h"
#include "backbon.pb.h"
#include "services/base_service.h"
#include <etcd/KeepAlive.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>

class BackbonServiceImpl final
    : public backbon::BackbonService::CallbackService,
      public BaseServiceServer<BackbonServiceImpl> {
public:
  BackbonServiceImpl(const std::string &service_name,
                     const std::string &listen_address);
  ~BackbonServiceImpl() { ReleaseKeepAlive(); }

  // 检查用户在线：以 gw:user:{account} 路由是否存在为准
  ::grpc::ServerUnaryReactor *
  CheckUserOnline(::grpc::CallbackServerContext *context,
                  const ::backbon::CheckUserOnlineReq *request,
                  ::backbon::CheckUserOnlineResp *response) override;

  // 注册服务：/services/{service}/{instanceId} -> JSON{address,methods}
  grpc::ServerUnaryReactor *
  RegisterService(::grpc::CallbackServerContext *context,
                  const ::backbon::RegisterServiceReq *request,
                  ::backbon::RegisterServiceResp *response) override;
  // 注销服务：按 instance_id 删除单个节点；为空则删整个服务前缀
  grpc::ServerUnaryReactor *
  UnregisterService(::grpc::CallbackServerContext *context,
                    const ::backbon::UnregisterServiceReq *request,
                    ::backbon::UnregisterServiceResp *response) override;
  // 获取服务信息：返回实例列表 + 方法集合
  ::grpc::ServerUnaryReactor *
  GetService(::grpc::CallbackServerContext *context,
             const ::backbon::GetServiceReq *request,
             ::backbon::GetServiceResp *response) override;

private:
  void ReleaseKeepAlive();
  void ForgetKeepAlive(const std::string &key_prefix);

  std::mutex keep_alive_mutex_;
  std::map<std::string, std::unique_ptr<etcd::KeepAlive>> keep_alives_;
  std::map<std::string, int64_t> leases_;
};

#endif
