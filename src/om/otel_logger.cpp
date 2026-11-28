#include "om/otel_logger.h"

#include "om/spdlog_otel_sink.h"

#include <opentelemetry/exporters/otlp/otlp_grpc_log_record_exporter.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_log_record_exporter_options.h>
#include <opentelemetry/logs/provider.h>
#include <opentelemetry/sdk/logs/logger_provider_factory.h>
#include <opentelemetry/sdk/logs/batch_log_record_processor.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace ape {
namespace otel {

namespace otlp = opentelemetry::exporter::otlp;
namespace logs = opentelemetry::logs;
namespace sdklogs = opentelemetry::sdk::logs;

namespace {

spdlog::level::level_enum LevelFromEnv()
{
    const char *value = std::getenv("SPDLOG_LEVEL");
    if (value == nullptr)
        return spdlog::level::info;
    const std::string level(value);
    if (level == "trace")
        return spdlog::level::trace;
    if (level == "debug")
        return spdlog::level::debug;
    if (level == "warn")
        return spdlog::level::warn;
    if (level == "error")
        return spdlog::level::err;
    return spdlog::level::info;
}

} // namespace

void InitLogger(const OtelConfig &config)
{
    if (config.disabled)
    {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("ape-server", console_sink);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
        logger->set_level(LevelFromEnv());
        spdlog::set_default_logger(logger);
        return;
    }

    otlp::OtlpGrpcLogRecordExporterOptions options;
    options.endpoint = config.endpoint;
    options.use_ssl_credentials = !config.insecure;
    auto exporter = std::make_unique<otlp::OtlpGrpcLogRecordExporter>(options);

    auto processor = std::make_unique<sdklogs::BatchLogRecordProcessor>(std::move(exporter));
    auto provider =
        sdklogs::LoggerProviderFactory::Create(std::move(processor), BuildResource(config));

    auto otel_logger = provider->GetLogger("ape-server", "", "1.0.0");
    logs::Provider::SetLoggerProvider(
        std::shared_ptr<logs::LoggerProvider>(std::move(provider)));

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto otel_sink = std::make_shared<SpdlogOtelSink>();
    otel_sink->SetOtelLogger(otel_logger);

    std::vector<spdlog::sink_ptr> sinks = {console_sink, otel_sink};
    auto logger = std::make_shared<spdlog::logger>("ape-server", sinks.begin(), sinks.end());
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
    logger->set_level(LevelFromEnv());
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
}

void CleanupLogger()
{
    spdlog::default_logger()->flush();
    auto provider = logs::Provider::GetLoggerProvider();
    if (!provider)
        return;
    if (auto *sdk_provider = dynamic_cast<sdklogs::LoggerProvider *>(provider.get()))
    {
        sdk_provider->ForceFlush(std::chrono::seconds(3));
        sdk_provider->Shutdown(std::chrono::seconds(3));
    }
}

} // namespace otel
} // namespace ape
