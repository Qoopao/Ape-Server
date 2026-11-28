#ifndef GROUP_FANOUT_HANDLER_H
#define GROUP_FANOUT_HANDLER_H

#include "messagequeue/message_handler.h"

#include <boost/asio/awaitable.hpp>
#include <string>

class GatewayRouter;

// PushService 的第二条 consumer（group_topic）：
//   payload = traceparent|serverMsgID
//   1. 读 Mongo/Redis 得到 MsgData（不把正文放进 Kafka）
//   2. 读 stream（dismissed/epoch/published_seq）与成员（缓存 miss 回源 SQL）
//   3. 过滤 joinSeq<seq 且 account!=sender
//   4. ≤GROUP_INLINE_MAX_MEMBERS 且字节 ≤GROUP_INLINE_MAX_BYTES：251 内联 MsgData；
//      否则只带 lastSeq/epoch，由客户端 245 拉取
//   5. 按 GROUP_FANOUT_CHUNK 分批 PushBatchBestEffort(251)，汇总后完成
class GroupFanoutHandler : public MessageHandler {
public:
  explicit GroupFanoutHandler(GatewayRouter *router);
  boost::asio::awaitable<void> handle(const std::string topic,
                                      const std::string message) override;

private:
  GatewayRouter *router_;
};

#endif
