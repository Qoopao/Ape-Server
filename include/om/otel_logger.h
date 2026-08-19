#ifndef OTEL_LOGGER_H
#define OTEL_LOGGER_H

namespace ape {
namespace otel {

// 初始化 OTel LoggerProvider + spdlog OTel sink
// 从此之后所有 spdlog::info/warn/error 都会带上 trace_id + span_id
// 通过 OTLP → Collector → Elasticsearch → 最终在 Kibana 中查看
void InitLogger();

} // namespace otel
} // namespace ape

#endif
