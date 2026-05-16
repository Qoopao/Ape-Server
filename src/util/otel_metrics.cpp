#include "util/otel_metrics.h"

#include <opentelemetry/exporters/otlp/otlp_grpc_metric_exporter.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_options.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/semconv/service_attributes.h>

#include <memory>

namespace ape {
namespace otel {

namespace metrics     = opentelemetry::metrics;
namespace sdkmetrics  = opentelemetry::sdk::metrics;
namespace resource    = opentelemetry::sdk::resource;
namespace otlp        = opentelemetry::exporter::otlp;

static bool initialized = false;

// Instruments (在 InitMeter 中创建，生命周期跟随 MeterProvider)
static opentelemetry::nostd::shared_ptr<metrics::Meter> g_meter;
static opentelemetry::nostd::unique_ptr<metrics::Counter<uint64_t>> g_rpc_requests;
static opentelemetry::nostd::unique_ptr<metrics::Histogram<double>> g_rpc_duration;
static opentelemetry::nostd::unique_ptr<metrics::UpDownCounter<int64_t>> g_ws_active;

void InitMeter(const std::string &service_name)
{
    if (initialized) return;

    // ── 1. OTLP gRPC Metrics Exporter ──
    otlp::OtlpGrpcMetricExporterOptions opts;
    opts.endpoint            = "localhost:4317";
    opts.use_ssl_credentials = false;
    auto exporter = std::make_unique<otlp::OtlpGrpcMetricExporter>(opts);

    // ── 2. PeriodicExportingMetricReader ──
    sdkmetrics::PeriodicExportingMetricReaderOptions reader_opts;
    reader_opts.export_interval_millis = std::chrono::milliseconds(10000);
    reader_opts.export_timeout_millis  = std::chrono::milliseconds(5000);
    auto reader = std::make_unique<sdkmetrics::PeriodicExportingMetricReader>(
        std::move(exporter), reader_opts);

    // ── 3. Resource ──
    auto resource_attrs = resource::ResourceAttributes{
        {opentelemetry::semconv::service::kServiceName, service_name},
        {"service.namespace", "ape-im"},
    };
    auto res = resource::Resource::Create(resource_attrs);

    // ── 4. MeterProvider — 转移所有权到全局 Provider ──
    auto provider = std::make_unique<sdkmetrics::MeterProvider>(
        std::unique_ptr<sdkmetrics::ViewRegistry>(new sdkmetrics::ViewRegistry()),
        res);

    provider->AddMetricReader(std::move(reader));

    // 获取 Meter（必须在 SetMeterProvider 前，因为需要 SDK provider 指针）
    g_meter = provider->GetMeter("ape-server", "1.0.0");

    // 转移所有权到全局 Provider（与 tracer 模式一致）
    metrics::Provider::SetMeterProvider(
        std::shared_ptr<metrics::MeterProvider>(std::move(provider)));

    // ── 5. 创建 Instruments ──
    g_rpc_requests = g_meter->CreateUInt64Counter(
        "rpc_server_requests_total",
        "Total number of gRPC requests",
        "1");

    g_rpc_duration = g_meter->CreateDoubleHistogram(
        "rpc_server_duration_ms",
        "gRPC request duration in milliseconds",
        "ms");

    g_ws_active = g_meter->CreateInt64UpDownCounter(
        "ws_connections_active",
        "Number of active WebSocket connections",
        "1");

    initialized = true;
}

metrics::Counter<uint64_t> &RpcServerRequests()
{
    return *g_rpc_requests;
}

metrics::Histogram<double> &RpcServerDuration()
{
    return *g_rpc_duration;
}

metrics::UpDownCounter<int64_t> &WsConnectionsActive()
{
    return *g_ws_active;
}

void CleanupMeter()
{
    // 通过全局 Provider 取回 SDK MeterProvider 进行清理
    auto mp = metrics::Provider::GetMeterProvider();
    if (mp)
    {
        auto *sdk_mp = dynamic_cast<sdkmetrics::MeterProvider *>(mp.get());
        if (sdk_mp)
        {
            sdk_mp->ForceFlush(std::chrono::seconds(5));
            sdk_mp->Shutdown(std::chrono::seconds(5));
        }
    }
}

} // namespace otel
} // namespace ape
