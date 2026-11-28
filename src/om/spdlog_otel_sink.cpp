#include "om/spdlog_otel_sink.h"

#include "om/obs_log.h"
#include "om/trace_context.h"

#include <opentelemetry/context/runtime_context.h>
#include <opentelemetry/logs/logger.h>
#include <opentelemetry/logs/severity.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/span_context.h>
#include <spdlog/common.h>

void SpdlogOtelSink::SetOtelLogger(
    opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> otel_logger)
{
    otel_logger_ = std::move(otel_logger);
}

void SpdlogOtelSink::sink_it_(const spdlog::details::log_msg &msg)
{
    if (!otel_logger_)
        return;

    // ── 1. spdlog level → OTel Severity ──
    auto severity = [](spdlog::level::level_enum lv) {
        switch (lv)
        {
        case spdlog::level::trace:    return opentelemetry::logs::Severity::kTrace;
        case spdlog::level::debug:    return opentelemetry::logs::Severity::kDebug;
        case spdlog::level::info:     return opentelemetry::logs::Severity::kInfo;
        case spdlog::level::warn:     return opentelemetry::logs::Severity::kWarn;
        case spdlog::level::err:      return opentelemetry::logs::Severity::kError;
        case spdlog::level::critical: return opentelemetry::logs::Severity::kFatal;
        default:                      return opentelemetry::logs::Severity::kInfo;
        }
    }(msg.level);

    auto record = otel_logger_->CreateLogRecord();
    if (!record)
        return;

    record->SetBody(opentelemetry::common::AttributeValue(
        std::string(msg.payload.data(), msg.payload.size())));

    // ── 2. 结构化字段：由 APE_LOG_* 门面在当前线程提供 ──
    const auto *fields = ape::log::CurrentFields();
    if (fields != nullptr)
    {
        for (const auto &field : *fields)
            record->SetAttribute(field.key, opentelemetry::common::AttributeValue(field.value));
        const auto &event = ape::log::CurrentEvent();
        if (!event.empty())
            record->SetAttribute("event", opentelemetry::common::AttributeValue(event));
    }

    // ── 3. trace 关联：优先显式 traceparent，其次线程本地 active span ──
    const auto &traceparent = ape::log::CurrentTraceParent();
    auto span_context = ape::otel::ParseTraceParent(traceparent);
    if (!span_context.IsValid())
    {
        auto span = opentelemetry::trace::GetSpan(
            opentelemetry::context::RuntimeContext::GetCurrent());
        if (span)
            span_context = span->GetContext();
    }
    if (span_context.IsValid())
    {
        record->SetTraceId(span_context.trace_id());
        record->SetSpanId(span_context.span_id());
        record->SetTraceFlags(span_context.trace_flags());
    }

    record->SetSeverity(severity);
    record->SetAttribute("thread.name",
                         opentelemetry::nostd::string_view(std::to_string(msg.thread_id)));

    otel_logger_->EmitLogRecord(std::move(record));
}
