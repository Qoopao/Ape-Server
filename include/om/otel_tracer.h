#ifndef OTEL_TRACER_H
#define OTEL_TRACER_H

#include "om/otel_config.h"

#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/trace/tracer.h>

#include <string>

namespace ape {
namespace otel {

// 初始化 TracerProvider + OTLP gRPC Exporter。
// 必须在任何 gRPC 服务启动前调用；OTEL_SDK_DISABLED=true 时直接跳过。
void InitTracer(const OtelConfig &config);

// 获取全局 tracer 实例（未初始化/已关闭时返回空）。
opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> GetTracer();

// 优雅关闭: flush 未发送的 span 并关闭 exporter。
void CleanupTracer();

} // namespace otel
} // namespace ape

#endif
