#include "om/otel_metrics.h"

#include <opentelemetry/exporters/otlp/otlp_grpc_metric_exporter.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_options.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>

#include <chrono>
#include <memory>

namespace ape {
namespace otel {

namespace metrics = opentelemetry::metrics;
namespace sdkmetrics = opentelemetry::sdk::metrics;
namespace otlp = opentelemetry::exporter::otlp;

static bool initialized = false;

// 在 InitMeter 中创建，生命周期跟随 MeterProvider。
static opentelemetry::nostd::shared_ptr<metrics::Meter> g_meter;
static opentelemetry::nostd::unique_ptr<metrics::Counter<uint64_t>> g_rpc_requests;
static opentelemetry::nostd::unique_ptr<metrics::Histogram<double>> g_rpc_duration;
static opentelemetry::nostd::unique_ptr<metrics::UpDownCounter<int64_t>> g_ws_active;

void InitMeter(const OtelConfig &config)
{
    if (initialized)
        return;
    if (config.disabled)
    {
        // 关闭导出，但仍创建 no-op instrument，保证调用点不需要判空。
        g_meter = metrics::Provider::GetMeterProvider()->GetMeter("ape-server", "1.0.0");
        g_rpc_requests = g_meter->CreateUInt64Counter(
            "rpc.server.requests", "Total number of gRPC requests", "1");
        g_rpc_duration = g_meter->CreateDoubleHistogram(
            "rpc.server.duration", "gRPC request duration in milliseconds", "ms");
        g_ws_active = g_meter->CreateInt64UpDownCounter(
            "ape.ws.connections_active", "Number of active WebSocket connections", "1");
        initialized = true;
        return;
    }

    otlp::OtlpGrpcMetricExporterOptions options;
    options.endpoint = config.endpoint;
    options.use_ssl_credentials = !config.insecure;
    auto exporter = std::make_unique<otlp::OtlpGrpcMetricExporter>(options);

    sdkmetrics::PeriodicExportingMetricReaderOptions reader_options;
    reader_options.export_interval_millis = std::chrono::milliseconds(10000);
    reader_options.export_timeout_millis = std::chrono::milliseconds(5000);
    auto reader = std::make_unique<sdkmetrics::PeriodicExportingMetricReader>(
        std::move(exporter), reader_options);

    auto provider = std::make_unique<sdkmetrics::MeterProvider>(
        std::unique_ptr<sdkmetrics::ViewRegistry>(new sdkmetrics::ViewRegistry()),
        BuildResource(config));

    provider->AddMetricReader(std::move(reader));

    // 先取 Meter，再转移 provider 所有权。
    g_meter = provider->GetMeter("ape-server", "1.0.0");
    metrics::Provider::SetMeterProvider(
        std::shared_ptr<metrics::MeterProvider>(std::move(provider)));

    g_rpc_requests = g_meter->CreateUInt64Counter(
        "rpc.server.requests", "Total number of gRPC requests", "1");
    g_rpc_duration = g_meter->CreateDoubleHistogram(
        "rpc.server.duration", "gRPC request duration in milliseconds", "ms");
    g_ws_active = g_meter->CreateInt64UpDownCounter(
        "ape.ws.connections_active", "Number of active WebSocket connections", "1");

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
    auto provider = metrics::Provider::GetMeterProvider();
    if (!provider)
        return;
    if (auto *sdk_provider = dynamic_cast<sdkmetrics::MeterProvider *>(provider.get()))
    {
        sdk_provider->ForceFlush(std::chrono::seconds(3));
        sdk_provider->Shutdown(std::chrono::seconds(3));
    }
}

} // namespace otel
} // namespace ape
