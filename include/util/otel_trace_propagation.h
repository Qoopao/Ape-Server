#ifndef OTEL_TRACE_PROPAGATION_H
#define OTEL_TRACE_PROPAGATION_H

#include <grpcpp/client_context.h>
#include <opentelemetry/common/key_value_iterable_view.h>
#include <opentelemetry/nostd/type_traits.h>
#include <opentelemetry/trace/span_context.h>
#include <opentelemetry/trace/tracer.h>

#include <string>

namespace ape {
namespace otel {

// ── gRPC metadata 传播 ──

// 将当前 active span 的 traceparent 注入到 gRPC ClientContext metadata
// 调用后，Server 端的 OTel 拦截器会自动提取并恢复父 Span
void InjectTraceContextToGrpcMetadata(grpc::ClientContext &ctx);

// ── 字符串编码（用于写入 Kafka / 其他文本载体）──

// 将当前 active span 编码为 W3C traceparent 字符串
// 格式: "00-{32 hex trace_id}-{16 hex span_id}-{2 hex flags}"
// 无 active span 时返回空字符串
std::string EncodeCurrentTraceParent();

// 解析 traceparent 字符串为 SpanContext（无效时返回 IsValid()==false）
opentelemetry::trace::SpanContext ParseTraceParent(const std::string &tp);

// 从 traceparent 字符串恢复父上下文并创建子 Span
// tp: W3C traceparent 字符串
// attrs 支持 initializer_list, std::vector<std::pair<...>> 等所有 KV 容器
template <class T,
          opentelemetry::nostd::enable_if_t<
              opentelemetry::common::detail::is_key_value_iterable<T>::value> * =
              nullptr>
inline opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>
StartSpanFromEncodedTraceParent(
    const std::string &tp,
    const std::string &span_name,
    const T &attrs,
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer)
{
    auto parent_ctx = ParseTraceParent(tp);
    if (!tracer || !parent_ctx.IsValid())
        return {};

    opentelemetry::trace::StartSpanOptions opts;
    opts.parent = parent_ctx;
    opts.kind   = opentelemetry::trace::SpanKind::kConsumer;
    return tracer->StartSpan(span_name, attrs, opts);
}

} // namespace otel
} // namespace ape

#endif
