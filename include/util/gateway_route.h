#ifndef APE_UTIL_GATEWAY_ROUTE_H
#define APE_UTIL_GATEWAY_ROUTE_H

#include <cstdint>
#include <optional>
#include <string>

// 网关路由表约定（改造方案 4.2）
//   key   : gw:user:{account}
//   value : "{instanceId}:{sessionId}"，sessionId 是每连接一个 UUID
//   TTL   : 300s，断线不删，靠 TTL 过期
namespace ape::gateway {

inline constexpr long long kRouteTtlSeconds = 300;
inline constexpr int kRpcTimeoutMs = 300;   // 单次推送 RPC 截止
inline constexpr int kRefreshIntervalMs = 2000;

inline std::string RouteKey(uint64_t account) {
  return "gw:user:" + std::to_string(account);
}

struct ParsedRoute {
  std::string instanceId;
  std::string sessionId;
};

// sessionId 是 UUID（不含冒号），因此从最后一个冒号切分可兼容
// 含端口的默认实例 ID（hostname:port）。
inline std::optional<ParsedRoute> ParseRoute(const std::string &value) {
  const auto pos = value.rfind(':');
  if (pos == std::string::npos || pos == 0 || pos + 1 >= value.size())
    return std::nullopt;
  ParsedRoute route;
  route.instanceId = value.substr(0, pos);
  route.sessionId = value.substr(pos + 1);
  if (route.instanceId.empty() || route.sessionId.empty())
    return std::nullopt;
  return route;
}

// 心跳/续期 CAS：只有仍持有路由的一方才能续期成功（INV-6）
inline constexpr const char *kRouteRenewScript =
    "local v = redis.call('GET', KEYS[1]) "
    "if v == ARGV[1] then "
    "  return redis.call('EXPIRE', KEYS[1], tonumber(ARGV[2])) "
    "end "
    "return 0";

} // namespace ape::gateway

#endif // APE_UTIL_GATEWAY_ROUTE_H
