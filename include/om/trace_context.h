#ifndef APE_TRACE_CONTEXT_H
#define APE_TRACE_CONTEXT_H

#include <grpcpp/client_context.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/span_context.h>
#include <opentelemetry/trace/span_metadata.h>
#include <opentelemetry/trace/tracer.h>

#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ape {
namespace otel {

// W3C traceparent 的显式载体。
//
// 为什么不用 opentelemetry 的线程本地 RuntimeContext：
// 本项目所有 handler 都在 co_spawn 出来的协程里执行，co_await 之后协程可能
// 在另一个 executor 线程恢复，而 RuntimeContext 是线程本地的。跨线程
// Attach/Detach 既会丢上下文，也会把 A 请求的上下文泄漏到 B 请求。
// 因此跨异步边界一律显式传递 TraceContext；线程本地上下文只作为同步段
// 的便利入口（FromActiveSpan）。
struct TraceContext
{
    std::string traceparent; // 空 = 无

    bool valid() const { return traceparent.size() == 55 && traceparent.rfind("00-", 0) == 0; }
    std::string traceId() const { return valid() ? traceparent.substr(3, 32) : std::string(); }
    std::string spanId() const { return valid() ? traceparent.substr(36, 16) : std::string(); }

    static TraceContext FromTraceParent(std::string_view traceparent);
    static TraceContext FromActiveSpan();
    // gRPC 客户端/服务端 metadata（multimap<string_ref,string_ref>）。
    static TraceContext FromMetadata(
        const std::multimap<grpc::string_ref, grpc::string_ref> &metadata);
};

using Attributes = std::vector<std::pair<std::string, std::string>>;

// 由 SpanContext 编码为 W3C traceparent；无效时返回空串。
std::string EncodeTraceParent(const opentelemetry::trace::SpanContext &context);
opentelemetry::trace::SpanContext ParseTraceParent(const std::string &traceparent);

// 当前 span（来自线程本地上下文，仅同步段使用）。
opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> CurrentSpan();
TraceContext TraceContextOf(const opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> &span);
TraceContext CurrentTraceContext();

// ── Span 生命周期 ──
//
// SpanHandle 只负责 Start/End，不创建 Scope。End() 可在任意线程调用
// （OTel Span 的 API 是线程安全的），因此可以安全地跨协程释放。
class SpanHandle
{
public:
    SpanHandle() = default;
    explicit SpanHandle(opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span);
    ~SpanHandle();

    SpanHandle(SpanHandle &&other) noexcept;
    SpanHandle &operator=(SpanHandle &&other) noexcept;
    SpanHandle(const SpanHandle &) = delete;
    SpanHandle &operator=(const SpanHandle &) = delete;

    bool valid() const { return static_cast<bool>(span_); }
    opentelemetry::trace::Span *span() const { return span_.get(); }
    TraceContext context() const { return TraceContextOf(span_); }

    SpanHandle &Attribute(std::string_view key, std::string_view value);
    SpanHandle &Attribute(std::string_view key, int64_t value);
    SpanHandle &Attribute(std::string_view key, double value);
    SpanHandle &Attribute(std::string_view key, bool value);

    void EndOk();
    void EndError(std::string_view message);
    void End();

private:
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span_;
};

// 新 trace（无父）。
SpanHandle StartRootSpan(std::string_view name, const Attributes &attributes = {},
                         opentelemetry::trace::SpanKind kind = opentelemetry::trace::SpanKind::kInternal);

// 父 = context；用于同步的跨进程/跨协程 hand-off。
SpanHandle StartChildSpan(const TraceContext &parent, std::string_view name,
                          const Attributes &attributes = {},
                          opentelemetry::trace::SpanKind kind = opentelemetry::trace::SpanKind::kInternal);

// 新 trace + link 指向 parent：用于异步投递尝试（outbox 重试、Kafka 消费）。
// 当前 SDK 的 StartSpanOptions 没有 links 字段，这里走 4 参数重载。
SpanHandle StartLinkedSpan(const TraceContext &origin, std::string_view name,
                           const Attributes &attributes = {},
                           opentelemetry::trace::SpanKind kind = opentelemetry::trace::SpanKind::kConsumer);

// ── 传播 ──

void InjectTraceContext(grpc::ClientContext &context, const TraceContext &trace);

// Kafka value 约定："traceparent|serverMsgID"；traceparent 为空时只写 id。
std::string EncodeKafkaValue(const TraceContext &trace, std::string_view message_id);
void DecodeKafkaValue(std::string_view value, std::string &traceparent, std::string &message_id);

} // namespace otel
} // namespace ape

#endif
