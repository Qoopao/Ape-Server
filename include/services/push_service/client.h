#ifndef PUSH_SERVICE_CLIENT_H
#define PUSH_SERVICE_CLIENT_H

#include "push.grpc.pb.h"
#include "push.pb.h"
#include <grpcpp/channel.h>
#include <memory>
#include <string>

class PushClient {
public:
    PushClient(std::shared_ptr<grpc::Channel> channel)
        : stub_(push::PushService::NewStub(channel)) {}

    // 推送消息到指定用户（在线用户走长连接推送，离线用户走APNs/FCM）
    ::push::PushMsgResp PushMsg(const ::push::PushMsgReq& request);

    // 删除用户推送token（用户登出或取消推送时调用）
    ::push::DelUserPushTokenResp DelUserPushToken(const ::push::DelUserPushTokenReq& request);

    // 消息确认（在线消息/离线消息统一ACK入口）
    ::push::AckMsgResp AckMsg(const ::push::AckMsgReq& request);

    // 预登记离线消息待ACK项（用户上线拉取离线消息后调用）
    ::push::AddPendingOfflineAckResp AddPendingOfflineAck(const ::push::AddPendingOfflineAckReq& request);

private:
    std::unique_ptr<push::PushService::Stub> stub_;
};

#endif