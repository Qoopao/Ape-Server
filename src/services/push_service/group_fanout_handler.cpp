#include "services/push_service/group_fanout_handler.h"
#include "groupchat.pb.h"
#include "om/obs_log.h"
#include "om/obs_metrics.h"
#include "om/trace_context.h"
#include "sdkws.pb.h"
#include "services/gateway_router.h"
#include "storage/mongohandler.h"
#include "storage/mysqlhandler.h"
#include "storage/redisconnector.h"
#include "storage/redishandler.h"

#include <algorithm>
#include <cstdlib>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace {

int64_t EnvInt(const char *name, int64_t fallback, int64_t minimum, int64_t maximum) {
  const char *value = std::getenv(name);
  if (!value || !*value)
    return fallback;
  try {
    return std::clamp<int64_t>(std::stoll(value), minimum, maximum);
  } catch (const std::exception &) {
    spdlog::warn("GroupFanout: invalid {}={}, using {}", name, value, fallback);
    return fallback;
  }
}

// 一次 eval 写入整组成员缓存并设置 TTL，避免 N 次 HSET 往返。
constexpr const char *kCacheScript =
    "local ttl=ARGV[1]; "
    "for i=2,#ARGV,2 do redis.call('HSET',KEYS[1],ARGV[i],ARGV[i+1]) end; "
    "redis.call('EXPIRE',KEYS[1],ttl); return 1";

boost::asio::awaitable<std::vector<MySQLHandler::GroupMemberJoinSeq>>
LoadMembers(const std::string &groupID) {
  const auto key = "grp:members:" + groupID;
  std::vector<MySQLHandler::GroupMemberJoinSeq> members;
  const bool cacheEnabled = RedisConnector::instance().pool_size() > 0;
  try {
    const auto cached = cacheEnabled
        ? co_await RedisConnector::instance().hgetall(key)
        : std::unordered_map<std::string, std::string>{};
    if (!cached.empty()) {
      members.reserve(cached.size());
      for (const auto &[account, joinSeq] : cached) {
        try {
          members.push_back({std::stoull(account), std::stoull(joinSeq)});
        } catch (const std::exception &) {
          // 脏缓存字段跳过；缺人会由 FAN-008/245 兜底。
        }
      }
      if (!members.empty())
        co_return members;
    }
  } catch (const std::exception &e) {
    spdlog::warn("GroupFanout: member cache read failed for group {}: {}", groupID, e.what());
  }

  // 回源 SQL：active=1 的权威成员。
  members = co_await MySQLHandler::GroupMembersWithJoinSeq(groupID);
  if (members.empty())
    co_return members;

  try {
    if (!cacheEnabled)
      co_return members;
    const auto ttl = EnvInt("GROUP_MEMBERS_CACHE_TTL", 600, 1, 86400);
    std::vector<std::string> args{std::to_string(ttl)};
    args.reserve(1 + members.size() * 2);
    for (const auto &member : members) {
      args.push_back(std::to_string(member.account));
      args.push_back(std::to_string(member.joinSeq));
    }
    co_await RedisConnector::instance().eval(kCacheScript, {key}, args);
  } catch (const std::exception &e) {
    spdlog::warn("GroupFanout: member cache fill failed for group {}: {}", groupID, e.what());
  }
  co_return members;
}

} // namespace

GroupFanoutHandler::GroupFanoutHandler(GatewayRouter *router) : router_(router) {}

boost::asio::awaitable<void> GroupFanoutHandler::handle(const std::string topic,
                                                        const std::string message) {
  if (router_ == nullptr)
    co_return;

  // payload: "traceparent|serverMsgID"（与单聊 msg_topic 一致）
  std::string traceparent;
  std::string serverMsgID;
  ape::otel::DecodeKafkaValue(message, traceparent, serverMsgID);
  if (serverMsgID.empty())
    co_return;

  const auto started = std::chrono::steady_clock::now();
  auto span = ape::otel::StartChildSpan(
      ape::otel::TraceContext::FromTraceParent(traceparent),
      "Kafka Consume " + topic,
      {{"messaging.system", "kafka"},
       {"messaging.destination", topic},
       {"im.server_msg_id", serverMsgID}},
      opentelemetry::trace::SpanKind::kConsumer);

  // 1) 消息正文：先 Redis 缓存，再 Mongo（Kafka 只带引用）。
  //    损坏事件/缓存异常只跳过并告警，不能让 Kafka 分区卡在同一 offset。
  std::optional<sdkws::MsgData> found;
  try {
    found = co_await RedisHandler::GetMsg(serverMsgID);
    if (!found.has_value())
      found = co_await MongoHandler::GetMsgByServerMsgIDAsync(serverMsgID);
  } catch (const std::exception &e) {
    spdlog::error("GroupFanout: load message {} failed, skipping: {}", serverMsgID, e.what());
    co_return;
  }
  if (!found.has_value()) {
    spdlog::info("GroupFanout: message {} no longer exists, skipping", serverMsgID);
    co_return;
  }
  const auto msg = *found;
  const auto groupID = msg.convid();
  if (groupID.empty())
    co_return;

  // 2) stream：解散则不再推送；published_seq 作为 hint 的 lastSeq。
  const auto state = co_await MySQLHandler::GroupFanoutState(groupID);
  if (!state.found || state.dismissed) {
    spdlog::debug("GroupFanout: group {} missing or dismissed, skip seq={}", groupID, msg.seq());
    co_return;
  }

  // 3) 成员：缓存 account→joinSeq；miss 回源 SQL 并回填。
  const auto members = co_await LoadMembers(groupID);
  if (members.empty())
    co_return;

  // 4) 收件人：可见下界 joinSeq < seq，发送者本人不推。
  std::vector<uint64_t> recipients;
  recipients.reserve(members.size());
  for (const auto &member : members) {
    if (member.account == msg.sendid())
      continue;
    if (msg.seq() > 0 && static_cast<int64_t>(member.joinSeq) >= msg.seq())
      continue;
    recipients.push_back(member.account);
  }
  if (recipients.empty())
    co_return;

  // 5) 大小群判定：active 成员数（排除 sender 前）与单条字节双阈值。
  const auto inlineMembers = EnvInt("GROUP_INLINE_MAX_MEMBERS", 200, 1, 1000000);
  const auto inlineBytes = EnvInt("GROUP_INLINE_MAX_BYTES", 262144, 1024, 16 * 1024 * 1024);
  groupchat::Notice notice;
  notice.set_groupid(groupID);
  notice.set_lastseq(state.publishedSeq);
  notice.set_epoch(state.epoch);
  notice.set_refresh(false);
  notice.set_reason(0);
  notice.set_lastmsgtime(static_cast<uint64_t>(msg.sendtime()));
  const bool inlineBody = static_cast<int64_t>(members.size()) <= inlineMembers &&
      static_cast<int64_t>(msg.ByteSizeLong()) <= inlineBytes;
  if (inlineBody)
    *notice.add_messages() = msg;

  const auto payload = notice.SerializeAsString();
  const auto chunk = static_cast<size_t>(EnvInt("GROUP_FANOUT_CHUNK", 500, 1, 10000));
  size_t delivered = 0, notOnline = 0, unknown = 0;
  for (size_t offset = 0; offset < recipients.size(); offset += chunk) {
    const auto end = std::min(recipients.size(), offset + chunk);
    std::vector<uint64_t> batch(recipients.begin() + offset, recipients.begin() + end);
    const auto results = co_await router_->PushBatchBestEffort(
        batch, 251, payload, span.context());
    for (const auto result : results) {
      switch (result) {
        case PushResult::Delivered: ++delivered; break;
        case PushResult::NotOnline: ++notOnline; break;
        case PushResult::Unknown: ++unknown; break;
      }
    }
  }
  const auto elapsed_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - started).count();
  ape::obs::RecordGroupFanout(inlineBody ? "inline" : "hint",
                              static_cast<int64_t>(recipients.size()),
                              static_cast<int64_t>(delivered),
                              static_cast<int64_t>(notOnline),
                              static_cast<int64_t>(unknown), elapsed_ms);
  APE_LOG_INFO("group.fanout")
      .Field("group_id", groupID)
      .Field("seq", msg.seq())
      .Field("mode", inlineBody ? "inline" : "hint")
      .Field("members", members.size())
      .Field("recipients", recipients.size())
      .Field("delivered", delivered)
      .Field("not_online", notOnline)
      .Field("unknown", unknown)
      .Trace(span.context());
  span.EndOk();
}
