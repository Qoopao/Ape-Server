#include "util/otel_trace_propagation.h"

#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/context/runtime_context.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/trace_flags.h>
#include <opentelemetry/trace/trace_id.h>
#include <opentelemetry/trace/trace_state.h>

#include <memory>
#include <string>

namespace ape {
namespace otel {

// ──────────────────────────────────────────
// Carrier: 将 traceparent 写入 gRPC ClientContext metadata
// ──────────────────────────────────────────
class GrpcClientMetadataCarrier
    : public opentelemetry::context::propagation::TextMapCarrier
{
public:
    explicit GrpcClientMetadataCarrier(grpc::ClientContext &ctx) : ctx_(ctx) {}

    // grpc的客户端不需要获取上下文，因为它是最上游的
    opentelemetry::nostd::string_view Get(
        opentelemetry::nostd::string_view) const noexcept override
    {
        return {};
    }

    void Set(opentelemetry::nostd::string_view key,
             opentelemetry::nostd::string_view value) noexcept override
    {
        ctx_.AddMetadata(std::string(key), std::string(value));
    }

private:
    grpc::ClientContext &ctx_;
};

// ──────────────────────────────────────────
// Carrier: 将 traceparent 写入 std::string
// ──────────────────────────────────────────
class StringCarrier : public opentelemetry::context::propagation::TextMapCarrier
{
public:
    opentelemetry::nostd::string_view Get(
        opentelemetry::nostd::string_view) const noexcept override
    {
        return {};
    }

    void Set(opentelemetry::nostd::string_view key,
             opentelemetry::nostd::string_view value) noexcept override
    {
        if (key == "traceparent")
            value_ = std::string(value);
    }

    std::string value_;
};

// ──────────────────────────────────────────
// 注入 traceparent 到 gRPC ClientContext
// ──────────────────────────────────────────
void InjectTraceContextToGrpcMetadata(grpc::ClientContext &ctx)
{
    auto span = opentelemetry::trace::GetSpan(
        opentelemetry::context::RuntimeContext::GetCurrent());
    if (!span || !span->GetContext().IsValid())
        return;

    opentelemetry::trace::propagation::HttpTraceContext propagator;
    GrpcClientMetadataCarrier carrier(ctx);
    auto current_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
    // carrier持有grpc client，Inject可以将sendSpan插入grpc的元数据里面
    propagator.Inject(carrier, current_ctx);
}

// ──────────────────────────────────────────
// 编码当前 active span 为 traceparent 字符串
// ──────────────────────────────────────────
std::string EncodeCurrentTraceParent()
{
    auto span = opentelemetry::trace::GetSpan(
        opentelemetry::context::RuntimeContext::GetCurrent());
    if (!span || !span->GetContext().IsValid())
        return "";

    opentelemetry::trace::propagation::HttpTraceContext propagator;
    StringCarrier carrier;
    auto current_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
    propagator.Inject(carrier, current_ctx);
    return carrier.value_;
}

// ──────────────────────────────────────────
// 解析 traceparent 字符串为 SpanContext
// 格式: "00-{32 hex trace_id}-{16 hex span_id}-{2 hex flags}"
// ──────────────────────────────────────────
opentelemetry::trace::SpanContext ParseTraceParent(const std::string &tp)
{
    if (tp.size() < 55 || tp[2] != '-')
        return opentelemetry::trace::SpanContext::GetInvalid();

    auto dash1 = tp.find('-', 3);
    auto dash2 = tp.find('-', dash1 + 1);
    if (dash1 == std::string::npos || dash2 == std::string::npos)
        return opentelemetry::trace::SpanContext::GetInvalid();

    opentelemetry::nostd::string_view trace_id_hex(tp.data() + 3, 32);
    opentelemetry::nostd::string_view span_id_hex(tp.data() + dash1 + 1, 16);
    opentelemetry::nostd::string_view flags_hex(tp.data() + dash2 + 1, 2);

    namespace otel_trace = opentelemetry::trace;
    auto trace_id =
        otel_trace::propagation::HttpTraceContext::TraceIdFromHex(trace_id_hex);
    auto span_id =
        otel_trace::propagation::HttpTraceContext::SpanIdFromHex(span_id_hex);
    auto flags =
        otel_trace::propagation::HttpTraceContext::TraceFlagsFromHex(flags_hex);

    if (!trace_id.IsValid() || !span_id.IsValid())
        return opentelemetry::trace::SpanContext::GetInvalid();

    return otel_trace::SpanContext(trace_id, span_id, flags, true,
                                   otel_trace::TraceState::GetDefault());
}

} // namespace otel
} // namespace ape
