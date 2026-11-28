#ifndef APE_UTIL_INSTANCE_ID_H
#define APE_UTIL_INSTANCE_ID_H

#include <string>

// 实例身份（多网关改造 4.1 / Q-1）
//
// 规则：
//   - 实例 ID 优先取环境变量 SERVICE_INSTANCE_ID，要求重启后保持不变；
//   - 未配置时退化为 hostname:<监听端口>，换端口即视为新实例；
//   - 注册上报的地址必须是可路由地址，缺省取本机第一块非 loopback 网卡的
//     IPv4，可用 SERVICE_ADVERTISE_ADDR 覆盖，绝不能是 0.0.0.0。
namespace ape::instance {

// 已配置的实例 ID（未配置返回空串）
std::string Configured();

// 为某个服务的监听地址计算实例 ID（SERVICE_INSTANCE_ID 优先）
std::string ForListenAddress(const std::string &listen_address);

// 服务注册时上报的可路由地址（SERVICE_ADVERTISE_ADDR 优先）
std::string AdvertiseAddress(const std::string &listen_address);

// 本进程网关实例 ID（由 main 在启动网关前设置）
void SetSelf(const std::string &id);
std::string Self();

} // namespace ape::instance

#endif // APE_UTIL_INSTANCE_ID_H
