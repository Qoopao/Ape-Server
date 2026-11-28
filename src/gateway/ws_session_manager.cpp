#include "gateway/ws_session_manager.h"
#include "om/otel_metrics.h"

#include <cstdint>
#include <cstdlib>
#include <spdlog/spdlog.h>

WSSessionManager& WSSessionManager::instance() {
    static WSSessionManager mgr;
    return mgr;
}

std::vector<uint64_t> WSSessionManager::onlineAccounts() {
    std::shared_lock lock(mutex_);
    std::vector<uint64_t> accounts;
    for (const auto& entry : sessions_) accounts.push_back(entry.first);
    return accounts;
}

void WSSessionManager::registerSession(const uint64_t account,
                                       std::shared_ptr<WSSession> session) {
    std::shared_ptr<WSSession> replaced;
    uint64_t total = 0;
    bool isReplacing = false;
    {
        std::unique_lock lock(mutex_);
        auto it = sessions_.find(account);
        isReplacing = it != sessions_.end();
        // 同实例重复登录：先摘掉旧连接，再踢（INV-4）
        if (isReplacing && it->second != session)
            replaced = it->second;
        sessions_[account] = session;
        total = sessions_.size();
    }

    if (!isReplacing) {
        ape::otel::WsConnectionsActive().Add(1);
    }
    if (replaced) {
        spdlog::info("WSSessionManager: replacing session for user={}", account);
        replaced->kick("duplicate_login");
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

bool WSSessionManager::kickUser(const uint64_t account, const std::string &reason) {
    std::shared_ptr<WSSession> session;
    {
        std::shared_lock lock(mutex_);
        auto it = sessions_.find(account);
        if (it == sessions_.end())
            return false;
        session = it->second;
    }
    session->kick(reason);
    return true;
}

void WSSessionManager::notifyDraining() {
    draining_.store(true);
    std::vector<std::shared_ptr<WSSession>> sessions;
    {
        std::shared_lock lock(mutex_);
        sessions.reserve(sessions_.size());
        for (const auto &entry : sessions_)
            sessions.push_back(entry.second);
    }
    for (const auto &session : sessions)
        session->notifyDraining();
    spdlog::info("WSSessionManager: draining notified to {} sessions", sessions.size());
}

size_t WSSessionManager::connectionCount() const {
    std::shared_lock lock(mutex_);
    return sessions_.size();
}

void WSSessionManager::closeAll() {
    std::vector<std::shared_ptr<WSSession>> sessions;
    {
        std::shared_lock lock(mutex_);
        sessions.reserve(sessions_.size());
        for (const auto &entry : sessions_)
            sessions.push_back(entry.second);
    }
    for (const auto &session : sessions)
        session->closeNow();
    spdlog::info("WSSessionManager: closing {} sessions", sessions.size());
}

size_t WSSessionManager::MaxConnections() {
    const char *value = std::getenv("GATEWAY_MAX_CONNECTIONS");
    if (value == nullptr)
        return 20000;
    try {
        return static_cast<size_t>(std::stoull(value));
    } catch (const std::exception &) {
        return 20000;
    }
}
