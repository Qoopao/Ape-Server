#ifndef SPDLOG_OTEL_SINK_H
#define SPDLOG_OTEL_SINK_H

#include <opentelemetry/logs/logger.h>
#include <spdlog/sinks/base_sink.h>

// spdlog sink: 把每条 spdlog 日志转换为 OTel LogRecord,发送到 OTLP → Collector → ES
// 自动从当前线程的 OTel Context 中提取 trace_id / span_id,实现日志与 Trace 关联
class SpdlogOtelSink : public spdlog::sinks::base_sink<std::mutex>
{
public:
    // otel_logger: 预先创建好的 OTel Logger 实例
    void SetOtelLogger(
        opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> otel_logger);

private:
    void sink_it_(const spdlog::details::log_msg &msg) override;
    void flush_() override {}

    opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> otel_logger_;
};

#endif
