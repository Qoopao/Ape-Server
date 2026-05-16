#ifndef OTEL_METRICS_H
#define OTEL_METRICS_H

#include <opentelemetry/metrics/sync_instruments.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/unique_ptr.h>

#include <string>

namespace ape {
namespace otel {

// ── 初始化 MeterProvider + OTLP gRPC Metrics Exporter ──
void InitMeter(const std::string &service_name);

// ── 优雅关闭: flush 未发送的数据 ──
void CleanupMeter();

// ── 获取预定义的 metric instruments ──

// Counter: gRPC 请求总数
opentelemetry::metrics::Counter<uint64_t> &RpcServerRequests();

// Histogram: gRPC 请求延迟分布 (ms)
opentelemetry::metrics::Histogram<double> &RpcServerDuration();

// UpDownCounter: 活跃 WebSocket 连接数 (导出为 Prometheus Gauge)
opentelemetry::metrics::UpDownCounter<int64_t> &WsConnectionsActive();

} // namespace otel
} // namespace ape

#endif
