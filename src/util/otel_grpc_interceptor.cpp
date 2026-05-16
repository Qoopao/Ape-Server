#include "util/otel_grpc_interceptor.h"
#include "util/otel_tracer.h"

#include <grpcpp/support/server_interceptor.h>
#include <opentelemetry/context/propagation/global_propagator.h>
#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>
#include <opentelemetry/trace/scope.h>
#include <opentelemetry/trace/span_context.h>
#include <opentelemetry/trace/span_metadata.h>
#include <spdlog/spdlog.h>

#include <map>
#include <memory>
#include <string>

// ──────────────────────────────────────────
// gRPC metadata carrier: 桥接 gRPC metadata 和 OTel TextMapCarrier
// ──────────────────────────────────────────
class GrpcMetadataCarrier
    : public opentelemetry::context::propagation::TextMapCarrier
{
public:
    explicit GrpcMetadataCarrier(
        const std::multimap<grpc::string_ref, grpc::string_ref> *metadata)
        : metadata_(metadata)
    {
    }

    opentelemetry::nostd::string_view Get(
        opentelemetry::nostd::string_view key) const noexcept override
    {
        auto it = metadata_->find(grpc::string_ref(key.data(), key.size()));
        if (it != metadata_->end())
        {
            return opentelemetry::nostd::string_view(it->second.data(),
                                                     it->second.size());
        }
        return {};
    }

    void Set(opentelemetry::nostd::string_view /*key*/,
             opentelemetry::nostd::string_view /*value*/) noexcept override
    {
        // 服务端只需要 Extract，不需要 Inject，留空
    }

private:
    const std::multimap<grpc::string_ref, grpc::string_ref> *metadata_;
};

// ──────────────────────────────────────────
// 拦截器类 (每个 RPC 创建一次)
// ──────────────────────────────────────────
class OtelServerInterceptor : public grpc::experimental::Interceptor
{
public:
    OtelServerInterceptor(const char *method, const std::string& service_name)
        : method_(method), service_name_(service_name)
    {
    }

    void Intercept(
        grpc::experimental::InterceptorBatchMethods* methods) override
    {
        // 判断当前属于方法调用的阶段，调用同一个钩子函数的不同功能
        // ── 阶段 1: 收到客户端请求头 → 提取 traceparent → 创建 Span ──
        if (g_tracer &&
            methods->QueryInterceptionHookPoint(
                grpc::experimental::InterceptionHookPoints::
                    POST_RECV_INITIAL_METADATA))
        {
            auto* metadata = methods->GetRecvInitialMetadata();
            if (metadata)
            {
                // 从 gRPC metadata 中提取 W3C TraceContext
                GrpcMetadataCarrier carrier(metadata);
                opentelemetry::context::Context ctx;
                auto new_ctx = g_http_trace_context->Extract(carrier, ctx);
                auto span_ctx =
                    opentelemetry::trace::GetSpan(new_ctx)->GetContext();

                opentelemetry::trace::StartSpanOptions opts;
                opts.kind = opentelemetry::trace::SpanKind::kServer;
                if (span_ctx.IsValid())
                {
                    opts.parent = span_ctx;
                }

                span_ =
                    g_tracer->StartSpan(method_, {{"rpc.system", "grpc"},
                                                  {"rpc.service", service_name_},
                                                  {"rpc.method", method_}},
                                        opts);

                // 设为当前线程的 Active Span，后续 Client 调用能提取 traceparent
                scope_ = std::make_unique<opentelemetry::trace::Scope>(span_);
            }
        }

        // ── 阶段 2: 准备发送响应状态 → 结束 Span ──
        if (span_ &&
            methods->QueryInterceptionHookPoint(
                grpc::experimental::InterceptionHookPoints::PRE_SEND_STATUS))
        {
            auto status = methods->GetSendStatus();
            if (!status.ok())
            {
                span_->SetStatus(opentelemetry::trace::StatusCode::kError,
                                 status.error_message());
                span_->SetAttribute("rpc.grpc.status_code",
                                    static_cast<int>(status.error_code()));
            }
            else
            {
                span_->SetStatus(opentelemetry::trace::StatusCode::kOk, "");
                span_->SetAttribute("rpc.grpc.status_code", 0);
            }
            // 先销毁 Scope（恢复之前的 context），再 End Span
            scope_.reset();
            span_->End();
            span_ = nullptr;
        }

        methods->Proceed();
    }

    // 全局 tracer 和 propagator(由 main 初始化)
    static opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer>
        g_tracer;
    static std::unique_ptr<
        opentelemetry::trace::propagation::HttpTraceContext>
        g_http_trace_context;

private:
    std::string method_;
    std::string service_name_;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span_;
    std::unique_ptr<opentelemetry::trace::Scope> scope_;
};

// 静态成员定义
opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer>
    OtelServerInterceptor::g_tracer;
std::unique_ptr<opentelemetry::trace::propagation::HttpTraceContext>
    OtelServerInterceptor::g_http_trace_context;

// ──────────────────────────────────────────
// 拦截器工厂
// ──────────────────────────────────────────
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

// ──────────────────────────────────────────
// 公开接口: 创建工厂 + 设置全局 tracer/propagator
// ──────────────────────────────────────────
namespace ape {
namespace otel {

std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>
CreateOtelServerInterceptorFactory(const std::string &service_name)
{
    // 在首次调用时设置 tracer 和 propagator
    if (!OtelServerInterceptor::g_tracer)
    {
        OtelServerInterceptor::g_tracer = GetTracer();
    }
    if (!OtelServerInterceptor::g_http_trace_context)
    {
        OtelServerInterceptor::g_http_trace_context =
            std::make_unique<
                opentelemetry::trace::propagation::HttpTraceContext>();
    }

    return std::make_unique<OtelServerInterceptorFactory>(service_name);
}

} // namespace otel
} // namespace ape
