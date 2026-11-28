#include "om/obs_log.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <utility>

namespace ape {
namespace log {

namespace {

struct ThreadState
{
    const std::vector<LogField> *fields = nullptr;
    std::string event;
    std::string traceparent;
};

ThreadState &Tls()
{
    thread_local ThreadState state;
    return state;
}

std::string FormatDouble(double value)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return std::string(buffer);
}

std::string QuoteIfNeeded(const std::string &value)
{
    const bool needs_quotes =
        value.empty() || value.find_first_of(" \t\"=\n") != std::string::npos;
    if (!needs_quotes)
        return value;
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    for (const char c : value)
    {
        if (c == '"' || c == '\\')
            quoted.push_back('\\');
        quoted.push_back(c);
    }
    quoted.push_back('"');
    return quoted;
}

} // namespace

Event::Event(spdlog::level::level_enum level, std::string event)
    : level_(level), event_(std::move(event))
{
    auto &state = Tls();
    state.fields = &fields_;
    state.event = event_;
    state.traceparent.clear();
}

Event::~Event()
{
    if (!emitted_)
        Emit();
    auto &state = Tls();
    state.fields = nullptr;
    state.event.clear();
    state.traceparent.clear();
}

Event &Event::Field(std::string_view key, std::string_view value)
{
    fields_.push_back(LogField{std::string(key), std::string(value)});
    return *this;
}

Event &Event::Field(std::string_view key, const char *value)
{
    return Field(key, std::string_view(value != nullptr ? value : ""));
}

Event &Event::Field(std::string_view key, int64_t value)
{
    const auto text = std::to_string(value);
    return Field(key, std::string_view(text));
}

Event &Event::Field(std::string_view key, uint64_t value)
{
    const auto text = std::to_string(value);
    return Field(key, std::string_view(text));
}

Event &Event::Field(std::string_view key, double value)
{
    const auto text = FormatDouble(value);
    return Field(key, std::string_view(text));
}

Event &Event::Field(std::string_view key, bool value)
{
    return Field(key, std::string_view(value ? "true" : "false"));
}

Event &Event::Trace(const otel::TraceContext &trace)
{
    return Trace(std::string_view(trace.traceparent));
}

Event &Event::Trace(std::string_view traceparent)
{
    traceparent_.assign(traceparent);
    Tls().traceparent = traceparent_;
    return *this;
}

void Event::Emit()
{
    emitted_ = true;
    std::string line;
    line.reserve(64 + fields_.size() * 24);
    line.append("[");
    line.append(event_);
    line.append("]");
    for (const auto &field : fields_)
    {
        line.push_back(' ');
        line.append(field.key);
        line.push_back('=');
        line.append(QuoteIfNeeded(field.value));
    }
    if (!traceparent_.empty() && traceparent_.size() >= 35)
    {
        line.append(" trace_id=");
        line.append(traceparent_.substr(3, 32));
    }
    // 让 sink 在同一次 spdlog 调用期间读到字段与 traceparent。
    auto &state = Tls();
    state.fields = &fields_;
    state.event = event_;
    state.traceparent = traceparent_;
    spdlog::log(level_, "{}", line);
}

const std::vector<LogField> *CurrentFields()
{
    return Tls().fields;
}

const std::string &CurrentEvent()
{
    return Tls().event;
}

const std::string &CurrentTraceParent()
{
    return Tls().traceparent;
}

} // namespace log
} // namespace ape
