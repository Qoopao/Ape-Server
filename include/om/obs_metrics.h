#ifndef APE_OBS_METRICS_H
#define APE_OBS_METRICS_H

#include <cstdint>
#include <string_view>

namespace ape {
namespace obs {

// 业务/运行时指标门面。所有函数在 OTEL_SDK_DISABLED 或 InitMeter 未调用时
// 都是 no-op（底层 instrument 为空），调用点不需要判空。
//
// 标签基数约定：只允许有界维度（type/operation/result/topic/partition/mode）。
// account / conversation_id / server_msg_id / request_id 只进 trace/log。

// ── 网关 ──
void RecordWsFrame(int32_t type, std::string_view result);
void RecordWsRequestDuration(int32_t type, double milliseconds);
void RecordWsAuth(std::string_view result);
void RecordWsKick(std::string_view reason);
void RecordWsWriteDrop(std::string_view reason);
void RecordWsRouteRenewFailure();
// 写队列中的帧数：入队 +1，出队 -1（UpDownCounter，导出为 gauge）。
void AddWsWriteQueueFrames(int64_t delta);

// ── 消息 ──
void RecordSend(std::string_view kind, std::string_view result, double milliseconds);
void RecordOutboxAttempt(int operation, std::string_view result);
void RecordOutboxTombstone(int operation);
void SetOutboxBacklog(int operation, int64_t pending, int64_t oldest_age_seconds);

// ── Kafka ──
void RecordKafkaProduce(std::string_view topic, std::string_view result, double milliseconds);
void RecordKafkaConsume(std::string_view topic, std::string_view result);
void SetKafkaLag(std::string_view topic, int partition, int64_t lag);

// ── 推送 / 群扇出 ──
void RecordPush(std::string_view result);
void RecordAck(std::string_view kind, int64_t conversations);
void RecordGroupFanout(std::string_view mode, int64_t recipients, int64_t delivered,
                       int64_t not_online, int64_t unknown, double milliseconds);

} // namespace obs
} // namespace ape

#endif
