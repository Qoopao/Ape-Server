#pragma once
#include "sdkws.pb.h"
#include <cstdint>

namespace message_validation {
// Pure envelope validation. Friendship, deletion boundaries and ACK receipts
// are checked by MySQLHandler; this function never accesses storage.
inline bool IsSingleMessageFor(uint64_t owner, const sdkws::MsgData& message) {
    return owner && !message.isgroupmsg() && message.sessiontype()==1 &&
        message.recvid()==owner && message.sendid() && !message.servermsgid().empty();
}
}
