#ifndef FRIEND_SERVICE_SERVER_H
#define FRIEND_SERVICE_SERVER_H

#include "friend.grpc.pb.h"
#include "services/base_service.h"

class FriendServiceImpl final : public friendpb::FriendService::CallbackService,
                                public BaseServiceServer<FriendServiceImpl>
{
public:
    FriendServiceImpl(const std::string& name, const std::string& address);
    grpc::ServerUnaryReactor* Execute(grpc::CallbackServerContext* context,
        const friendpb::Command* request, friendpb::Reply* response) override;
};

#endif
