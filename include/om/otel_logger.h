#ifndef OTEL_LOGGER_H
#define OTEL_LOGGER_H

#include "om/otel_config.h"

namespace ape {
namespace otel {

// 初始化 OTel Logs（OTLP gRPC exporter）+ spdlog 双 sink（控制台 + OTel）。
void InitLogger(const OtelConfig &config);

// 关闭前 flush：否则 BatchLogRecordProcessor 里最后几秒的日志会丢。
void CleanupLogger();

} // namespace otel
} // namespace ape

#endif
