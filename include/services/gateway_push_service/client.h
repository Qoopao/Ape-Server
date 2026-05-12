#ifndef GATEWAY_PUSH_SERVICE_CLIENT_H
#define GATEWAY_PUSH_SERVICE_CLIENT_H

#include "gateway_push.grpc.pb.h"
#include "gateway_push.pb.h"
#include <grpcpp/channel.h>
#include <memory>
#include <string>

// GatewayPushClient - 调用 Gateway 内部推送服务的客户端
// 由 PushService 使用，通过 gRPC 请求 Gateway 向指定用户推送消息
class GatewayPushClient {
public:
    explicit GatewayPushClient(std::shared_ptr<grpc::Channel> channel);

    // 向指定用户推送消息
    // userID: 目标用户ID
    // msgDataBin: sdkws.MsgData 序列化后的 bytes
    // conversationID: 会话ID（群聊时使用，单聊可选）
    // 返回 true 表示 Gateway 收到请求并成功投递到 WebSocket session
    bool PushToUser(const std::string &userID,
                    const std::string &msgDataBin,
                    const std::string &conversationID);

private:
    std::unique_ptr<gateway_push::GatewayPushService::Stub> stub_;
};

#endif