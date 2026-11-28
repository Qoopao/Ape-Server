#ifndef OTEL_METRICS_H
#define OTEL_METRICS_H

#include "om/otel_config.h"

#include <opentelemetry/metrics/sync_instruments.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/unique_ptr.h>

#include <string>

namespace ape {
namespace otel {

// 初始化 MeterProvider + OTLP gRPC Metrics Exporter。
void InitMeter(const OtelConfig &config);

// 优雅关闭: flush 未发送的数据。
void CleanupMeter();

// ── 预定义 instrument（gRPC 服务端指标）──
opentelemetry::metrics::Counter<uint64_t> &RpcServerRequests();
opentelemetry::metrics::Histogram<double> &RpcServerDuration();
opentelemetry::metrics::UpDownCounter<int64_t> &WsConnectionsActive();

} // namespace otel
} // namespace ape

#endif
