#include "util/spdlog_otel_sink.h"

#include <opentelemetry/context/runtime_context.h>
#include <opentelemetry/logs/logger.h>
#include <opentelemetry/logs/severity.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/span_context.h>
#include <spdlog/common.h>

void SpdlogOtelSink::SetOtelLogger(
    opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> otel_logger)
{
    otel_logger_ = otel_logger;
}

void SpdlogOtelSink::sink_it_(const spdlog::details::log_msg &msg)
{
    if (!otel_logger_)
        return;

    // ── 1. 从 spdlog level 转换为 OTel Severity ──
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

    // ── 2. 创建 LogRecord ──
    auto record = otel_logger_->CreateLogRecord();
    if (!record)
        return;

    // ── 3. 设置正文: spdlog fmt 的输出文本 ──
    record->SetBody(
        opentelemetry::common::AttributeValue(std::string(msg.payload.data(), msg.payload.size())));

    // ── 4. 从当前 OTel Context 提取 trace_id / span_id ──
    auto span = opentelemetry::trace::GetSpan(
        opentelemetry::context::RuntimeContext::GetCurrent());
    if (span)
    {
        auto ctx = span->GetContext();
        if (ctx.IsValid())
        {
            record->SetTraceId(ctx.trace_id());
            record->SetSpanId(ctx.span_id());
            record->SetTraceFlags(ctx.trace_flags());
        }
    }

    // ── 5. 设置时间戳和属性 ──
    record->SetSeverity(severity);
    std::string thread_name = std::to_string(msg.thread_id);
    record->SetAttribute("thread.name",
                         opentelemetry::nostd::string_view(thread_name));

    // ── 6. 发送到 OTel Logger → OTLP Exporter → Collector ──
    otel_logger_->EmitLogRecord(std::move(record));
}
