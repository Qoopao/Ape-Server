#ifndef MSG_SERVICE_SERVER_H
#define MSG_SERVICE_SERVER_H

#include "msg.grpc.pb.h"
#include "msg.pb.h"
#include "services/base_service.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>
#include <memory>
#include <spdlog/spdlog.h>

class IMessageProducer;

class MsgServiceImpl final : public msg::MessageService::CallbackService,
                             public BaseServiceServer<MsgServiceImpl> {
public:
  MsgServiceImpl(const std::string &service_name,
                 const std::string &listen_address);
  ~MsgServiceImpl() override = default;

  // 依赖注入：消息队列生产者
  void SetProducer(IMessageProducer *producer);

  ::grpc::ServerUnaryReactor *GetMaxSeq(::grpc::CallbackServerContext *context,
                                        const ::sdkws::GetMaxSeqReq *request,
                                        ::sdkws::GetMaxSeqResp *response) override;
  ::grpc::ServerUnaryReactor *GetMaxSeqs(::grpc::CallbackServerContext *context,
                                         const ::msg::GetMaxSeqsReq *request,
                                         ::msg::SeqsInfoResp *response) override;
  ::grpc::ServerUnaryReactor *GetHasReadSeqs(::grpc::CallbackServerContext *context,
                                             const ::msg::GetHasReadSeqsReq *request,
                                             ::msg::SeqsInfoResp *response) override;
  ::grpc::ServerUnaryReactor *GetMsgByConversationIDs(
      ::grpc::CallbackServerContext *context,
      const ::msg::GetMsgByConversationIDsReq *request,
      ::msg::GetMsgByConversationIDsResp *response) override;
  ::grpc::ServerUnaryReactor *
  GetConversationMaxSeq(::grpc::CallbackServerContext *context,
                        const ::msg::GetConversationMaxSeqReq *request,
                        ::msg::GetConversationMaxSeqResp *response) override;
  ::grpc::ServerUnaryReactor *
  PullMessageBySeqs(::grpc::CallbackServerContext *context,
                    const ::sdkws::PullMessageBySeqsReq *request,
                    ::sdkws::PullMessageBySeqsResp *response) override;
  ::grpc::ServerUnaryReactor *GetSeqMessage(::grpc::CallbackServerContext *context,
                                            const ::msg::GetSeqMessageReq *request,
                                            ::msg::GetSeqMessageResp *response) override;
  ::grpc::ServerUnaryReactor *SearchMessage(::grpc::CallbackServerContext *context,
                                            const ::msg::SearchMessageReq *request,
                                            ::msg::SearchMessageResp *response) override;
  ::grpc::ServerUnaryReactor *SendMessages(::grpc::CallbackServerContext *context,
                                           const ::sdkws::SendMessageReq *request,
                                           ::sdkws::SendMessageResp *response) override;
  ::grpc::ServerUnaryReactor *SendSimpleMsg(::grpc::CallbackServerContext *context,
                                            const ::msg::SendSimpleMsgReq *request,
                                            ::msg::SendSimpleMsgResp *response) override;
  ::grpc::ServerUnaryReactor *SetUserConversationsMinSeq(
      ::grpc::CallbackServerContext *context,
      const ::msg::SetUserConversationsMinSeqReq *request,
      ::msg::SetUserConversationsMinSeqResp *response) override;
  ::grpc::ServerUnaryReactor *
  ClearConversationsMsg(::grpc::CallbackServerContext *context,
                        const ::msg::ClearConversationsMsgReq *request,
                        ::msg::ClearConversationsMsgResp *response) override;
  ::grpc::ServerUnaryReactor *UserClearAllMsg(::grpc::CallbackServerContext *context,
                                              const ::msg::UserClearAllMsgReq *request,
                                              ::msg::UserClearAllMsgResp *response) override;
  ::grpc::ServerUnaryReactor *DeleteMsgs(::grpc::CallbackServerContext *context,
                                         const ::msg::DeleteMsgsReq *request,
                                         ::msg::DeleteMsgsResp *response) override;
  ::grpc::ServerUnaryReactor *
  DeleteMsgPhysicalBySeq(::grpc::CallbackServerContext *context,
                         const ::msg::DeleteMsgPhysicalBySeqReq *request,
                         ::msg::DeleteMsgPhysicalBySeqResp *response) override;
  ::grpc::ServerUnaryReactor *
  DeleteMsgPhysical(::grpc::CallbackServerContext *context,
                    const ::msg::DeleteMsgPhysicalReq *request,
                    ::msg::DeleteMsgPhysicalResp *response) override;
  ::grpc::ServerUnaryReactor *
  SetSendMsgStatus(::grpc::CallbackServerContext *context,
                   const ::msg::SetSendMsgStatusReq *request,
                   ::msg::SetSendMsgStatusResp *response) override;
  ::grpc::ServerUnaryReactor *
  GetSendMsgStatus(::grpc::CallbackServerContext *context,
                   const ::msg::GetSendMsgStatusReq *request,
                   ::msg::GetSendMsgStatusResp *response) override;
  ::grpc::ServerUnaryReactor *RevokeMsg(::grpc::CallbackServerContext *context,
                                        const ::msg::RevokeMsgReq *request,
                                        ::msg::RevokeMsgResp *response) override;
  ::grpc::ServerUnaryReactor *MarkMsgsAsRead(::grpc::CallbackServerContext *context,
                                             const ::msg::MarkMsgsAsReadReq *request,
                                             ::msg::MarkMsgsAsReadResp *response) override;
  ::grpc::ServerUnaryReactor *
  MarkConversationAsRead(::grpc::CallbackServerContext *context,
                         const ::msg::MarkConversationAsReadReq *request,
                         ::msg::MarkConversationAsReadResp *response) override;
  ::grpc::ServerUnaryReactor *SetConversationHasReadSeq(
      ::grpc::CallbackServerContext *context,
      const ::msg::SetConversationHasReadSeqReq *request,
      ::msg::SetConversationHasReadSeqResp *response) override;
  ::grpc::ServerUnaryReactor *GetConversationsHasReadAndMaxSeq(
      ::grpc::CallbackServerContext *context,
      const ::msg::GetConversationsHasReadAndMaxSeqReq *request,
      ::msg::GetConversationsHasReadAndMaxSeqResp *response) override;
  ::grpc::ServerUnaryReactor *GetActiveUser(::grpc::CallbackServerContext *context,
                                            const ::msg::GetActiveUserReq *request,
                                            ::msg::GetActiveUserResp *response) override;
  ::grpc::ServerUnaryReactor *GetActiveGroup(::grpc::CallbackServerContext *context,
                                             const ::msg::GetActiveGroupReq *request,
                                             ::msg::GetActiveGroupResp *response) override;
  ::grpc::ServerUnaryReactor *GetServerTime(::grpc::CallbackServerContext *context,
                                            const ::msg::GetServerTimeReq *request,
                                            ::msg::GetServerTimeResp *response) override;
  ::grpc::ServerUnaryReactor *ClearMsg(::grpc::CallbackServerContext *context,
                                       const ::msg::ClearMsgReq *request,
                                       ::msg::ClearMsgResp *response) override;
  ::grpc::ServerUnaryReactor *DestructMsgs(::grpc::CallbackServerContext *context,
                                           const ::msg::DestructMsgsReq *request,
                                           ::msg::DestructMsgsResp *response) override;
  ::grpc::ServerUnaryReactor *
  GetActiveConversation(::grpc::CallbackServerContext *context,
                        const ::msg::GetActiveConversationReq *request,
                        ::msg::GetActiveConversationResp *response) override;
  ::grpc::ServerUnaryReactor *SetUserConversationMaxSeq(
      ::grpc::CallbackServerContext *context,
      const ::msg::SetUserConversationMaxSeqReq *request,
      ::msg::SetUserConversationMaxSeqResp *response) override;
  ::grpc::ServerUnaryReactor *SetUserConversationMinSeq(
      ::grpc::CallbackServerContext *context,
      const ::msg::SetUserConversationMinSeqReq *request,
      ::msg::SetUserConversationMinSeqResp *response) override;
  ::grpc::ServerUnaryReactor *GetLastMessageSeqByTime(
      ::grpc::CallbackServerContext *context,
      const ::msg::GetLastMessageSeqByTimeReq *request,
      ::msg::GetLastMessageSeqByTimeResp *response) override;
  ::grpc::ServerUnaryReactor *GetLastMessage(::grpc::CallbackServerContext *context,
                                             const ::msg::GetLastMessageReq *request,
                                             ::msg::GetLastMessageResp *response) override;

private:
  IMessageProducer *producer_{nullptr};
};

#endif
