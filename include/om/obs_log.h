#ifndef APE_OBS_LOG_H
#define APE_OBS_LOG_H

#include "om/trace_context.h"

#include <spdlog/common.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ape {
namespace log {

struct LogField
{
    std::string key;
    std::string value;
};

// 结构化事件：一次日志 = 一个 event 名 + 一组字段。
// 控制台输出 "event key=value ..."，同时通过 SpdlogOtelSink 变成 OTel LogRecord
// 的 attributes，Kibana 可按字段过滤。
//
// 用法：
//   APE_LOG_INFO("msg.send.accepted").Field("account", account).Trace(trace);
class Event
{
public:
    Event(spdlog::level::level_enum level, std::string event);
    ~Event();

    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;

    Event &Field(std::string_view key, std::string_view value);
    Event &Field(std::string_view key, const char *value);
    Event &Field(std::string_view key, int64_t value);
    Event &Field(std::string_view key, uint64_t value);
    Event &Field(std::string_view key, int value) { return Field(key, static_cast<int64_t>(value)); }
    Event &Field(std::string_view key, double value);
    Event &Field(std::string_view key, bool value);
    Event &Trace(const otel::TraceContext &trace);
    Event &Trace(std::string_view traceparent);

private:
    void Emit();

    spdlog::level::level_enum level_;
    std::string event_;
    std::vector<LogField> fields_;
    std::string traceparent_;
    bool emitted_{false};
};

// 以下访问器只在当前线程、当前 spdlog 调用栈内有效，供 SpdlogOtelSink 读取。
const std::vector<LogField> *CurrentFields();
const std::string &CurrentEvent();
const std::string &CurrentTraceParent();

} // namespace log
} // namespace ape

#define APE_LOG_TRACE(event) ::ape::log::Event(::spdlog::level::trace, (event))
#define APE_LOG_DEBUG(event) ::ape::log::Event(::spdlog::level::debug, (event))
#define APE_LOG_INFO(event) ::ape::log::Event(::spdlog::level::info, (event))
#define APE_LOG_WARN(event) ::ape::log::Event(::spdlog::level::warn, (event))
#define APE_LOG_ERROR(event) ::ape::log::Event(::spdlog::level::err, (event))

#endif
