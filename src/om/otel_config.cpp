#include "om/otel_config.h"

#include <opentelemetry/sdk/resource/resource.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace ape {
namespace otel {

namespace resource = opentelemetry::sdk::resource;

namespace {

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

const char *Env(const char *name)
{
    const char *value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? value : nullptr;
}

bool EnvBool(const char *name, bool fallback)
{
    const char *value = Env(name);
    if (value == nullptr)
        return fallback;
    const auto text = Lower(value);
    return text == "1" || text == "true" || text == "yes" || text == "on";
}

double EnvRatio(const char *name, double fallback)
{
    const char *value = Env(name);
    if (value == nullptr)
        return fallback;
    try {
        const double ratio = std::stod(value);
        if (ratio < 0.0 || ratio > 1.0)
            return fallback;
        return ratio;
    } catch (const std::exception &) {
        return fallback;
    }
}

// "http://collector:4317/" -> "collector:4317"；"collector" -> "collector:4317"。
std::string NormalizeEndpoint(std::string endpoint, bool &insecure)
{
    const auto scheme = endpoint.find("://");
    if (scheme != std::string::npos)
    {
        const auto prefix = Lower(endpoint.substr(0, scheme));
        if (prefix == "https")
            insecure = false;
        endpoint = endpoint.substr(scheme + 3);
    }
    const auto slash = endpoint.find('/');
    if (slash != std::string::npos)
        endpoint = endpoint.substr(0, slash);
    if (!endpoint.empty() && endpoint.find(':') == std::string::npos)
        endpoint += ":4317";
    return endpoint;
}

std::string DefaultInstanceId()
{
    char host[256]{};
    if (::gethostname(host, sizeof(host) - 1) != 0)
        std::strncpy(host, "unknown", sizeof(host) - 1);
    return std::string(host) + ":" + std::to_string(::getpid());
}

} // namespace

std::string ServiceNameForRole(const std::string &role)
{
    if (role.empty() || role == "unknown")
        return "ape-server";
    return "ape-" + role;
}

OtelConfig LoadOtelConfig(const std::string &role)
{
    OtelConfig config;
    config.disabled = EnvBool("OTEL_SDK_DISABLED", false);
    config.service_name = ServiceNameForRole(role);
    config.service_instance_id = Env("SERVICE_INSTANCE_ID") != nullptr
                                     ? Env("SERVICE_INSTANCE_ID")
                                     : DefaultInstanceId();
    config.environment = Env("APE_ENV") != nullptr ? Env("APE_ENV") : "dev";
    config.version = Env("APE_SERVICE_VERSION") != nullptr ? Env("APE_SERVICE_VERSION") : "1.0.0";
    config.insecure = EnvBool("OTEL_EXPORTER_OTLP_INSECURE", true);

    config.endpoint = Env("OTEL_EXPORTER_OTLP_ENDPOINT") != nullptr
                          ? Env("OTEL_EXPORTER_OTLP_ENDPOINT")
                          : "localhost:4317";
    config.endpoint = NormalizeEndpoint(config.endpoint, config.insecure);

    if (const char *name = Env("OTEL_SERVICE_NAME"))
        config.service_name = name;
    config.sample_ratio = EnvRatio("OTEL_TRACES_SAMPLER_ARG", 1.0);
    return config;
}

resource::Resource BuildResource(const OtelConfig &config)
{
    resource::ResourceAttributes attributes{
        {"service.name", config.service_name},
        {"service.namespace", "ape-im"},
        {"service.version", config.version},
        {"service.instance.id", config.service_instance_id},
        {"deployment.environment.name", config.environment},
        {"host.name", config.service_instance_id.substr(0, config.service_instance_id.find(':'))},
    };
    return resource::Resource::Create(attributes);
}

std::string DescribeOtelConfig(const OtelConfig &config)
{
    return "service.name=" + config.service_name +
           " service.instance.id=" + config.service_instance_id +
           " env=" + config.environment +
           " endpoint=" + config.endpoint +
           " insecure=" + (config.insecure ? "1" : "0") +
           " sample_ratio=" + std::to_string(config.sample_ratio) +
           " disabled=" + (config.disabled ? "1" : "0");
}

} // namespace otel
} // namespace ape
