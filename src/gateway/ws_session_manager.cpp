#include "gateway/ws_session_manager.h"
#include "om/otel_metrics.h"

#include <spdlog/spdlog.h>

WSSessionManager& WSSessionManager::instance() {
    static WSSessionManager mgr;
    return mgr;
}

void WSSessionManager::registerSession(const std::string& userId,
                                       std::shared_ptr<WSSession> session) {
    std::unique_lock lock(mutex_);
    bool isReplacing = sessions_.find(userId) != sessions_.end();
    sessions_[userId] = session;
    int64_t total = static_cast<int64_t>(sessions_.size());
    lock.unlock();

    if (!isReplacing) {
        ape::otel::WsConnectionsActive().Add(1);
    }
    spdlog::info("WSSessionManager: registered session for user={}, total={}", userId, total);
}


bool WSSessionManager::unregisterSession(const std::string& userId,
                                         std::shared_ptr<WSSession> session) {
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(userId);
    if (it != sessions_.end() && it->second == session) {
        sessions_.erase(it);
        lock.unlock();
        ape::otel::WsConnectionsActive().Add(-1);
        spdlog::info("unregistered session for user={}", userId);
        return true;
    }
    lock.unlock();
    spdlog::info("stale session for user={}, skipping", userId);
    return false;
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