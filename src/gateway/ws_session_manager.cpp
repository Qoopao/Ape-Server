#include "gateway/ws_session_manager.h"
#include "om/otel_metrics.h"

#include <cstdint>
#include <spdlog/spdlog.h>

WSSessionManager& WSSessionManager::instance() {
    static WSSessionManager mgr;
    return mgr;
}

void WSSessionManager::registerSession(const uint64_t account,
                                       std::shared_ptr<WSSession> session) {
    std::unique_lock lock(mutex_);
    bool isReplacing = sessions_.find(account) != sessions_.end();
    sessions_[account] = session;
    uint64_t total = sessions_.size();
    lock.unlock();

    if (!isReplacing) {
        ape::otel::WsConnectionsActive().Add(1);
    }
    spdlog::info("WSSessionManager: registered session for user={}, total={}", account, total);
}


bool WSSessionManager::unregisterSession(const uint64_t account,
                                         std::shared_ptr<WSSession> session) {
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(account);
    if (it != sessions_.end() && it->second == session) {
        sessions_.erase(it);
        lock.unlock();
        ape::otel::WsConnectionsActive().Add(-1);
        spdlog::info("unregistered session for user={}", account);
        return true;
    }
    lock.unlock();
    spdlog::info("stale session for user={}, skipping", account);
    return false;
}



bool WSSessionManager::pushToUser(const uint64_t account,
                                  std::shared_ptr<std::string> payload) {
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(account);
    if (it == sessions_.end()) {
        spdlog::warn("WSSessionManager: no session for user={}, cannot push", account);
        return false;
    }

    auto session = it->second;
    lock.unlock();  // 提前释放锁，避免 asyncSend 回调中可能的死锁

    session->asyncSend(payload);
    return true;
}

bool WSSessionManager::hasSession(const uint64_t account) {
    std::shared_lock lock(mutex_);
    return sessions_.find(account) != sessions_.end();
}