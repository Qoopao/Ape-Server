#include "om/otel_tracer.h"

#include <opentelemetry/exporters/otlp/otlp_grpc_exporter.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_options.h>
#include <opentelemetry/sdk/trace/batch_span_processor.h>
#include <opentelemetry/sdk/trace/sampler.h>
#include <opentelemetry/sdk/trace/samplers/trace_id_ratio.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/trace/provider.h>

#include <chrono>
#include <memory>

namespace ape {
namespace otel {

namespace trace = opentelemetry::trace;
namespace sdktrace = opentelemetry::sdk::trace;
namespace otlp = opentelemetry::exporter::otlp;

// 不加这个标记时，两次 Init 会让旧 TracerProvider 被动销毁，可能不走 Shutdown。
static bool initialized = false;

void InitTracer(const OtelConfig &config)
{
    if (initialized || config.disabled)
        return;

    otlp::OtlpGrpcExporterOptions options;
    options.endpoint = config.endpoint;
    options.use_ssl_credentials = !config.insecure;
    auto exporter = std::make_unique<otlp::OtlpGrpcExporter>(options);

    sdktrace::BatchSpanProcessorOptions processor_options;
    processor_options.max_queue_size = 2048;
    processor_options.schedule_delay_millis = std::chrono::milliseconds(5000);
    processor_options.max_export_batch_size = 512;
    auto processor =
        std::make_unique<sdktrace::BatchSpanProcessor>(std::move(exporter), processor_options);

    std::unique_ptr<sdktrace::Sampler> sampler;
    if (config.sample_ratio < 1.0)
        sampler = std::make_unique<sdktrace::TraceIdRatioBasedSampler>(config.sample_ratio);
    else
        sampler = std::make_unique<sdktrace::AlwaysOnSampler>();

    auto provider = std::make_unique<sdktrace::TracerProvider>(
        std::move(processor), BuildResource(config), std::move(sampler));

    trace::Provider::SetTracerProvider(
        std::shared_ptr<trace::TracerProvider>(std::move(provider)));
    initialized = true;
}

opentelemetry::nostd::shared_ptr<trace::Tracer> GetTracer()
{
    auto *provider = dynamic_cast<sdktrace::TracerProvider *>(
        trace::Provider::GetTracerProvider().get());
    if (provider == nullptr)
        return {};
    return provider->GetTracer("ape-server", "1.0.0");
}

void CleanupTracer()
{
    auto provider = trace::Provider::GetTracerProvider();
    if (!provider)
        return;
    if (auto *sdk_provider = dynamic_cast<sdktrace::TracerProvider *>(provider.get()))
    {
        sdk_provider->ForceFlush(std::chrono::seconds(3));
        sdk_provider->Shutdown(std::chrono::seconds(3));
    }
}

} // namespace otel
} // namespace ape
