// 可观测基础设施单测：不依赖 collector / 中间件，只验证纯函数与 no-op 安全性。
#include "om/obs_log.h"
#include "om/obs_metrics.h"
#include "om/otel_config.h"
#include "om/trace_context.h"
#include "util/health_server.h"

#include <grpcpp/client_context.h>
#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <thread>

namespace {

int g_failures = 0;

void Check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++g_failures;
  }
}

#include "observability_health_test.inc"

constexpr const char *kTraceParent =
    "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

void TestTraceContext() {
  const auto trace = ape::otel::TraceContext::FromTraceParent(kTraceParent);
  Check(trace.valid(), "valid traceparent parses");
  Check(trace.traceId() == "4bf92f3577b34da6a3ce929d0e0e4736", "trace id");
  Check(trace.spanId() == "00f067aa0ba902b7", "span id");

  const auto context = ape::otel::ParseTraceParent(kTraceParent);
  Check(context.IsValid(), "parse to SpanContext");
  Check(ape::otel::EncodeTraceParent(context) == kTraceParent, "round-trip encode");

  Check(!ape::otel::TraceContext::FromTraceParent("").valid(), "empty invalid");
  Check(!ape::otel::TraceContext::FromTraceParent("00-zzzz").valid(), "short invalid");
  Check(!ape::otel::TraceContext::FromTraceParent(
             "00-00000000000000000000000000000000-00f067aa0ba902b7-01")
             .valid(),
        "zero trace id invalid");
  Check(ape::otel::EncodeTraceParent(
            opentelemetry::trace::SpanContext::GetInvalid())
            .empty(),
        "invalid span context encodes empty");

  std::string key = "traceparent";
  std::string value = kTraceParent;
  std::multimap<grpc::string_ref, grpc::string_ref> metadata;
  metadata.emplace(grpc::string_ref(key), grpc::string_ref(value));
  Check(ape::otel::TraceContext::FromMetadata(metadata).traceparent == kTraceParent,
        "metadata extraction");
}

void TestKafkaValue() {
  const auto trace = ape::otel::TraceContext::FromTraceParent(kTraceParent);
  const auto encoded = ape::otel::EncodeKafkaValue(trace, "server-msg-1");
  Check(encoded == std::string(kTraceParent) + "|server-msg-1", "encode kafka value");

  std::string traceparent, message_id;
  ape::otel::DecodeKafkaValue(encoded, traceparent, message_id);
  Check(traceparent == kTraceParent, "decode traceparent");
  Check(message_id == "server-msg-1", "decode message id");

  ape::otel::DecodeKafkaValue("server-msg-2", traceparent, message_id);
  Check(traceparent.empty(), "no traceparent");
  Check(message_id == "server-msg-2", "plain message id");

  ape::otel::DecodeKafkaValue("garbage|server-msg-3", traceparent, message_id);
  Check(traceparent.empty(), "invalid prefix rejected");
  Check(message_id == "garbage|server-msg-3", "invalid prefix kept as id");
}

void TestConfig() {
  unsetenv("OTEL_SERVICE_NAME");
  unsetenv("OTEL_EXPORTER_OTLP_ENDPOINT");
  unsetenv("OTEL_EXPORTER_OTLP_INSECURE");
  unsetenv("OTEL_SDK_DISABLED");
  unsetenv("SERVICE_INSTANCE_ID");
  unsetenv("APE_ENV");
  unsetenv("OTEL_TRACES_SAMPLER_ARG");

  const auto defaults = ape::otel::LoadOtelConfig("gateway");
  Check(defaults.service_name == "ape-gateway", "service name by role");
  Check(defaults.endpoint == "localhost:4317", "default endpoint");
  Check(defaults.insecure, "default insecure");
  Check(defaults.sample_ratio == 1.0, "default sample ratio");
  Check(ape::otel::ServiceNameForRole("") == "ape-server", "empty role fallback");

  setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "http://collector:4317/", 1);
  setenv("OTEL_SERVICE_NAME", "custom-name", 1);
  setenv("SERVICE_INSTANCE_ID", "gw-1", 1);
  setenv("OTEL_TRACES_SAMPLER_ARG", "0.25", 1);
  const auto overridden = ape::otel::LoadOtelConfig("msg");
  Check(overridden.endpoint == "collector:4317", "endpoint normalized");
  Check(overridden.service_name == "custom-name", "service name override");
  Check(overridden.service_instance_id == "gw-1", "instance id override");
  Check(overridden.sample_ratio == 0.25, "sample ratio override");
  Check(!overridden.disabled, "not disabled by default");

  setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "https://collector:4318", 1);
  const auto tls = ape::otel::LoadOtelConfig("all");
  Check(tls.endpoint == "collector:4318", "https endpoint normalized");
  Check(!tls.insecure, "https implies tls");

  setenv("OTEL_SDK_DISABLED", "true", 1);
  Check(ape::otel::LoadOtelConfig("all").disabled, "disabled flag");

  unsetenv("OTEL_SERVICE_NAME");
  unsetenv("OTEL_EXPORTER_OTLP_ENDPOINT");
  unsetenv("OTEL_EXPORTER_OTLP_INSECURE");
  unsetenv("OTEL_SDK_DISABLED");
  unsetenv("SERVICE_INSTANCE_ID");
  unsetenv("OTEL_TRACES_SAMPLER_ARG");
}

void TestStructuredLog() {
  const auto trace = ape::otel::TraceContext::FromTraceParent(kTraceParent);
  {
    ape::log::Event event(spdlog::level::info, "test.event");
    event.Field("account", static_cast<uint64_t>(42))
        .Field("ok", true)
        .Field("ms", 1.5)
        .Trace(trace);
    const auto *fields = ape::log::CurrentFields();
    Check(fields != nullptr && fields->size() == 3, "log fields captured");
    if (fields != nullptr && fields->size() == 3) {
      Check((*fields)[0].key == "account" && (*fields)[0].value == "42",
            "field account");
      Check((*fields)[1].value == "true", "field bool");
      Check((*fields)[2].value == "1.500", "field double");
    }
    Check(ape::log::CurrentTraceParent() == kTraceParent, "log trace parent");
  }
  Check(ape::log::CurrentFields() == nullptr, "log fields cleared");
}

void TestMetricsNoopSafety() {
  // InitMeter 之前：instrument 为空，只能 no-op，不能崩。
  ape::obs::RecordWsFrame(101, "ok");
  ape::obs::RecordSend("single", "ok", 1.0);
  ape::obs::SetOutboxBacklog(1, 3, 10);
  ape::obs::SetKafkaLag("msg_topic", 0, 5);
  ape::obs::RecordGroupFanout("inline", 3, 2, 1, 0, 4.0);

  ape::obs::RecordWsFrame(101, "ok");
  ape::obs::RecordOutboxAttempt(1, "error");
}

} // namespace

int main() {
  TestTraceContext();
  TestKafkaValue();
  TestConfig();
  TestStructuredLog();
  TestHealthServer();
  TestMetricsNoopSafety();

  if (g_failures != 0) {
    std::cerr << g_failures << " observability test(s) failed" << std::endl;
    return 1;
  }
  std::cout << "observability tests passed" << std::endl;
  return 0;
}
