#ifndef GATEWAY_PUSH_SERVICE_CLIENT_H
#define GATEWAY_PUSH_SERVICE_CLIENT_H

#include "gateway_push.grpc.pb.h"
#include "gateway_push.pb.h"
#include "om/trace_context.h"
#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <grpcpp/channel.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// 推送结果（三态判定的原始素材）：
//   answered=false            → 没问到（RPC 失败/超时），调用方必须按 Unknown 处理
//   answered=true, delivered=false → 目标实例明确回答"这个账号不在我这儿"
struct GatewayPushOutcome {
    bool answered = false;
    bool delivered = false;
    std::string instanceId;
};

struct GatewayBatchOutcome {
    bool answered = false;
    std::vector<bool> results;
    std::string instanceId;
};

// GatewayPushClient - 单个网关实例的推送客户端。
// 会话级超时由调用方传入（默认单次 RPC 300ms，见 Q-3）。
class GatewayPushClient {
public:
    explicit GatewayPushClient(std::shared_ptr<grpc::Channel> channel);

    boost::asio::awaitable<GatewayPushOutcome>
    PushFrame(uint64_t account, int32_t type, const std::string &data,
              int deadlineMs = 300, const std::string &requestId = {},
              const std::string &errorCode = {},
              const ape::otel::TraceContext &trace = {});

    boost::asio::awaitable<GatewayBatchOutcome>
    PushBatch(const std::vector<uint64_t> &accounts, int32_t type,
              const std::string &data, int deadlineMs = 300,
              const ape::otel::TraceContext &trace = {});

    // nullopt = RPC 失败/超时（没问到）；true/false = 目标实例的明确答复
    boost::asio::awaitable<std::optional<bool>>
    IsOnline(uint64_t account, int deadlineMs = 300);

    // 返回 true 表示该实例确实踢掉了一个本地 session
    boost::asio::awaitable<bool>
    KickUser(uint64_t account, const std::string &reason,
             const std::string &exceptInstanceId, int deadlineMs = 300);

private:
    std::unique_ptr<gateway_push::GatewayPushService::Stub> stub_;
};

#endif
