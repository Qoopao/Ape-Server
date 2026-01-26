#include "msg.grpc.pb.h"
#include "msg.pb.h"
#include "services/register/etcd_service_register.h"
#include "services/register/base_register_service.h"
#include <csignal>
#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>
#include <memory>
#include <spdlog/spdlog.h>


class MsgServiceImpl final : public msg::MessageService::Service, public BaseRegisterService {
public:
  MsgServiceImpl() {
    // 信号处理，ctrl+c退出
    struct sigaction sa;
    sa.sa_handler = &MsgServiceImpl::SignalHandler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
  };

  void setRegistry(std::unique_ptr<EtcdServiceRegistry> registry) {
    this->etcd_service_registry = std::move(registry);
  }


  void registerGrpcService(grpc::ServerBuilder &builder) override {
    builder.RegisterService(this);
  }

  void independentWait() override {
    std::unique_lock<std::mutex> lock(this->mtx_quit);
    this->cv_quit.wait(lock, [this]() {
      return this->should_exit.load(std::memory_order_acquire);
    });
  }

  ::grpc::Status GetMaxSeq(::grpc::ServerContext *context,
                           const ::sdkws::GetMaxSeqReq *request,
                           ::sdkws::GetMaxSeqResp *response) override;
  ::grpc::Status GetMaxSeqs(::grpc::ServerContext *context,
                            const ::msg::GetMaxSeqsReq *request,
                            ::msg::SeqsInfoResp *response) override;
  ::grpc::Status GetHasReadSeqs(::grpc::ServerContext *context,
                                const ::msg::GetHasReadSeqsReq *request,
                                ::msg::SeqsInfoResp *response) override;
  ::grpc::Status GetMsgByConversationIDs(
      ::grpc::ServerContext *context,
      const ::msg::GetMsgByConversationIDsReq *request,
      ::msg::GetMsgByConversationIDsResp *response) override;
  ::grpc::Status
  GetConversationMaxSeq(::grpc::ServerContext *context,
                        const ::msg::GetConversationMaxSeqReq *request,
                        ::msg::GetConversationMaxSeqResp *response) override;
  ::grpc::Status
  PullMessageBySeqs(::grpc::ServerContext *context,
                    const ::sdkws::PullMessageBySeqsReq *request,
                    ::sdkws::PullMessageBySeqsResp *response) override;
  ::grpc::Status GetSeqMessage(::grpc::ServerContext *context,
                               const ::msg::GetSeqMessageReq *request,
                               ::msg::GetSeqMessageResp *response) override;
  ::grpc::Status SearchMessage(::grpc::ServerContext *context,
                               const ::msg::SearchMessageReq *request,
                               ::msg::SearchMessageResp *response) override;
  ::grpc::Status SendMessages(::grpc::ServerContext *context,
                              const ::sdkws::SendMessageReq *request,
                              ::sdkws::SendMessageResp *response) override;
  ::grpc::Status SendSimpleMsg(::grpc::ServerContext *context,
                               const ::msg::SendSimpleMsgReq *request,
                               ::msg::SendSimpleMsgResp *response) override;
  ::grpc::Status SetUserConversationsMinSeq(
      ::grpc::ServerContext *context,
      const ::msg::SetUserConversationsMinSeqReq *request,
      ::msg::SetUserConversationsMinSeqResp *response) override;
  ::grpc::Status
  ClearConversationsMsg(::grpc::ServerContext *context,
                        const ::msg::ClearConversationsMsgReq *request,
                        ::msg::ClearConversationsMsgResp *response) override;
  ::grpc::Status UserClearAllMsg(::grpc::ServerContext *context,
                                 const ::msg::UserClearAllMsgReq *request,
                                 ::msg::UserClearAllMsgResp *response) override;
  ::grpc::Status DeleteMsgs(::grpc::ServerContext *context,
                            const ::msg::DeleteMsgsReq *request,
                            ::msg::DeleteMsgsResp *response) override;
  ::grpc::Status
  DeleteMsgPhysicalBySeq(::grpc::ServerContext *context,
                         const ::msg::DeleteMsgPhysicalBySeqReq *request,
                         ::msg::DeleteMsgPhysicalBySeqResp *response) override;
  ::grpc::Status
  DeleteMsgPhysical(::grpc::ServerContext *context,
                    const ::msg::DeleteMsgPhysicalReq *request,
                    ::msg::DeleteMsgPhysicalResp *response) override;
  ::grpc::Status
  SetSendMsgStatus(::grpc::ServerContext *context,
                   const ::msg::SetSendMsgStatusReq *request,
                   ::msg::SetSendMsgStatusResp *response) override;
  ::grpc::Status
  GetSendMsgStatus(::grpc::ServerContext *context,
                   const ::msg::GetSendMsgStatusReq *request,
                   ::msg::GetSendMsgStatusResp *response) override;
  ::grpc::Status RevokeMsg(::grpc::ServerContext *context,
                           const ::msg::RevokeMsgReq *request,
                           ::msg::RevokeMsgResp *response) override;
  ::grpc::Status MarkMsgsAsRead(::grpc::ServerContext *context,
                                const ::msg::MarkMsgsAsReadReq *request,
                                ::msg::MarkMsgsAsReadResp *response) override;
  ::grpc::Status
  MarkConversationAsRead(::grpc::ServerContext *context,
                         const ::msg::MarkConversationAsReadReq *request,
                         ::msg::MarkConversationAsReadResp *response) override;
  ::grpc::Status SetConversationHasReadSeq(
      ::grpc::ServerContext *context,
      const ::msg::SetConversationHasReadSeqReq *request,
      ::msg::SetConversationHasReadSeqResp *response) override;
  ::grpc::Status GetConversationsHasReadAndMaxSeq(
      ::grpc::ServerContext *context,
      const ::msg::GetConversationsHasReadAndMaxSeqReq *request,
      ::msg::GetConversationsHasReadAndMaxSeqResp *response) override;
  ::grpc::Status GetActiveUser(::grpc::ServerContext *context,
                               const ::msg::GetActiveUserReq *request,
                               ::msg::GetActiveUserResp *response) override;
  ::grpc::Status GetActiveGroup(::grpc::ServerContext *context,
                                const ::msg::GetActiveGroupReq *request,
                                ::msg::GetActiveGroupResp *response) override;
  ::grpc::Status GetServerTime(::grpc::ServerContext *context,
                               const ::msg::GetServerTimeReq *request,
                               ::msg::GetServerTimeResp *response) override;
  ::grpc::Status ClearMsg(::grpc::ServerContext *context,
                          const ::msg::ClearMsgReq *request,
                          ::msg::ClearMsgResp *response) override;
  ::grpc::Status DestructMsgs(::grpc::ServerContext *context,
                              const ::msg::DestructMsgsReq *request,
                              ::msg::DestructMsgsResp *response) override;
  ::grpc::Status
  GetActiveConversation(::grpc::ServerContext *context,
                        const ::msg::GetActiveConversationReq *request,
                        ::msg::GetActiveConversationResp *response) override;
  ::grpc::Status SetUserConversationMaxSeq(
      ::grpc::ServerContext *context,
      const ::msg::SetUserConversationMaxSeqReq *request,
      ::msg::SetUserConversationMaxSeqResp *response) override;
  ::grpc::Status SetUserConversationMinSeq(
      ::grpc::ServerContext *context,
      const ::msg::SetUserConversationMinSeqReq *request,
      ::msg::SetUserConversationMinSeqResp *response) override;
  ::grpc::Status GetLastMessageSeqByTime(
      ::grpc::ServerContext *context,
      const ::msg::GetLastMessageSeqByTimeReq *request,
      ::msg::GetLastMessageSeqByTimeResp *response) override;
  ::grpc::Status GetLastMessage(::grpc::ServerContext *context,
                                const ::msg::GetLastMessageReq *request,
                                ::msg::GetLastMessageResp *response) override;

private:
  std::unique_ptr<EtcdServiceRegistry> etcd_service_registry;
  std::unique_ptr<grpc::Server> server;

// 测试用
  static std::atomic<bool> should_exit;
  static std::condition_variable cv_quit;
  static std::mutex mtx_quit;
  static void SignalHandler(int sig) {
     std::lock_guard<std::mutex> lock(mtx_quit);
     should_exit.store(true, std::memory_order_release);
     cv_quit.notify_one();
  }
};

