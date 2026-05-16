#ifndef OTEL_TRACER_H
#define OTEL_TRACER_H

#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/trace/tracer.h>

#include <string>

namespace ape {
namespace otel {

// 初始化 TracerProvider + OTLP gRPC Exporter
// 必须在任何 gRPC 服务启动前调用
// service_name: 在 Jaeger 中显示的服务名 (如 "AuthService")
void InitTracer(const std::string &service_name);

// 获取全局 tracer 实例
opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> GetTracer();

// 优雅关闭: flush 未发送的 span 并关闭 exporter
void CleanupTracer();

} // namespace otel
} // namespace ape

#endif
