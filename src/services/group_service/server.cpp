#include "services/group_service/server.h"
#include "om/obs_metrics.h"
#include "om/trace_context.h"
#include "services/group_service/group_outbox_worker.h"
#include "storage/mysqlconnector.h"
#include "storage/mysqlhandler.h"
#include "storage/mongohandler.h"
#include "storage/redisconnector.h"

#include <algorithm>
#include <cstdlib>
#include <spdlog/spdlog.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace {

std::string GatewayFallbackAddress() {
  const char *value = std::getenv("GATEWAY_PUSH_ADDR");
  return value ? std::string(value) : std::string("localhost:50055");
}

int64_t EnvInt(const char *name, int64_t fallback, int64_t minimum, int64_t maximum) {
  const char *value = std::getenv(name);
  if (!value || !*value)
    return fallback;
  try {
    return std::clamp<int64_t>(std::stoll(value), minimum, maximum);
  } catch (const std::exception &) {
    spdlog::warn("GroupService: invalid {}={}, using {}", name, value, fallback);
    return fallback;
  }
}

// 固定窗口 1s 计数；Redis 故障时放行，由群级上限兜底（README §5.2）。
constexpr const char *kRateScript =
    "local n=redis.call('INCR',KEYS[1]); "
    "if n==1 then redis.call('PEXPIRE',KEYS[1],1000) end; "
    "return n";

boost::asio::awaitable<bool> AllowSend(const std::string &groupID, uint64_t account) {
  if (groupID.empty() || !account)
    co_return true;
  // Redis 未启动（单测/降级）：放行，由群级上限与存储兜底。
  if (RedisConnector::instance().pool_size() <= 0)
    co_return true;
  const auto memberLimit = EnvInt("GROUP_SEND_RATE_MEMBER", 5, 1, 1000000);
  const auto groupLimit = EnvInt("GROUP_SEND_RATE_GROUP", 50, 1, 1000000);
  try {
    auto &redis = RedisConnector::instance();
    const auto member = co_await redis.eval(kRateScript,
        {"grp:rate:m:" + groupID + ":" + std::to_string(account)}, {});
    if (member > memberLimit)
      co_return false;
    const auto group = co_await redis.eval(kRateScript, {"grp:rate:g:" + groupID}, {});
    if (group > groupLimit)
      co_return false;
  } catch (const std::exception &e) {
    spdlog::warn("GroupService: rate limit unavailable for group {}: {}", groupID, e.what());
  }
  co_return true;
}

void FinishGroupError(grpc::ServerUnaryReactor *reactor, const std::string &code,
                      const std::string &message = {}) {
  // 245 响应没有 errorCode 字段：外层 SdkWSResp.errorCode 由网关从 gRPC status
  // 的 message 取回，因此这里把业务错误码原样放在 message 里。
  reactor->Finish(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                               message.empty() ? code : code + "|" + message));
}

} // namespace

GroupServiceImpl::GroupServiceImpl(const std::string &service_name,
                                   const std::string &listen_address)
    : BaseServiceServer<GroupServiceImpl>(service_name, listen_address),
      router_([this]() { return GetBackbonClient(); }, GatewayFallbackAddress()) {}

GroupServiceImpl::~GroupServiceImpl() { StopOutbox(); Stop(); }

void GroupServiceImpl::SetProducer(IMessageProducer *producer) { producer_ = producer; }

void GroupServiceImpl::StartOutbox() {
  if (outbox_ || producer_ == nullptr)
    return;
  outbox_ = std::make_unique<GroupOutboxWorker>(*producer_);
  outbox_->Start();
}

void GroupServiceImpl::StopOutbox() {
  if (!outbox_)
    return;
  outbox_->Stop();
  outbox_.reset();
}

grpc::ServerUnaryReactor* GroupServiceImpl::Execute(grpc::CallbackServerContext* context,
    const groupchat::Command* request, groupchat::Reply* response) {
    auto* reactor = context->DefaultReactor();
    boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        [this, reactor, response, command = *request]() -> boost::asio::awaitable<void> {
            try {
                *response = co_await MySQLHandler::ExecuteGroupCommand(command);
                const auto operation = static_cast<int>(command.operation());
                if (response->errorcode() == "0") {
                    // 成员变更后成员缓存立即失效（TTL 只是兜底）。
                    if (operation == groupchat::Create || operation == groupchat::Review ||
                        operation == groupchat::Leave || operation == groupchat::Kick ||
                        operation == groupchat::Dismiss) {
                        if (RedisConnector::instance().pool_size() > 0) {
                            try {
                                co_await RedisConnector::instance().del(
                                    "grp:members:" + response->groupid());
                            } catch (const std::exception &e) {
                                spdlog::warn("GroupService: invalidate member cache failed: {}", e.what());
                            }
                        }
                    }
                    // 成员/元数据变化只推 refresh；消息正文由 251 新消息链路负责。
                    if (operation == groupchat::Apply || operation == groupchat::Review ||
                        operation == groupchat::Rename || operation == groupchat::Leave ||
                        operation == groupchat::Kick || operation == groupchat::Dismiss)
                        co_await Notify(command, *response);
                }
                reactor->Finish(grpc::Status::OK);
            } catch (const std::exception& e) {
                spdlog::error("GroupService.Execute: {}", e.what());
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "DB_ERROR"));
            }
        }, boost::asio::detached);
    return reactor;
}

boost::asio::awaitable<void> GroupServiceImpl::Notify(const groupchat::Command &command,
                                                      const groupchat::Reply &reply) {
    if (reply.groupid().empty())
        co_return;
    const auto operation = static_cast<int>(command.operation());
    const int reason = operation == groupchat::Dismiss ? 2 : 1;
    try {
        groupchat::Notice notice;
        notice.set_groupid(reply.groupid());
        notice.set_refresh(true);
        notice.set_reason(reason);
        notice.set_epoch(reply.epoch());

        const auto members = co_await MySQLHandler::GroupMembersWithJoinSeq(reply.groupid());
        std::vector<uint64_t> accounts;
        accounts.reserve(members.size());
        for (const auto &member : members) {
            // 被踢者已 active=0，不在这个列表里；剩余成员收到普通刷新。
            if (operation == groupchat::Leave && member.account == command.actor())
                continue;
            accounts.push_back(member.account);
        }
        if (!accounts.empty()) {
            const auto results = co_await router_.PushBatchBestEffort(
                accounts, 251, notice.SerializeAsString());
            for (size_t i = 0; i < results.size() && i < accounts.size(); ++i) {
                if (results[i] != PushResult::Delivered)
                    spdlog::debug("GroupService: refresh notice {} for account {}",
                                  results[i] == PushResult::NotOnline ? "not online" : "unknown",
                                  accounts[i]);
            }
        }

        if (operation == groupchat::Kick && command.target()) {
            // 被踢者已不在 active 列表：单独尽力通知 reason=3。
            groupchat::Notice kicked;
            kicked.set_groupid(reply.groupid());
            kicked.set_refresh(true);
            kicked.set_reason(3);
            kicked.set_epoch(reply.epoch());
            co_await router_.PushBatchBestEffort(
                {command.target()}, 251, kicked.SerializeAsString());
        }
    } catch (const std::exception &e) {
        // 通知失败不影响已提交的成员变更；客户端登录同步会补齐状态。
        spdlog::warn("GroupService refresh notice failed: {}", e.what());
    }
}

grpc::ServerUnaryReactor* GroupServiceImpl::SendGroupMessages(
    grpc::CallbackServerContext* context, const groupchat::SendGroupMessageReq* request,
    groupchat::SendGroupMessageResp* response) {
    auto* reactor = context->DefaultReactor();
    const auto trace = ape::otel::TraceContext::FromMetadata(context->client_metadata());
    auto span = ape::otel::StartChildSpan(
        trace, "GroupService/SendGroupMessages",
        {{"rpc.system", "grpc"}, {"im.group_id", request->groupid()}},
        opentelemetry::trace::SpanKind::kServer);
    const auto span_trace = span.context();
    boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        [reactor, response, command = *request, span = std::move(span),
         span_trace]() mutable -> boost::asio::awaitable<void> {
            try {
                groupchat::SendGroupMessageResp result;
                if (!co_await AllowSend(command.groupid(), command.actor())) {
                    result.set_errorcode("RATE_LIMITED");
                    result.set_errormsg("发送过于频繁，请稍后重试");
                } else {
                    auto send = co_await MySQLHandler::SendGroupMessage(
                        command, span_trace.traceparent);
                    result.set_errorcode(send.errorCode);
                    result.set_errormsg(send.errorMessage);
                    if (send.errorCode == "0")
                        *result.mutable_msg() = send.msg;
                }
                *response = std::move(result);
                span.EndOk();
                reactor->Finish(grpc::Status::OK);
            } catch (const std::exception &e) {
                span.EndError(e.what());
                spdlog::error("GroupService.SendGroupMessages: {}", e.what());
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "DB_ERROR"));
            }
        }, boost::asio::detached);
    return reactor;
}

grpc::ServerUnaryReactor* GroupServiceImpl::GetGroupChatHistory(
    grpc::CallbackServerContext* context, const groupchat::GetGroupChatHistoryReq* request,
    groupchat::GetGroupChatHistoryResp* response) {
    auto* reactor = context->DefaultReactor();
    boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        [reactor, response, query = *request]() -> boost::asio::awaitable<void> {
            try {
                if (!query.owneruserid() || query.groupid().empty()) {
                    FinishGroupError(reactor, "INVALID_PARAM");
                    co_return;
                }
                if (query.limit() < 0) {
                    FinishGroupError(reactor, "INVALID_PARAM");
                    co_return;
                }
                if (query.afterseq() > 0 && query.beforeseq() > 0) {
                    FinishGroupError(reactor, "INVALID_PARAM");
                    co_return;
                }
                const auto defaultLimit = EnvInt("GROUP_HISTORY_DEFAULT_LIMIT", 20, 1, 100);
                const auto maxLimit = EnvInt("GROUP_HISTORY_MAX_LIMIT", 100, 1, 1000);
                const auto limit = static_cast<int>(std::clamp<int64_t>(
                    query.limit() == 0 ? defaultLimit : query.limit(), 1, maxLimit));

                auto access = co_await MySQLHandler::GroupAccess(query.groupid(), query.owneruserid());
                if (access.errorCode != "0") {
                    FinishGroupError(reactor, access.errorCode, access.errorMessage);
                    co_return;
                }
                const auto &group = access.group;
                const int64_t joinSeq = static_cast<int64_t>(group.joinSeq);
                const int64_t publishedSeq = static_cast<int64_t>(group.publishedSeq);
                const int64_t lastSeq = publishedSeq;
                const int64_t toSeq = query.snapshot() > 0
                    ? std::min<int64_t>(query.snapshot(), lastSeq)
                    : lastSeq;

                std::vector<sdkws::MsgData> messages;
                int64_t nextSeq = joinSeq;
                bool hasMore = false;
                if (query.beforeseq() > 0) {
                    // 向前：最新 N 用 beforeSeq=lastSeq+1；多取一条判断是否还有更旧。
                    const int64_t upper = std::min<int64_t>(query.beforeseq(), toSeq + 1);
                    messages = co_await MongoHandler::GetGroupLatestMessagesAsync(
                        query.groupid(), joinSeq, upper, toSeq, limit + 1);
                    hasMore = messages.size() > static_cast<size_t>(limit);
                    if (hasMore)
                        messages.erase(messages.begin());   // 丢弃最旧的一条
                    nextSeq = messages.empty() ? joinSeq : messages.front().seq();
                } else {
                    // 向后：增量/补洞；afterSeq 与 beforeSeq 都为 0 时从 joinSeq 起。
                    const int64_t from = std::max<int64_t>(
                        query.afterseq() > 0 ? query.afterseq() : joinSeq, joinSeq);
                    messages = co_await MongoHandler::GetGroupChatHistoryAsync(
                        query.groupid(), from, toSeq, limit + 1);
                    hasMore = messages.size() > static_cast<size_t>(limit);
                    if (hasMore)
                        messages.resize(limit);
                    nextSeq = messages.empty() ? from : messages.back().seq();
                }

                int64_t deletedSeq = 0;
                response->clear_messages();
                for (auto &message : messages) {
                    if (message.isdeleted())
                        deletedSeq = std::max<int64_t>(deletedSeq, message.seq());
                    *response->add_messages() = std::move(message);
                }
                response->set_hasmore(hasMore);
                response->set_nextseq(nextSeq);
                response->set_minseq(joinSeq);
                response->set_lastseq(toSeq);
                response->set_epoch(group.epoch);
                response->set_deletedseq(deletedSeq);
                reactor->Finish(grpc::Status::OK);
            } catch (const std::exception &e) {
                spdlog::error("GroupService.GetGroupChatHistory: {}", e.what());
                FinishGroupError(reactor, "DB_ERROR");
            }
        }, boost::asio::detached);
    return reactor;
}
