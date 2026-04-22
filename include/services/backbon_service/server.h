#ifndef BACKBON_SERVICE_SERVER_H
#define BACKBON_SERVICE_SERVER_H

#include "backbon.grpc.pb.h"
#include "backbon.pb.h"
#include "services/base_service.h"
#include <etcd/KeepAlive.hpp>

class BackbonServiceImpl final
    : public backbon::BackbonService::CallbackService,
      public BaseServiceServer<BackbonServiceImpl> {
public:
  BackbonServiceImpl(const std::string &service_name,
                     const std::string &listen_address);
  ~BackbonServiceImpl() { ReleaseKeepAlive(); }

  // 检查用户是否在线
  ::grpc::ServerUnaryReactor *
  CheckUserOnline(::grpc::CallbackServerContext *context,
                  const ::backbon::CheckUserOnlineReq *request,
                  ::backbon::CheckUserOnlineResp *response) override;

  // 注册服务
  grpc::ServerUnaryReactor *
  RegisterService(::grpc::CallbackServerContext *context,
                  const ::backbon::RegisterServiceReq *request,
                  ::backbon::RegisterServiceResp *response) override;
  // 注销服务
  grpc::ServerUnaryReactor *
  UnregisterService(::grpc::CallbackServerContext *context,
                    const ::backbon::UnregisterServiceReq *request,
                    ::backbon::UnregisterServiceResp *response) override;
  // 获取服务信息
  grpc::ServerUnaryReactor *
  GetService(::grpc::CallbackServerContext *context,
             const ::backbon::GetServiceReq *request,
             ::backbon::GetServiceResp *response) override;

private:
  std::unique_ptr<etcd::KeepAlive> keep_alive;

  // 手动释放 KeepAlive
  void ReleaseKeepAlive() {
    if (keep_alive) {
      keep_alive.reset(); // 释放 unique_ptr，调用 KeepAlive 析构函数
    }
  }
};

#endif
