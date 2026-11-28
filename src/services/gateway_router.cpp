#include "services/gateway_router.h"
#include "services/gateway_push_service/client.h"
#include "storage/redisconnector.h"
#include "util/gateway_route.h"

#include <grpcpp/grpcpp.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <set>

namespace asio = boost::asio;

GatewayRouter::GatewayRouter(BackbonProvider backbon_provider,
                             std::string fallback_address)
    : backbon_provider_(std::move(backbon_provider)),
      fallback_address_(std::move(fallback_address)) {}

GatewayRouter::~GatewayRouter() = default;

// GATEWAY_ROUTING=0时返回false,
bool GatewayRouter::RoutingEnabled() {
  const char *value = std::getenv("GATEWAY_ROUTING");
  return !(value != nullptr && std::string(value) == "0");
}

// 把 /services/GatewayPushService/{id} 下的实例同步到instance_address_
boost::asio::awaitable<void> GatewayRouter::Refresh(bool force) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    // force为true的时候强制查询，不等待kRefreshIntervalMs
    if (!force && refreshed_once_ && now - last_refresh_ < std::chrono::milliseconds(ape::gateway::kRefreshIntervalMs)) {
      co_return;
    }
    if (refreshing_){
      co_return;
    }
    refreshing_ = true;
  }

  // 拿到各个网关推送服务的实例
  std::vector<Instance> discovered;
  try {
    BackbonClient *backbon = backbon_provider_ ? backbon_provider_() : nullptr;
    if (backbon != nullptr) {
      const auto resp = co_await backbon->GetServicesList("GatewayPushService");
      if (resp.registered()) {
        for (const auto &instance : resp.instances()) {
          if (instance.instance_id().empty() || instance.address().empty())
            continue;
          discovered.push_back({instance.instance_id(), instance.address()});
        }
      }
    }
  } catch (const std::exception &e) {
    spdlog::warn("GatewayRouter: refresh instance list failed: {}", e.what());
  }

  // 加锁写回instance_address_
  {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshing_ = false;
    last_refresh_ = std::chrono::steady_clock::now();
    if (!discovered.empty()) {
      instance_address_.clear();
      for (const auto &instance : discovered){
        instance_address_[instance.instanceId] = instance.address;
      }
    } else if (!refreshed_once_ && !fallback_address_.empty()) {
      // 首次查询实例失败了，用兜底的实例地址
      instance_address_.clear();
      instance_address_["fallback"] = fallback_address_;
      spdlog::info("GatewayRouter: using fallback gateway address {}", fallback_address_);
    }
    refreshed_once_ = true;
  }
  co_return;
}

boost::asio::awaitable<std::optional<std::string>>
GatewayRouter::Resolve(uint64_t account) {
  if (!RoutingEnabled()){
    co_return std::nullopt;
  }
  
  try {
    // 从redis获得账号的登陆网关实例
    const auto value = co_await RedisConnector::instance().get(ape::gateway::RouteKey(account));
    if (!value.has_value()){
      co_return std::nullopt;
    }

    // 尝试解析实例ID
    const auto parsed = ape::gateway::ParseRoute(*value);
    if (!parsed.has_value()){
      co_return std::nullopt;
    }
    co_return parsed->instanceId;
  } catch (const std::exception &e) {
    // 路由查询失败交给广播兜底
    spdlog::warn("GatewayRouter: resolve account={} failed: {}", account, e.what());
    co_return std::nullopt;
  }
}

// 把map拷成vector返回（给广播遍历用）
std::vector<GatewayRouter::Instance> GatewayRouter::Instances() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Instance> instances;
  instances.reserve(instance_address_.size());
  for (const auto &entry : instance_address_){
    instances.push_back({entry.first, entry.second});
  }
  return instances;
}

// 查询单个实例的地址
std::optional<std::string>
GatewayRouter::AddressOf(const std::string &instanceId) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = instance_address_.find(instanceId);
  if (it == instance_address_.end()){
    return std::nullopt;
  }
  return it->second;
}

// 查询有没有对应地址的网关推送服务的grpc客户端，没有就新创建
GatewayPushClient *GatewayRouter::ClientFor(const std::string &address) {
  if (address.empty()){
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = clients_.find(address);
  if (it != clients_.end()){
    return it->second.get();
  }
    
  auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
  auto client = std::make_unique<GatewayPushClient>(channel);
  auto *raw = client.get();
  clients_[address] = std::move(client);
  return raw;
}

// 并发执行N个协程并等全部完成
boost::asio::awaitable<void> GatewayRouter::RunAll(std::vector<std::function<boost::asio::awaitable<void>()>> tasks) {
  if (tasks.empty()){
    co_return;
  }
    
  const auto executor = co_await asio::this_coro::executor;
  using DoneChannel = asio::experimental::channel<void(boost::system::error_code)>;
  auto done = std::make_shared<DoneChannel>(executor, tasks.size());
  const size_t count = tasks.size();
  for (auto &task : tasks) {
    asio::co_spawn(
        executor,
        [task = std::move(task), done, count]() -> asio::awaitable<void> {
          try {
            co_await task();
          } catch (const std::exception &e) {
            spdlog::warn("GatewayRouter: parallel push task failed: {}", e.what());
          }
          // 异步往donechannel发送信号
          boost::system::error_code ec;
          co_await done->async_send(boost::system::error_code{},asio::redirect_error(asio::use_awaitable, ec));
        },
        asio::detached);
  }

  // 直到收到全部完成信号才返回
  for (size_t i = 0; i < count; ++i) {
    boost::system::error_code ec;
    co_await done->async_receive(asio::redirect_error(asio::use_awaitable, ec));
  }
}

// 向所有实例并发问一遍，因为这时不知道用户登陆在哪个实例上
// 要推的账号、帧类型、数据、要跳过的实例（已经问过的那个）
boost::asio::awaitable<GatewayRouter::BroadcastSummary>
GatewayRouter::BroadcastFrame(uint64_t account, int32_t type,
                              const std::string &data,
                              const std::string &skipInstanceId,
                              const std::string &requestId,
                              const std::string &errorCode,
                              const ape::otel::TraceContext &trace) {
  auto summary = BroadcastSummary{};
  auto instances = Instances();
  if (!skipInstanceId.empty()) {
    instances.erase(
        std::remove_if(instances.begin(), instances.end(),
                       [&](const Instance &instance) {
                         return instance.instanceId == skipInstanceId;
                       }),
        instances.end());
  }
  if (instances.empty())
    co_return summary;

  summary.questioned = instances.size();
  auto shared = std::make_shared<BroadcastSummary>();
  shared->questioned = instances.size();
  auto guard = std::make_shared<std::mutex>();
  std::vector<std::function<asio::awaitable<void>()>> tasks;
  tasks.reserve(instances.size());
  for (const auto &instance : instances) {
    auto *client = ClientFor(instance.address);
    if (client == nullptr) {
      std::lock_guard<std::mutex> lock(*guard);
      shared->anyUnanswered = true;
      continue;
    }
    tasks.push_back([client, account, type, data, shared, guard, requestId, errorCode, trace]() -> asio::awaitable<void> {
      const auto outcome =
          co_await client->PushFrame(account, type, data,
                                     ape::gateway::kRpcTimeoutMs, requestId,
                                     errorCode, trace);
      std::lock_guard<std::mutex> lock(*guard);
      if (!outcome.answered) {
        shared->anyUnanswered = true;
      } else if (outcome.delivered) {
        shared->anyDelivered = true;
      }
    });
  }
  co_await RunAll(std::move(tasks));
  summary.anyDelivered = shared->anyDelivered;
  summary.anyUnanswered = shared->anyUnanswered;
  co_return summary;
}

/*
  流程

  1. Refresh()                               确保实例列表最新
  2. inst = Resolve(account)                 在某个实例gw-2？
    有 inst 且能查到地址：
        r = gRPC PushFrame(gw-2, 300ms)
        r.delivered == true、返回 Delivered
    否则记下"已经问过 gw-2"
        answered=false（超时/连不上）         knownFailure = true
        answered=true 但 false               只是不在 gw-2
        广播：向除 gw-2 外的实例并发问
          任意 Delivered                     Delivered
          有 anyUnanswered 或 knownFailure   Unknown
          没有任何实例可问                    Unknown
          其余（都明确说不在）                 NotOnline
*/
boost::asio::awaitable<PushResult>
GatewayRouter::PushFrame(uint64_t account, int32_t type, std::string data,
                         const std::string &requestId,
                         const std::string &errorCode,
                         const ape::otel::TraceContext &trace) {
  co_await Refresh(false);

  bool knownFailure = false;
  std::string askedInstance;
  if (RoutingEnabled()) {
    const auto resolved = co_await Resolve(account);
    if (resolved.has_value()) {
      const auto address = AddressOf(*resolved);
      if (address.has_value()) {
        if (auto *client = ClientFor(*address); client != nullptr) {
          const auto outcome = co_await client->PushFrame(
              account, type, data, ape::gateway::kRpcTimeoutMs, requestId,
              errorCode, trace);
          if (outcome.answered && outcome.delivered)
            co_return PushResult::Delivered;
          askedInstance = *resolved;
          if (!outcome.answered)
            knownFailure = true;
          if (outcome.answered) {
            spdlog::info("GatewayRouter: account={} not on instance={}, "
                         "broadcast fallback",
                         account, *resolved);
          }
        }
      }
    }
  }

  const auto summary = co_await BroadcastFrame(account, type, data,
                                               askedInstance, requestId,
                                               errorCode, trace);
  if (summary.anyDelivered)
    co_return PushResult::Delivered;
  if (summary.anyUnanswered || knownFailure)
    co_return PushResult::Unknown;
  if (summary.questioned == 0 && askedInstance.empty())
    co_return PushResult::Unknown; // 没有任何实例可问
  co_return PushResult::NotOnline;
}


// 处理群消息的跨实例投递
boost::asio::awaitable<std::vector<PushResult>>
GatewayRouter::PushBatch(const std::vector<uint64_t> &accounts, int32_t type,
                         std::string data) {
  const size_t total = accounts.size();
  std::vector<PushResult> results(total, PushResult::Unknown);
  if (total == 0)
    co_return results;

  co_await Refresh(false);

  // 1) 批量读路由，按实例分组
  std::vector<std::optional<std::string>> routes(total);
  if (RoutingEnabled()) {
    std::vector<std::string> keys;
    keys.reserve(total);
    for (const auto account : accounts)
      keys.push_back(ape::gateway::RouteKey(account));
    try {
      const auto values = co_await RedisConnector::instance().mget(keys);
      for (size_t i = 0; i < total && i < values.size(); ++i) {
        if (!values[i].has_value())
          continue;
        const auto parsed = ape::gateway::ParseRoute(*values[i]);
        if (parsed.has_value())
          routes[i] = parsed->instanceId;
      }
    } catch (const std::exception &e) {
      spdlog::warn("GatewayRouter: batch route lookup failed: {}", e.what());
    }
  }

  std::map<std::string, std::vector<size_t>> grouped;
  std::vector<size_t> unresolved;
  for (size_t i = 0; i < total; ++i) {
    if (routes[i].has_value() && AddressOf(*routes[i]).has_value())
      grouped[*routes[i]].push_back(i);
    else
      unresolved.push_back(i);
  }

  // 2) 每个实例一次 PushBatch
  std::vector<GatewayBatchOutcome> group_outcomes(grouped.size());
  std::vector<std::vector<size_t>> group_indices;
  group_indices.reserve(grouped.size());
  std::vector<std::function<asio::awaitable<void>()>> tasks;
  size_t group = 0;
  for (const auto &entry : grouped) {
    group_indices.push_back(entry.second);
    const auto address = AddressOf(entry.first);
    auto *client = address.has_value() ? ClientFor(*address) : nullptr;
    auto *outcome = &group_outcomes[group];
    const auto indices = entry.second;
    if (client == nullptr) {
      ++group;
      continue;
    }
    tasks.push_back([client, accounts, indices, type, data, outcome]()
                        -> asio::awaitable<void> {
      std::vector<uint64_t> subset;
      subset.reserve(indices.size());
      for (const auto index : indices)
        subset.push_back(accounts[index]);
      *outcome = co_await client->PushBatch(subset, type, data,
                                            ape::gateway::kRpcTimeoutMs);
    });
    ++group;
  }
  co_await RunAll(std::move(tasks));

  // 3) 已投递的直接定型；其余进入广播兜底
  std::vector<size_t> failed = unresolved;
  for (size_t g = 0; g < group_indices.size(); ++g) {
    const auto &indices = group_indices[g];
    const auto &outcome = group_outcomes[g];
    for (size_t k = 0; k < indices.size(); ++k) {
      const size_t index = indices[k];
      if (outcome.answered && k < outcome.results.size() &&
          outcome.results[k]) {
        results[index] = PushResult::Delivered;
      } else {
        failed.push_back(index);
      }
    }
  }
  std::sort(failed.begin(), failed.end());
  failed.erase(std::unique(failed.begin(), failed.end()), failed.end());

  if (failed.empty())
    co_return results;

  const auto instances = Instances();
  if (instances.empty()) {
    for (const auto index : failed)
      results[index] = PushResult::Unknown;
    co_return results;
  }

  // 4) 广播：对每个实例推一次剩余账号，再按账号聚合
  std::vector<GatewayBatchOutcome> broadcasts(instances.size());
  std::vector<size_t> broadcastable;
  tasks.clear();
  for (size_t i = 0; i < instances.size(); ++i) {
    auto *client = ClientFor(instances[i].address);
    if (client == nullptr)
      continue;
    auto *outcome = &broadcasts[i];
    broadcastable.push_back(i);
    tasks.push_back([client, accounts, failed, type, data, outcome]()
                        -> asio::awaitable<void> {
      std::vector<uint64_t> subset;
      subset.reserve(failed.size());
      for (const auto index : failed)
        subset.push_back(accounts[index]);
      *outcome = co_await client->PushBatch(subset, type, data,
                                            ape::gateway::kRpcTimeoutMs);
    });
  }
  co_await RunAll(std::move(tasks));

  for (size_t f = 0; f < failed.size(); ++f) {
    bool anyDelivered = false;
    bool anyUnanswered = false;
    for (const auto i : broadcastable) {
      const auto &outcome = broadcasts[i];
      if (!outcome.answered || f >= outcome.results.size()) {
        anyUnanswered = true;
      } else if (outcome.results[f]) {
        anyDelivered = true;
      }
    }
    if (broadcastable.empty())
      anyUnanswered = true;
    results[failed[f]] = anyDelivered
                             ? PushResult::Delivered
                             : (anyUnanswered ? PushResult::Unknown
                                              : PushResult::NotOnline);
  }
  co_return results;
}


// 跨实例踢用户下线，目前架构仅支持单客户端登陆
boost::asio::awaitable<void>
GatewayRouter::KickUser(uint64_t account, const std::string &reason,
                        const std::string &exceptInstanceId) {
  co_await Refresh(false);

  std::vector<Instance> targets;
  const auto resolved = co_await Resolve(account);
  if (resolved.has_value() && *resolved != exceptInstanceId &&
      AddressOf(*resolved).has_value()) {
    targets.push_back({*resolved, *AddressOf(*resolved)});
  } else {
    for (const auto &instance : Instances()) {
      if (instance.instanceId != exceptInstanceId)
        targets.push_back(instance);
    }
  }
  if (targets.empty())
    co_return;

  std::vector<std::function<asio::awaitable<void>()>> tasks;
  tasks.reserve(targets.size());
  for (const auto &target : targets) {
    auto *client = ClientFor(target.address);
    if (client == nullptr)
      continue;
    tasks.push_back([client, account, reason, exceptInstanceId]()
                        -> asio::awaitable<void> {
      co_await client->KickUser(account, reason, exceptInstanceId,
                                ape::gateway::kRpcTimeoutMs);
    });
  }
  co_await RunAll(std::move(tasks));
  co_return;
}

// 群扇出：尽力而为。只问已知路由所在的网关，不广播兜底、不重试；
// 无路由/实例明确回答“不在” => NotOnline，实例不可达 => Unknown。
boost::asio::awaitable<std::vector<PushResult>>
GatewayRouter::PushBatchBestEffort(const std::vector<uint64_t> &accounts,
                                   int32_t type, std::string data,
                                   const ape::otel::TraceContext &trace) {
  const size_t total = accounts.size();
  std::vector<PushResult> results(total, PushResult::NotOnline);
  if (total == 0)
    co_return results;

  co_await Refresh(false);

  std::vector<std::optional<std::string>> routes(total);
  if (RoutingEnabled()) {
    std::vector<std::string> keys;
    keys.reserve(total);
    for (const auto account : accounts)
      keys.push_back(ape::gateway::RouteKey(account));
    try {
      const auto values = co_await RedisConnector::instance().mget(keys);
      for (size_t i = 0; i < total && i < values.size(); ++i) {
        if (!values[i].has_value())
          continue;
        const auto parsed = ape::gateway::ParseRoute(*values[i]);
        if (parsed.has_value())
          routes[i] = parsed->instanceId;
      }
    } catch (const std::exception &e) {
      spdlog::warn("GatewayRouter: best-effort route lookup failed: {}", e.what());
      co_return results;
    }
  }

  std::map<std::string, std::vector<size_t>> grouped;
  for (size_t i = 0; i < total; ++i) {
    if (routes[i].has_value() && AddressOf(*routes[i]).has_value())
      grouped[*routes[i]].push_back(i);
    else
      results[i] = PushResult::NotOnline;
  }
  if (grouped.empty())
    co_return results;

  std::vector<GatewayBatchOutcome> outcomes(grouped.size());
  std::vector<std::vector<size_t>> groupIndices;
  groupIndices.reserve(grouped.size());
  std::vector<std::function<asio::awaitable<void>()>> tasks;
  size_t group = 0;
  for (const auto &entry : grouped) {
    groupIndices.push_back(entry.second);
    const auto address = AddressOf(entry.first);
    auto *client = address.has_value() ? ClientFor(*address) : nullptr;
    auto *outcome = &outcomes[group];
    const auto indices = entry.second;
    if (client != nullptr) {
      tasks.push_back([client, accounts, indices, type, data, outcome, trace]()
                          -> asio::awaitable<void> {
        std::vector<uint64_t> subset;
        subset.reserve(indices.size());
        for (const auto index : indices)
          subset.push_back(accounts[index]);
        *outcome = co_await client->PushBatch(subset, type, data,
                                              ape::gateway::kRpcTimeoutMs, trace);
      });
    }
    ++group;
  }
  co_await RunAll(std::move(tasks));

  for (size_t g = 0; g < groupIndices.size(); ++g) {
    const auto &indices = groupIndices[g];
    const auto &outcome = outcomes[g];
    for (size_t k = 0; k < indices.size(); ++k) {
      const auto index = indices[k];
      if (!outcome.answered) {
        results[index] = PushResult::Unknown;   // 实例不可达：不重试
      } else if (k < outcome.results.size() && outcome.results[k]) {
        results[index] = PushResult::Delivered;
      } else {
        results[index] = PushResult::NotOnline; // 实例明确否认：不广播
      }
    }
  }
  co_return results;
}
