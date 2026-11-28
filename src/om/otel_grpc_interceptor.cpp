#include "om/otel_grpc_interceptor.h"

#include "om/otel_metrics.h"

#include <grpcpp/support/server_interceptor.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <memory>
#include <string>

// ──────────────────────────────────────────
// gRPC 服务端拦截器：只做 RED 指标。
//
// 不在这里创建 span：
//   1. handler 会把工作 co_spawn 到别的 executor，拦截器的线程本地上下文到不了
//      协程里；
//   2. 若同时在 handler 里建 span，同一个 RPC 会出现两个 span。
// 关键 handler（Msg/Group/Push/GatewayPush）用 ape::otel::TraceContext 显式
// 从 metadata 还原父上下文并自己建 span；其余 RPC 只有指标。
// ──────────────────────────────────────────
class OtelServerInterceptor : public grpc::experimental::Interceptor
{
public:
    OtelServerInterceptor(const char *method, const std::string &service_name)
        : method_(method), service_name_(service_name)
    {
    }

    void Intercept(grpc::experimental::InterceptorBatchMethods *methods) override
    {
        if (methods->QueryInterceptionHookPoint(
                grpc::experimental::InterceptionHookPoints::POST_RECV_INITIAL_METADATA))
        {
            start_time_ = std::chrono::steady_clock::now();
        }

        if (methods->QueryInterceptionHookPoint(
                grpc::experimental::InterceptionHookPoints::PRE_SEND_STATUS))
        {
            auto status = methods->GetSendStatus();
            const auto duration_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start_time_)
                                         .count();
            std::string status_str =
                status.ok() ? "OK" : std::to_string(static_cast<int>(status.error_code()));

            ape::otel::RpcServerRequests().Add(
                1,
                {{"rpc_service", service_name_.c_str()},
                 {"rpc_method", method_.c_str()},
                 {"status_code", status_str.c_str()}},
                opentelemetry::context::Context{});
            ape::otel::RpcServerDuration().Record(
                duration_ms,
                {{"rpc_service", service_name_.c_str()},
                 {"rpc_method", method_.c_str()}},
                opentelemetry::context::Context{});
        }

        methods->Proceed();
    }

private:
    std::string method_;
    std::string service_name_;
    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();
};

class OtelServerInterceptorFactory
    : public grpc::experimental::ServerInterceptorFactoryInterface
{
public:
    explicit OtelServerInterceptorFactory(const std::string &service_name)
        : service_name_(service_name)
    {
    }

    grpc::experimental::Interceptor *CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo *info) override
    {
        return new OtelServerInterceptor(info->method(), service_name_);
    }

private:
    std::string service_name_;
};

namespace ape {
namespace otel {

std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>
CreateOtelServerInterceptorFactory(const std::string &service_name)
{
    return std::make_unique<OtelServerInterceptorFactory>(service_name);
}

} // namespace otel
} // namespace ape
