#ifndef OTEL_GRPC_INTERCEPTOR_H
#define OTEL_GRPC_INTERCEPTOR_H

#include <grpcpp/support/interceptor.h>
#include <grpcpp/support/server_interceptor.h>

#include <memory>
#include <string>

namespace ape {
namespace otel {

// 创建并返回一个 ServerInterceptorFactory 实例
// service_name: 当前服务名 (如 "AuthService")
std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>
CreateOtelServerInterceptorFactory(const std::string &service_name);

} // namespace otel
} // namespace ape

#endif
