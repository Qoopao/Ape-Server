#include "om/obs_metrics.h"

#include <opentelemetry/common/key_value_iterable_view.h>
#include <opentelemetry/metrics/meter.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/metrics/sync_instruments.h>

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ape {
namespace obs {

namespace metrics = opentelemetry::metrics;

namespace {

using Attributes = std::vector<std::pair<std::string, std::string>>;

metrics::Meter *Meter()
{
    // InitMeter 之前拿到的是 no-op provider，instrument 也是 no-op，不会崩溃。
    static auto meter =
        metrics::Provider::GetMeterProvider()->GetMeter("ape-server", "1.0.0");
    return meter.get();
}

// 每个 instrument 一个函数，保证只创建一次，且生命周期覆盖进程。
#define APE_DEFINE_COUNTER(fn, name, description)                                          \
    metrics::Counter<uint64_t> *fn()                                                       \
    {                                                                                      \
        static auto instrument = Meter()->CreateUInt64Counter((name), (description), "1"); \
        return instrument.get();                                                           \
    }

#define APE_DEFINE_HISTOGRAM(fn, name, description, unit)                                      \
    metrics::Histogram<double> *fn()                                                           \
    {                                                                                          \
        static auto instrument =                                                               \
            Meter()->CreateDoubleHistogram((name), (description), (unit));                     \
        return instrument.get();                                                               \
    }

// ABI v1 没有同步 Gauge，用 UpDownCounter + 增量维护当前值。
#define APE_DEFINE_UPDOWN(fn, name, description)                                              \
    metrics::UpDownCounter<int64_t> *fn()                                                     \
    {                                                                                         \
        static auto instrument =                                                              \
            Meter()->CreateInt64UpDownCounter((name), (description), "1");                    \
        return instrument.get();                                                              \
    }

APE_DEFINE_COUNTER(WsFrames, "ape.ws.frames", "WebSocket 入站帧数")
APE_DEFINE_HISTOGRAM(WsRequestDuration, "ape.ws.request_duration", "WebSocket 请求处理耗时", "ms")
APE_DEFINE_COUNTER(WsAuth, "ape.ws.auth", "认证/注册结果")
APE_DEFINE_COUNTER(WsKicks, "ape.ws.kicks", "连接被踢次数")
APE_DEFINE_COUNTER(WsWriteDrops, "ape.ws.write_drops", "写队列丢弃/关闭次数")
APE_DEFINE_COUNTER(WsRouteRenewFailures, "ape.ws.route_renew_failures", "路由 CAS 续期失败次数")
APE_DEFINE_UPDOWN(WsWriteQueueFrames, "ape.ws.write_queue_frames", "写队列中待发送帧数")

APE_DEFINE_COUNTER(SendTotal, "ape.im.send", "发送受理结果")
APE_DEFINE_HISTOGRAM(SendDuration, "ape.im.send_duration", "发送受理耗时", "ms")
APE_DEFINE_COUNTER(OutboxAttempts, "ape.outbox.attempts", "outbox 处理尝试")
APE_DEFINE_COUNTER(OutboxTombstones, "ape.outbox.tombstones", "outbox 放弃后写 tombstone 的次数")
APE_DEFINE_UPDOWN(OutboxPending, "ape.outbox.pending", "未完成 outbox 行数")
APE_DEFINE_UPDOWN(OutboxOldestAge, "ape.outbox.oldest_age", "最老未完成 outbox 年龄")

APE_DEFINE_COUNTER(KafkaProduce, "ape.kafka.produce", "Kafka 生产结果")
APE_DEFINE_HISTOGRAM(KafkaProduceDuration, "ape.kafka.produce_duration", "Kafka 生产确认耗时", "ms")
APE_DEFINE_COUNTER(KafkaConsume, "ape.kafka.consume", "Kafka 消费结果")
APE_DEFINE_UPDOWN(KafkaLag, "ape.kafka.lag", "Kafka 消费 lag")

APE_DEFINE_COUNTER(Push, "ape.push.deliveries", "在线推送三态")
APE_DEFINE_COUNTER(Acks, "ape.push.acks", "ACK 处理")
APE_DEFINE_COUNTER(GroupFanoutRecipients, "ape.group.fanout.recipients", "群扇出收件人结果")
APE_DEFINE_HISTOGRAM(GroupFanoutDuration, "ape.group.fanout.duration", "群扇出耗时", "ms")

void AddN(metrics::Counter<uint64_t> *counter, uint64_t value, const Attributes &attributes)
{
    if (counter == nullptr || value == 0)
        return;
    counter->Add(value, opentelemetry::common::KeyValueIterableView<Attributes>{attributes},
                 opentelemetry::context::Context{});
}

void Add(metrics::Counter<uint64_t> *counter, const Attributes &attributes)
{
    AddN(counter, 1, attributes);
}

void Observe(metrics::Histogram<double> *histogram, double value, const Attributes &attributes)
{
    if (histogram == nullptr)
        return;
    histogram->Record(value, opentelemetry::common::KeyValueIterableView<Attributes>{attributes},
                      opentelemetry::context::Context{});
}

void Adjust(metrics::UpDownCounter<int64_t> *counter, int64_t delta,
            const Attributes &attributes)
{
    if (counter == nullptr || delta == 0)
        return;
    counter->Add(delta, opentelemetry::common::KeyValueIterableView<Attributes>{attributes},
                 opentelemetry::context::Context{});
}

// 把「采样值」转成 UpDownCounter 的增量；按标签组合记住上一次的值。
struct GaugeState
{
    std::mutex mutex;
    std::map<std::string, int64_t> last;
};

int64_t DeltaFor(GaugeState &state, const std::string &key, int64_t value)
{
    std::lock_guard lock(state.mutex);
    const auto it = state.last.find(key);
    const int64_t previous = it == state.last.end() ? 0 : it->second;
    state.last[key] = value;
    return value - previous;
}

} // namespace

void RecordWsFrame(int32_t type, std::string_view result)
{
    Add(WsFrames(), {{"direction", "in"},
                     {"type", std::to_string(type)},
                     {"result", std::string(result)}});
}

void RecordWsRequestDuration(int32_t type, double milliseconds)
{
    Observe(WsRequestDuration(), milliseconds, {{"type", std::to_string(type)}});
}

void RecordWsAuth(std::string_view result)
{
    Add(WsAuth(), {{"result", std::string(result)}});
}

void RecordWsKick(std::string_view reason)
{
    Add(WsKicks(), {{"reason", std::string(reason)}});
}

void RecordWsWriteDrop(std::string_view reason)
{
    Add(WsWriteDrops(), {{"reason", std::string(reason)}});
}

void RecordWsRouteRenewFailure()
{
    Add(WsRouteRenewFailures(), {});
}

void AddWsWriteQueueFrames(int64_t delta)
{
    Adjust(WsWriteQueueFrames(), delta, {});
}

void RecordSend(std::string_view kind, std::string_view result, double milliseconds)
{
    Add(SendTotal(), {{"kind", std::string(kind)}, {"result", std::string(result)}});
    Observe(SendDuration(), milliseconds, {{"kind", std::string(kind)}});
}

void RecordOutboxAttempt(int operation, std::string_view result)
{
    Add(OutboxAttempts(),
        {{"operation", std::to_string(operation)}, {"result", std::string(result)}});
}

void RecordOutboxTombstone(int operation)
{
    Add(OutboxTombstones(), {{"operation", std::to_string(operation)}});
}

void SetOutboxBacklog(int operation, int64_t pending, int64_t oldest_age_seconds)
{
    static GaugeState pending_state;
    static GaugeState age_state;
    const std::string key = std::to_string(operation);
    const Attributes attributes{{"operation", key}};
    Adjust(OutboxPending(), DeltaFor(pending_state, key, pending), attributes);
    Adjust(OutboxOldestAge(), DeltaFor(age_state, key, oldest_age_seconds), attributes);
}

void RecordKafkaProduce(std::string_view topic, std::string_view result, double milliseconds)
{
    Add(KafkaProduce(), {{"topic", std::string(topic)}, {"result", std::string(result)}});
    Observe(KafkaProduceDuration(), milliseconds, {{"topic", std::string(topic)}});
}

void RecordKafkaConsume(std::string_view topic, std::string_view result)
{
    Add(KafkaConsume(), {{"topic", std::string(topic)}, {"result", std::string(result)}});
}

void SetKafkaLag(std::string_view topic, int partition, int64_t lag)
{
    static GaugeState state;
    const std::string key = std::string(topic) + ":" + std::to_string(partition);
    Adjust(KafkaLag(), DeltaFor(state, key, lag),
           {{"topic", std::string(topic)}, {"partition", std::to_string(partition)}});
}

void RecordPush(std::string_view result)
{
    Add(Push(), {{"result", std::string(result)}});
}

void RecordAck(std::string_view kind, int64_t conversations)
{
    AddN(Acks(), static_cast<uint64_t>(conversations > 0 ? conversations : 1),
         {{"kind", std::string(kind)}});
}

void RecordGroupFanout(std::string_view mode, int64_t recipients, int64_t delivered,
                       int64_t not_online, int64_t unknown, double milliseconds)
{
    const std::string mode_text(mode);
    AddN(GroupFanoutRecipients(), static_cast<uint64_t>(delivered),
         {{"mode", mode_text}, {"result", "delivered"}});
    AddN(GroupFanoutRecipients(), static_cast<uint64_t>(not_online),
         {{"mode", mode_text}, {"result", "not_online"}});
    AddN(GroupFanoutRecipients(), static_cast<uint64_t>(unknown),
         {{"mode", mode_text}, {"result", "unknown"}});
    Observe(GroupFanoutDuration(), milliseconds, {{"mode", mode_text}});
    (void)recipients;
}

} // namespace obs
} // namespace ape
