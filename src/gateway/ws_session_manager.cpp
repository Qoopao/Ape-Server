#include "gateway/ws_session_manager.h"

#include <spdlog/spdlog.h>

WSSessionManager& WSSessionManager::instance() {
    static WSSessionManager mgr;
    return mgr;
}

void WSSessionManager::registerSession(const std::string& userId,
                                       std::shared_ptr<WSSession> session) {
    std::unique_lock lock(mutex_);
    // 如果已有旧连接，日志警告（可能是多端登录，后续可扩展为多session管理）
    auto it = sessions_.find(userId);
    if (it != sessions_.end()) {
        spdlog::warn("WSSessionManager: replacing existing session for user={}", userId);
    }
    sessions_[userId] = session;
    spdlog::info("WSSessionManager: registered session for user={}, total={}",
                 userId, sessions_.size());
}

void WSSessionManager::unregisterSession(const std::string& userId) {
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(userId);
    if (it != sessions_.end()) {
        sessions_.erase(it);
        spdlog::info("WSSessionManager: unregistered session for user={}, total={}",
                     userId, sessions_.size());
    }
}

bool WSSessionManager::pushToUser(const std::string& userId,
                                  std::shared_ptr<std::string> payload) {
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(userId);
    if (it == sessions_.end()) {
        spdlog::warn("WSSessionManager: no session for user={}, cannot push", userId);
        return false;
    }

    auto session = it->second;
    lock.unlock();  // 提前释放锁，避免 asyncSend 回调中可能的死锁

    session->asyncSend(payload);
    return true;
}

bool WSSessionManager::hasSession(const std::string& userId) {
    std::shared_lock lock(mutex_);
    return sessions_.find(userId) != sessions_.end();
}