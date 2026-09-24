#ifndef WS_SESSION_MANAGER_H
#define WS_SESSION_MANAGER_H

#include "gateway/ws_session.h"

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

private:
    WSSessionManager() = default;
    WSSessionManager(const WSSessionManager&) = delete;
    WSSessionManager& operator=(const WSSessionManager&) = delete;

    std::shared_mutex mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<WSSession>> sessions_;
};

#endif