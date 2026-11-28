// 单测专用 stub：可观测核心不链接完整 OTel SDK/exporter，避免静态库循环。
#include "om/otel_tracer.h"

namespace ape {
namespace otel {

opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> GetTracer()
{
    return {};
}

} // namespace otel
} // namespace ape
