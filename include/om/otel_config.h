#ifndef APE_OTEL_CONFIG_H
#define APE_OTEL_CONFIG_H

#include <opentelemetry/sdk/resource/resource.h>

#include <cstdint>
#include <string>

namespace ape {
namespace otel {

// 可观测配置：所有字段都可由环境变量覆盖，未设置时使用本地开发默认值。
// 约定（标准 OTEL_* 优先，APE_* 作为项目内覆盖）：
//   OTEL_SDK_DISABLED=true             关闭 trace/metric/log 导出（本地/CI 用）
//   OTEL_EXPORTER_OTLP_ENDPOINT        collector 地址，支持 http://host:4317 / host:4317
//   OTEL_EXPORTER_OTLP_INSECURE        "false" 时启用 TLS（默认 true）
//   OTEL_SERVICE_NAME                  覆盖 service.name
//   OTEL_TRACES_SAMPLER_ARG            0.0~1.0，采样比例（默认 1.0）
//   SERVICE_INSTANCE_ID                实例 ID，默认 hostname:pid
//   APE_ENV                            部署环境，默认 dev
//   APE_SERVICE_VERSION                版本号，默认 1.0.0
struct OtelConfig
{
    bool disabled = false;
    std::string service_name = "ape-server";
    std::string service_instance_id;
    std::string environment = "dev";
    std::string version = "1.0.0";
    std::string endpoint = "localhost:4317";
    bool insecure = true;
    double sample_ratio = 1.0;
};

// role 为空/unknown 时回落到 "ape-server"，否则为 "ape-<role>"。
std::string ServiceNameForRole(const std::string &role);

// 读取环境变量并归一化端点；不依赖任何全局状态。
OtelConfig LoadOtelConfig(const std::string &role);

// 生成统一的 Resource：service.name/instance/version/environment/host.name。
opentelemetry::sdk::resource::Resource BuildResource(const OtelConfig &config);

// 单行描述，用于启动日志（不含密钥）。
std::string DescribeOtelConfig(const OtelConfig &config);

} // namespace otel
} // namespace ape

#endif
