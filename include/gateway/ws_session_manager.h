#ifndef WS_SESSION_MANAGER_H
#define WS_SESSION_MANAGER_H

#include "gateway/ws_session.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

// WSSessionManager 全局连接池单例
// 职责：维护 account → WSSession 的映射，提供注册/注销/推送接口
// 线程安全：使用 shared_mutex，读多写少优化
class WSSessionManager {
public:
    static WSSessionManager& instance();

    // 注册一个 WebSocket 连接
    void registerSession(const uint64_t account,
                         std::shared_ptr<WSSession> session);

    // 注销一个 WebSocket 连接，加上std::shared_ptr<WSSession> session避免误删新的session
    bool unregisterSession(const uint64_t account, std::shared_ptr<WSSession> session);

    // 向指定用户推送消息
    // 返回 true 表示 session 存在并已投递发送
    bool pushToUser(const uint64_t account,
                    std::shared_ptr<std::string> payload);

    // 检查用户是否有活跃连接
    bool hasSession(const uint64_t account);
    std::vector<uint64_t> onlineAccounts();

    // 踢掉某账号的本地连接（跨实例互踢 / 同实例重复登录），返回是否命中
    bool kickUser(const uint64_t account, const std::string &reason);

    // 优雅下线：给所有本地连接发 SERVER_DRAINING，之后由调用方等待 grace
    void notifyDraining();
    bool draining() const { return draining_.load(); }

    size_t connectionCount() const;
    // 关闭全部本地连接（grace 期满后强制下线）
    void closeAll();
    // GATEWAY_MAX_CONNECTIONS，缺省 20000
    static size_t MaxConnections();

private:
    WSSessionManager() = default;
    WSSessionManager(const WSSessionManager&) = delete;
    WSSessionManager& operator=(const WSSessionManager&) = delete;

    mutable std::shared_mutex mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<WSSession>> sessions_;
    std::atomic<bool> draining_{false};
};

#endif
