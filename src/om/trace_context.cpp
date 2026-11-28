#include "om/trace_context.h"

#include "om/otel_tracer.h"

#include <opentelemetry/common/key_value_iterable.h>
#include <opentelemetry/common/key_value_iterable_view.h>
#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/context/runtime_context.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>
#include <opentelemetry/trace/trace_flags.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace ape {
namespace otel {

namespace trace = opentelemetry::trace;
namespace context = opentelemetry::context;
namespace nostd = opentelemetry::nostd;
namespace common = opentelemetry::common;

namespace {

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

nostd::string_view View(std::string_view text)
{
    return nostd::string_view(text.data(), text.size());
}

// 一个只带 1 条 link 的迭代器；attributes 为空。
class SingleLink final : public trace::SpanContextKeyValueIterable
{
public:
    explicit SingleLink(trace::SpanContext span_context) : span_context_(span_context) {}

    bool ForEachKeyValue(
        nostd::function_ref<bool(trace::SpanContext, const common::KeyValueIterable &)>
            callback) const noexcept override
    {
        static const common::NoopKeyValueIterable empty;
        return callback(span_context_, empty);
    }

    size_t size() const noexcept override { return 1; }

private:
    trace::SpanContext span_context_;
};

trace::StartSpanOptions RootOptions(trace::SpanKind kind)
{
    trace::StartSpanOptions options;
    options.kind = kind;
    options.parent = context::Context{}.SetValue(trace::kIsRootSpanKey, true);
    return options;
}

bool IsHex(std::string_view text)
{
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

} // namespace

std::string EncodeTraceParent(const trace::SpanContext &span_context)
{
    if (!span_context.IsValid())
        return {};
    char trace_id[32]{};
    char span_id[16]{};
    char flags[2]{};
    span_context.trace_id().ToLowerBase16(trace_id);
    span_context.span_id().ToLowerBase16(span_id);
    span_context.trace_flags().ToLowerBase16(flags);
    // TraceFlags 输出大写十六进制；W3C 要求小写。
    flags[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(flags[0])));
    flags[1] = static_cast<char>(std::tolower(static_cast<unsigned char>(flags[1])));
    std::string result;
    result.reserve(55);
    result.append("00-");
    result.append(trace_id, 32);
    result.push_back('-');
    result.append(span_id, 16);
    result.push_back('-');
    result.append(flags, 2);
    return result;
}

trace::SpanContext ParseTraceParent(const std::string &traceparent)
{
    // 格式：00-<32hex>-<16hex>-<2hex>，长度固定 55。
    if (traceparent.size() != 55 || traceparent[2] != '-' || traceparent[35] != '-' ||
        traceparent[52] != '-')
        return trace::SpanContext::GetInvalid();
    const std::string_view version(traceparent.data(), 2);
    const std::string_view trace_id(traceparent.data() + 3, 32);
    const std::string_view span_id(traceparent.data() + 36, 16);
    const std::string_view flags(traceparent.data() + 53, 2);
    if (version != "00" || !IsHex(trace_id) || !IsHex(span_id) || !IsHex(flags))
        return trace::SpanContext::GetInvalid();

    auto trace_id_value = trace::propagation::HttpTraceContext::TraceIdFromHex(View(trace_id));
    auto span_id_value = trace::propagation::HttpTraceContext::SpanIdFromHex(View(span_id));
    auto flags_value = trace::propagation::HttpTraceContext::TraceFlagsFromHex(View(flags));
    if (!trace_id_value.IsValid() || !span_id_value.IsValid())
        return trace::SpanContext::GetInvalid();
    return trace::SpanContext(trace_id_value, span_id_value, flags_value, true,
                              trace::TraceState::GetDefault());
}

TraceContext TraceContext::FromTraceParent(std::string_view traceparent)
{
    TraceContext result;
    if (traceparent.size() == 55 && ParseTraceParent(std::string(traceparent)).IsValid())
        result.traceparent = ToLower(std::string(traceparent));
    return result;
}

TraceContext TraceContext::FromMetadata(
    const std::multimap<grpc::string_ref, grpc::string_ref> &metadata)
{
    const auto it = metadata.find(grpc::string_ref("traceparent"));
    if (it == metadata.end())
        return {};
    return FromTraceParent(std::string_view(it->second.data(), it->second.size()));
}

TraceContext TraceContext::FromActiveSpan()
{
    return TraceContextOf(CurrentSpan());
}

nostd::shared_ptr<trace::Span> CurrentSpan()
{
    return trace::GetSpan(context::RuntimeContext::GetCurrent());
}

TraceContext TraceContextOf(const nostd::shared_ptr<trace::Span> &span)
{
    TraceContext result;
    if (!span)
        return result;
    result.traceparent = EncodeTraceParent(span->GetContext());
    return result;
}

TraceContext CurrentTraceContext()
{
    return TraceContext::FromActiveSpan();
}

// ── SpanHandle ──

SpanHandle::SpanHandle(nostd::shared_ptr<trace::Span> span) : span_(std::move(span))
{
}

SpanHandle::~SpanHandle()
{
    End();
}

SpanHandle::SpanHandle(SpanHandle &&other) noexcept : span_(std::move(other.span_))
{
    other.span_ = {};
}

SpanHandle &SpanHandle::operator=(SpanHandle &&other) noexcept
{
    if (this != &other)
    {
        End();
        span_ = std::move(other.span_);
        other.span_ = {};
    }
    return *this;
}

SpanHandle &SpanHandle::Attribute(std::string_view key, std::string_view value)
{
    if (span_)
        span_->SetAttribute(View(key), View(value));
    return *this;
}

SpanHandle &SpanHandle::Attribute(std::string_view key, int64_t value)
{
    if (span_)
        span_->SetAttribute(View(key), value);
    return *this;
}

SpanHandle &SpanHandle::Attribute(std::string_view key, double value)
{
    if (span_)
        span_->SetAttribute(View(key), value);
    return *this;
}

SpanHandle &SpanHandle::Attribute(std::string_view key, bool value)
{
    if (span_)
        span_->SetAttribute(View(key), value);
    return *this;
}

void SpanHandle::EndOk()
{
    if (!span_)
        return;
    span_->SetStatus(trace::StatusCode::kOk, "");
    End();
}

void SpanHandle::EndError(std::string_view message)
{
    if (!span_)
        return;
    span_->SetStatus(trace::StatusCode::kError, View(message));
    span_->SetAttribute("error.message", View(message));
    End();
}

void SpanHandle::End()
{
    if (!span_)
        return;
    span_->End();
    span_ = {};
}

// ── 创建 ──

SpanHandle StartRootSpan(std::string_view name, const Attributes &attributes, trace::SpanKind kind)
{
    auto tracer = GetTracer();
    if (!tracer)
        return {};
    if (attributes.empty())
        return SpanHandle(tracer->StartSpan(View(name), RootOptions(kind)));
    return SpanHandle(tracer->StartSpan(
        View(name), common::KeyValueIterableView<Attributes>{attributes}, RootOptions(kind)));
}

SpanHandle StartChildSpan(const TraceContext &parent, std::string_view name,
                          const Attributes &attributes, trace::SpanKind kind)
{
    auto tracer = GetTracer();
    if (!tracer)
        return {};
    trace::StartSpanOptions options;
    options.kind = kind;
    options.parent = ParseTraceParent(parent.traceparent);
    if (attributes.empty())
        return SpanHandle(tracer->StartSpan(View(name), options));
    return SpanHandle(tracer->StartSpan(
        View(name), common::KeyValueIterableView<Attributes>{attributes}, options));
}

SpanHandle StartLinkedSpan(const TraceContext &origin, std::string_view name,
                           const Attributes &attributes, trace::SpanKind kind)
{
    auto tracer = GetTracer();
    if (!tracer)
        return {};
    trace::StartSpanOptions options = RootOptions(kind);
    const auto origin_context = ParseTraceParent(origin.traceparent);
    SingleLink link(origin_context);
    if (attributes.empty())
        return SpanHandle(tracer->StartSpan(View(name), common::NoopKeyValueIterable{}, link,
                                           options));
    return SpanHandle(tracer->StartSpan(
        View(name), common::KeyValueIterableView<Attributes>{attributes}, link, options));
}

void InjectTraceContext(grpc::ClientContext &client, const TraceContext &trace)
{
    if (trace.valid())
        client.AddMetadata("traceparent", trace.traceparent);
}

std::string EncodeKafkaValue(const TraceContext &trace, std::string_view message_id)
{
    if (!trace.valid())
        return std::string(message_id);
    std::string value;
    value.reserve(trace.traceparent.size() + 1 + message_id.size());
    value.append(trace.traceparent);
    value.push_back('|');
    value.append(message_id);
    return value;
}

void DecodeKafkaValue(std::string_view value, std::string &traceparent, std::string &message_id)
{
    traceparent.clear();
    const auto separator = value.find('|');
    if (separator != std::string_view::npos && separator == 55)
    {
        const auto candidate = value.substr(0, separator);
        if (ParseTraceParent(std::string(candidate)).IsValid())
        {
            traceparent.assign(candidate);
            message_id.assign(value.substr(separator + 1));
            return;
        }
    }
    message_id.assign(value);
}

} // namespace otel
} // namespace ape
