#include "gateway/ws_session_manager.h"
#include "util/otel_metrics.h"

#include <spdlog/spdlog.h>

WSSessionManager& WSSessionManager::instance() {
    static WSSessionManager mgr;
    return mgr;
}

void WSSessionManager::registerSession(const std::string& userId,
                                       std::shared_ptr<WSSession> session) {
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(userId);
    if (it != sessions_.end()) {
        spdlog::warn("WSSessionManager: replacing existing session for user={}", userId);
    }
    sessions_[userId] = session;
    int64_t total = static_cast<int64_t>(sessions_.size());
    lock.unlock();

    ape::otel::WsConnectionsActive().Add(1);
    spdlog::info("WSSessionManager: registered session for user={}, total={}", userId, total);
}

void WSSessionManager::unregisterSession(const std::string& userId) {
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(userId);
    if (it != sessions_.end()) {
        sessions_.erase(it);
    }
    int64_t total = static_cast<int64_t>(sessions_.size());
    lock.unlock();

    ape::otel::WsConnectionsActive().Add(-1);
    spdlog::info("WSSessionManager: unregistered session for user={}, total={}", userId, total);
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