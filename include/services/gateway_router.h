#ifndef GATEWAY_ROUTER_H
#define GATEWAY_ROUTER_H

#include "om/trace_context.h"
#include "services/backbon_service/client.h"
#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class GatewayPushClient;

// 推送三态（改造方案 4.5）：
//   Delivered : 目标网关确认该用户在此实例且帧已入队
//   NotOnline : 没有实例持有该账号的活跃 session（所有被问到的实例都明确回答"没有"）
//   Unknown   : 有实例不可达/超时，且无人报成功 —— 不得当作离线，由调用方决定重试
enum class PushResult { Delivered, NotOnline, Unknown };

// GatewayRouter - 非网关进程共享的推送路由组件。
// 职责：解析 account → 实例（读 gw:user），调用该实例的 GatewayPushService，
// 路由失效/实例不可达时广播兜底，并把结果归一成三态。
// 它不做投递判定——判定在网关侧（INV-1/INV-2）。
class GatewayRouter {
public:
  struct Instance {
    std::string instanceId;
    std::string address;
  };

  // provider 每次刷新时重新取 BackbonClient（注册是异步完成的，不能在构造时缓存）；
  // 取不到时退化到 fallback_address 单实例（本地开发 / 总回滚）
  using BackbonProvider = std::function<BackbonClient *()>;
  GatewayRouter(BackbonProvider backbon_provider, std::string fallback_address);
  ~GatewayRouter();

  // 从 etcd 前缀 /services/GatewayPushService/ 刷新实例缓存（带最小刷新间隔）
  boost::asio::awaitable<void> Refresh(bool force = false);

  // account → 实例 ID；路由缺失或 GATEWAY_ROUTING=0 时返回空
  boost::asio::awaitable<std::optional<std::string>> Resolve(uint64_t account);

  std::vector<Instance> Instances();
  std::optional<std::string> AddressOf(const std::string &instanceId);

  boost::asio::awaitable<PushResult>
  PushFrame(uint64_t account, int32_t type, std::string data,
            const std::string &requestId = {},
            const std::string &errorCode = {},
            const ape::otel::TraceContext &trace = {});

  boost::asio::awaitable<std::vector<PushResult>>
  PushBatch(const std::vector<uint64_t> &accounts, int32_t type, std::string data);

  // 群扇出专用：无路由/实例否认一律 NotOnline，实例不可达为 Unknown；
  // 不向所有网关广播、不重试（消息可经 245 恢复）。
  boost::asio::awaitable<std::vector<PushResult>>
  PushBatchBestEffort(const std::vector<uint64_t> &accounts, int32_t type,
                      std::string data,
                      const ape::otel::TraceContext &trace = {});

  // 跨实例互踢：路由可用且不是自己时只打目标实例，否则向所有实例广播
  boost::asio::awaitable<void>
  KickUser(uint64_t account, const std::string &reason,
           const std::string &exceptInstanceId);

  static bool RoutingEnabled();

private:
  // 纯数据，便于按值返回；并发更新时由调用方持有的 mutex 保护
  struct BroadcastSummary {
    bool anyDelivered = false;
    bool anyUnanswered = false;
    size_t questioned = 0;
  };

  GatewayPushClient *ClientFor(const std::string &address);
  void ApplyInstances(const std::vector<Instance> &instances);

  // 并发执行一组任务并等待全部完成（每个任务自带 RPC 截止时间）
  static boost::asio::awaitable<void>
  RunAll(std::vector<std::function<boost::asio::awaitable<void>()>> tasks);

  boost::asio::awaitable<BroadcastSummary>
  BroadcastFrame(uint64_t account, int32_t type, const std::string &data,
                 const std::string &skipInstanceId,
                 const std::string &requestId = {},
                 const std::string &errorCode = {},
                 const ape::otel::TraceContext &trace = {});

  BackbonProvider backbon_provider_;
  const std::string fallback_address_;

  std::mutex mutex_;
  std::map<std::string, std::string> instance_address_;
  std::map<std::string, std::unique_ptr<GatewayPushClient>> clients_;
  std::chrono::steady_clock::time_point last_refresh_{};
  bool refreshing_{false};
  bool refreshed_once_{false};
};

#endif // GATEWAY_ROUTER_H
