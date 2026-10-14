#pragma once
#include "sdkws.pb.h"
#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Realtime dispatch body cache; Redis is not the offline-recovery authority.
class RedisHandler {
public:
    static boost::asio::awaitable<bool> SaveMsg(const sdkws::MsgData& message);
    static boost::asio::awaitable<std::optional<sdkws::MsgData>> GetMsg(const std::string& id);
    static boost::asio::awaitable<void> DeleteMsgCache(const std::string& id);
    static boost::asio::awaitable<void> CleanupAcknowledgedMessage(uint64_t owner,const std::string& id);
    static boost::asio::awaitable<void> DeleteSingleChatHistory(uint64_t first,uint64_t second,std::vector<std::string> ids);
};
