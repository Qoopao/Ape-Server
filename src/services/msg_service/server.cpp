#include "services/msg_service/server.h"

::grpc::Status MsgServiceImpl::SendMessages(::grpc::ServerContext *context,
                              const ::sdkws::SendMessageReq *request,
                              ::sdkws::SendMessageResp *response){
    // 检查非空
    if(request->msgs_size() == 0){
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "msgs is empty");
    }

    //
    return ::grpc::Status::OK;
}



