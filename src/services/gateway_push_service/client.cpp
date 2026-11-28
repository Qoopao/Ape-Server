#include "services/gateway_push_service/client.h"
#include "om/otel_trace_propagation.h"
#include "om/trace_context.h"
#include "util/grpc_async_util.h"

#include <chrono>
#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>

namespace {
// ClientContext 不可拷贝/移动，只能原地设置
void PrepareContext(grpc::ClientContext &ctx, int deadlineMs,
                    const ape::otel::TraceContext &trace = {}) {
    ape::otel::InjectTraceContext(
        ctx, trace.valid() ? trace : ape::otel::CurrentTraceContext());
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::milliseconds(deadlineMs));
}
} // namespace

GatewayPushClient::GatewayPushClient(std::shared_ptr<grpc::Channel> channel)
    : stub_(gateway_push::GatewayPushService::NewStub(channel)) {}

boost::asio::awaitable<GatewayPushOutcome>
GatewayPushClient::PushFrame(uint64_t account, int32_t type,
                             const std::string &data, int deadlineMs,
                             const std::string &requestId,
                             const std::string &errorCode,
                             const ape::otel::TraceContext &trace) {
    grpc::ClientContext ctx;
    PrepareContext(ctx, deadlineMs, trace);
    gateway_push::PushFrameReq req;
    req.set_account(account);
    req.set_type(type);
    req.set_data(data);
    req.set_request_id(requestId);
    req.set_error_code(errorCode);
    gateway_push::PushFrameResp resp;

    auto status = co_await ape::grpc_util::GrpcAwait([&](auto &&handler) {
        stub_->async()->PushFrame(&ctx, &req, &resp,
                                  std::forward<decltype(handler)>(handler));
    });

    GatewayPushOutcome outcome;
    if (!status.ok()) {
        // 没问到，不能当成"用户不在线"
        spdlog::warn("GatewayPushClient::PushFrame: account={} type={} error={}",
                     account, type, status.error_message());
        co_return outcome;
    }
    outcome.answered = true;
    outcome.delivered = resp.success();
    outcome.instanceId = resp.instance_id();
    co_return outcome;
}

boost::asio::awaitable<GatewayBatchOutcome>
GatewayPushClient::PushBatch(const std::vector<uint64_t> &accounts, int32_t type,
                             const std::string &data, int deadlineMs,
                             const ape::otel::TraceContext &trace) {
    grpc::ClientContext ctx;
    PrepareContext(ctx, deadlineMs, trace);
    gateway_push::PushBatchReq req;
    for (const auto account : accounts)
        req.add_accounts(account);
    req.set_type(type);
    req.set_data(data);
    gateway_push::PushBatchResp resp;

    auto status = co_await ape::grpc_util::GrpcAwait([&](auto &&handler) {
        stub_->async()->PushBatch(&ctx, &req, &resp,
                                  std::forward<decltype(handler)>(handler));
    });

    GatewayBatchOutcome outcome;
    if (!status.ok()) {
        spdlog::warn("GatewayPushClient::PushBatch: accounts={} type={} error={}",
                     accounts.size(), type, status.error_message());
        co_return outcome;
    }
    if (resp.results_size() != static_cast<int>(accounts.size())) {
        spdlog::error("GatewayPushClient::PushBatch: result size mismatch, "
                      "expected={}, got={}",
                      accounts.size(), resp.results_size());
        co_return outcome;
    }
    outcome.answered = true;
    outcome.instanceId = resp.instance_id();
    outcome.results.assign(resp.results().begin(), resp.results().end());
    co_return outcome;
}

boost::asio::awaitable<std::optional<bool>>
GatewayPushClient::IsOnline(uint64_t account, int deadlineMs) {
    grpc::ClientContext ctx;
    PrepareContext(ctx, deadlineMs);
    gateway_push::IsOnlineReq req;
    req.set_account(account);
    gateway_push::IsOnlineResp resp;

    auto status = co_await ape::grpc_util::GrpcAwait([&](auto &&handler) {
        stub_->async()->IsOnline(&ctx, &req, &resp,
                                 std::forward<decltype(handler)>(handler));
    });
    if (!status.ok()) {
        spdlog::warn("GatewayPushClient::IsOnline: account={} error={}", account,
                     status.error_message());
        co_return std::nullopt;
    }
    co_return resp.online();
}

boost::asio::awaitable<bool>
GatewayPushClient::KickUser(uint64_t account, const std::string &reason,
                            const std::string &exceptInstanceId, int deadlineMs) {
    grpc::ClientContext ctx;
    PrepareContext(ctx, deadlineMs);
    gateway_push::KickUserReq req;
    req.set_account(account);
    req.set_reason(reason);
    req.set_except_instance_id(exceptInstanceId);
    gateway_push::KickUserResp resp;

    auto status = co_await ape::grpc_util::GrpcAwait([&](auto &&handler) {
        stub_->async()->KickUser(&ctx, &req, &resp,
                                 std::forward<decltype(handler)>(handler));
    });
    if (!status.ok()) {
        spdlog::warn("GatewayPushClient::KickUser: account={} error={}", account,
                     status.error_message());
        co_return false;
    }
    co_return resp.kicked();
}
