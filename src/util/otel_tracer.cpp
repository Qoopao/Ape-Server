#include "util/otel_tracer.h"

#include <opentelemetry/exporters/otlp/otlp_grpc_exporter.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_options.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/batch_span_processor.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/semconv/service_attributes.h>
#include <opentelemetry/trace/provider.h>

#include <memory>

namespace ape {
namespace otel {

namespace trace    = opentelemetry::trace;
namespace sdktrace = opentelemetry::sdk::trace;
namespace resource = opentelemetry::sdk::resource;
namespace otlp     = opentelemetry::exporter::otlp;

static bool initialized = false;

void InitTracer(const std::string &service_name)
{
    if (initialized) return;

    // ── 1. 配置 OTLP gRPC Exporter ──
    otlp::OtlpGrpcExporterOptions opts;
    opts.endpoint            = "localhost:4317"; // OTEL Collector
    opts.use_ssl_credentials = false;            // 内网不需要 TLS
    auto exporter = std::make_unique<otlp::OtlpGrpcExporter>(opts);

    // ── 2. 配置 BatchSpanProcessor ──
    sdktrace::BatchSpanProcessorOptions bsp_opts;
    bsp_opts.max_queue_size        = 2048;
    bsp_opts.schedule_delay_millis = std::chrono::milliseconds(5000);
    bsp_opts.max_export_batch_size = 512;
    auto processor = std::make_unique<sdktrace::BatchSpanProcessor>(
        std::move(exporter), bsp_opts);

    // ── 3. 配置 Resource (服务名会显示在 Jaeger) ──
    auto resource_attrs = resource::ResourceAttributes{
        {opentelemetry::semconv::service::kServiceName, service_name},
        {"service.namespace", "ape-im"},
    };
    auto res = resource::Resource::Create(resource_attrs);

    // ── 4. 创建 TracerProvider 并设为全局 ──
    auto g_provider = std::make_unique<sdktrace::TracerProvider>(
        std::move(processor), res);

    trace::Provider::SetTracerProvider(
        std::shared_ptr<trace::TracerProvider>(std::move(g_provider)));

    initialized = true;
}

opentelemetry::nostd::shared_ptr<trace::Tracer> GetTracer()
{
    auto *provider = dynamic_cast<sdktrace::TracerProvider *>(
        trace::Provider::GetTracerProvider().get());
    if (!provider)
        return {};
    return provider->GetTracer("ape-server", "1.0.0");
}

void CleanupTracer() {
    auto provider = trace::Provider::GetTracerProvider();
    if (provider) {
        auto* sdk_provider = dynamic_cast<sdktrace::TracerProvider*>(provider.get());
        if (sdk_provider) {
            sdk_provider->ForceFlush(std::chrono::seconds(5));
            sdk_provider->Shutdown(std::chrono::seconds(5));
        }
    }
}

} // namespace otel
} // namespace ape
