#include "util/otel_logger.h"
#include "util/spdlog_otel_sink.h"

#include <opentelemetry/exporters/otlp/otlp_grpc_log_record_exporter.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_log_record_exporter_options.h>
#include <opentelemetry/logs/provider.h>
#include <opentelemetry/sdk/logs/logger_provider_factory.h>
#include <opentelemetry/sdk/logs/batch_log_record_processor.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/semconv/service_attributes.h>

#include <memory>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <vector>

namespace ape {
namespace otel {

namespace otlp     = opentelemetry::exporter::otlp;
namespace resource = opentelemetry::sdk::resource;
namespace logs     = opentelemetry::logs;
namespace sdklogs  = opentelemetry::sdk::logs;

void InitLogger()
{
    // ── 1. OTLP gRPC Log Exporter ──
    otlp::OtlpGrpcLogRecordExporterOptions opts;
    opts.endpoint            = "localhost:4317";
    opts.use_ssl_credentials = false;
    auto exporter =
        std::make_unique<otlp::OtlpGrpcLogRecordExporter>(opts);

    // ── 2. BatchLogRecordProcessor (异步批量发送，避免阻塞) ──
    auto processor =
        std::make_unique<sdklogs::BatchLogRecordProcessor>(std::move(exporter));

    // ── 3. Resource ──
    auto resource_attrs = resource::ResourceAttributes{
        {opentelemetry::semconv::service::kServiceName, "ape-server"},
        {"service.namespace", "ape-im"},
    };
    auto res = resource::Resource::Create(resource_attrs);

    // ── 4. LoggerProvider ──
    auto provider = sdklogs::LoggerProviderFactory::Create(
        std::move(processor), res);

    auto otel_logger = provider->GetLogger("ape-server", "", "1.0.0");

    // 转移所有权到全局 Provider
    logs::Provider::SetLoggerProvider(
        std::shared_ptr<logs::LoggerProvider>(std::move(provider)));

    // ── 5. spdlog multi-sink: 终端 + OTel/ES ──
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto otel_sink     = std::make_shared<SpdlogOtelSink>();
    otel_sink->SetOtelLogger(otel_logger);

    std::vector<spdlog::sink_ptr> sinks = {console_sink, otel_sink};
    auto spdlog_logger = std::make_shared<spdlog::logger>("ape-server", sinks.begin(), sinks.end());
    spdlog::set_default_logger(spdlog_logger);
    spdlog::info("[Otel] Logger initialized — all spdlog messages now carry trace_id");
}

} // namespace otel
} // namespace ape
