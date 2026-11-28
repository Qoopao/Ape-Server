#include "util/instance_id.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

#include <spdlog/spdlog.h>

namespace ape::instance {
namespace {

std::mutex g_mutex;
std::string g_self;

std::string Env(const char *name) {
  const char *value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

std::string PortOf(const std::string &listen_address) {
  const auto pos = listen_address.rfind(':');
  if (pos == std::string::npos || pos + 1 >= listen_address.size())
    return "0";
  return listen_address.substr(pos + 1);
}

std::string Hostname() {
  char buffer[256] = {0};
  if (::gethostname(buffer, sizeof(buffer) - 1) != 0)
    return "unknown";
  buffer[sizeof(buffer) - 1] = '\0';
  std::string host(buffer);
  return host.empty() ? "unknown" : host;
}

// 取本机第一块可用网卡的 IPv4；优先私有网段，避免上报 link-local。
std::string PrimaryIPv4() {
  struct ifaddrs *ifaddr = nullptr;
  if (::getifaddrs(&ifaddr) != 0)
    return {};
  std::string chosen;
  for (auto *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET)
      continue;
    if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0)
      continue;
    char host[NI_MAXHOST] = {0};
    if (::getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host,
                      sizeof(host), nullptr, 0, NI_NUMERICHOST) != 0)
      continue;
    const std::string ip(host);
    if (ip.rfind("169.254.", 0) == 0)
      continue;
    if (chosen.empty())
      chosen = ip;
    if (ip.rfind("10.", 0) == 0 || ip.rfind("192.168.", 0) == 0 ||
        ip.rfind("172.", 0) == 0) {
      chosen = ip;
      break;
    }
  }
  ::freeifaddrs(ifaddr);
  return chosen;
}

} // namespace

std::string Configured() { return Env("SERVICE_INSTANCE_ID"); }

std::string ForListenAddress(const std::string &listen_address) {
  const std::string configured = Configured();
  if (!configured.empty())
    return configured;
  return Hostname() + ":" + PortOf(listen_address);
}

std::string AdvertiseAddress(const std::string &listen_address) {
  const std::string configured = Env("SERVICE_ADVERTISE_ADDR");
  if (!configured.empty())
    return configured;
  const std::string ip = PrimaryIPv4();
  if (ip.empty()) {
    spdlog::warn("instance: no routable IPv4 found, advertising 127.0.0.1");
    return "127.0.0.1:" + PortOf(listen_address);
  }
  return ip + ":" + PortOf(listen_address);
}

void SetSelf(const std::string &id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_self = id;
}

std::string Self() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_self;
}

} // namespace ape::instance
