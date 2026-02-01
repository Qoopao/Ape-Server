#ifndef MSG_SERVICE_CLIENT_H
#define MSG_SERVICE_CLIENT_H

#include "msg.grpc.pb.h"
#include "msg.pb.h"
#include "services/register_discovery/rpc_call_manager.h"
#include <atomic>
#include <cmath>
#include <google/protobuf/empty.pb.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <memory>

class MsgClientHandler {
public:
  MsgClientHandler(const std::string &service_name,
                   const std::string &etcd_endpoint = "http://127.0.0.1:2379")
      : rpc_call_mgr_(
            std::make_shared<RpcCallManager>(service_name, etcd_endpoint)) {
    RefreshStub();
  };

  void Destroy() {
    std::lock_guard<std::mutex> lock(rpc_mutex_);
    is_destroyed_.store(true);

    if (stub) {
      stub.reset();
    }
    if (current_channel_) {
      current_channel_.reset();
    }
    current_ip_port_.clear();
    rpc_call_mgr_.reset();
  }

  ~MsgClientHandler() { Destroy(); };

  void RefreshStub() {
    std::lock_guard<std::mutex> lock(rpc_mutex_); // 加锁
    if (is_destroyed_.load()) {                   // 析构检查
      spdlog::warn("对象已析构，无法刷新Stub");
      return;
    }
    // 初始化stub
    auto [availableChannel, ip_port] =
        rpc_call_mgr_->GetAvailableChannelWithIpPort();
    if (availableChannel && !ip_port.empty()) {
      stub = std::make_unique<::msg::MessageService::Stub>(availableChannel);
      current_channel_ = availableChannel;
      current_ip_port_ = ip_port;
      spdlog::info("MsgClientHandler Stub已刷新, 当前节点: {}", ip_port);
    } else {
      spdlog::error("初始化stub时未发现服务可用节点");
      stub.reset();
      current_channel_.reset();
      current_ip_port_.clear();
    }
  }

  void GetMaxSeqClient(const ::sdkws::GetMaxSeqReq &request,
                       ::sdkws::GetMaxSeqResp &response);
  void GetMaxSeqsClient(const ::msg::GetMaxSeqsReq &request,
                        ::msg::SeqsInfoResp &response);
  void GetHasReadSeqsClient(const ::msg::GetHasReadSeqsReq &request,
                            ::msg::SeqsInfoResp &response);
  void GetMsgByConversationIDsClient(
      const ::msg::GetMsgByConversationIDsReq &request,
      ::msg::GetMsgByConversationIDsResp &response);
  void
  GetConversationMaxSeqClient(const ::msg::GetConversationMaxSeqReq &request,
                              ::msg::GetConversationMaxSeqResp &response);
  void PullMessageBySeqsClient(const ::sdkws::PullMessageBySeqsReq &request,
                               ::sdkws::PullMessageBySeqsResp &response);
  void GetSeqMessageClient(const ::msg::GetSeqMessageReq &request,
                           ::msg::GetSeqMessageResp &response);
  void SearchMessageClient(const ::msg::SearchMessageReq &request,
                           ::msg::SearchMessageResp &response);
  void SendMessagesClient(const ::sdkws::SendMessageReq &request,
                          ::sdkws::SendMessageResp &response);
  void SendSimpleMsgClient(const ::msg::SendSimpleMsgReq &request,
                           ::msg::SendSimpleMsgResp &response);
  void SetUserConversationsMinSeqClient(
      const ::msg::SetUserConversationsMinSeqReq &request,
      ::msg::SetUserConversationsMinSeqResp &response);
  void
  ClearConversationsMsgClient(const ::msg::ClearConversationsMsgReq &request,
                              ::msg::ClearConversationsMsgResp &response);
  void UserClearAllMsgClient(const ::msg::UserClearAllMsgReq &request,
                             ::msg::UserClearAllMsgResp &response);
  void DeleteMsgsClient(const ::msg::DeleteMsgsReq &request,
                        ::msg::DeleteMsgsResp &response);
  void
  DeleteMsgPhysicalBySeqClient(const ::msg::DeleteMsgPhysicalBySeqReq &request,
                               ::msg::DeleteMsgPhysicalBySeqResp &response);
  void DeleteMsgPhysicalClient(const ::msg::DeleteMsgPhysicalReq &request,
                               ::msg::DeleteMsgPhysicalResp &response);
  void SetSendMsgStatusClient(const ::msg::SetSendMsgStatusReq &request,
                              ::msg::SetSendMsgStatusResp &response);
  void GetSendMsgStatusClient(const ::msg::GetSendMsgStatusReq &request,
                              ::msg::GetSendMsgStatusResp &response);
  void RevokeMsgClient(const ::msg::RevokeMsgReq &request,
                       ::msg::RevokeMsgResp &response);
  void MarkMsgsAsReadClient(const ::msg::MarkMsgsAsReadReq &request,
                            ::msg::MarkMsgsAsReadResp &response);
  void
  MarkConversationAsReadClient(const ::msg::MarkConversationAsReadReq &request,
                               ::msg::MarkConversationAsReadResp &response);
  void SetConversationHasReadSeqClient(
      const ::msg::SetConversationHasReadSeqReq &request,
      ::msg::SetConversationHasReadSeqResp &response);
  void GetConversationsHasReadAndMaxSeqClient(
      const ::msg::GetConversationsHasReadAndMaxSeqReq &request,
      ::msg::GetConversationsHasReadAndMaxSeqResp &response);
  void GetActiveUserClient(const ::msg::GetActiveUserReq &request,
                           ::msg::GetActiveUserResp &response);
  void GetActiveGroupClient(const ::msg::GetActiveGroupReq &request,
                            ::msg::GetActiveGroupResp &response);
  void GetServerTimeClient(const ::msg::GetServerTimeReq &request,
                           ::msg::GetServerTimeResp &response);
  void ClearMsgClient(const ::msg::ClearMsgReq &request,
                      ::msg::ClearMsgResp &response);
  void DestructMsgsClient(const ::msg::DestructMsgsReq &request,
                          ::msg::DestructMsgsResp &response);
  void
  GetActiveConversationClient(const ::msg::GetActiveConversationReq &request,
                              ::msg::GetActiveConversationResp &response);
  void SetUserConversationMaxSeqClient(
      const ::msg::SetUserConversationMaxSeqReq &request,
      ::msg::SetUserConversationMaxSeqResp &response);
  void SetUserConversationMinSeqClient(
      const ::msg::SetUserConversationMinSeqReq &request,
      ::msg::SetUserConversationMinSeqResp &response);
  void GetLastMessageSeqByTimeClient(
      const ::msg::GetLastMessageSeqByTimeReq &request,
      ::msg::GetLastMessageSeqByTimeResp &response);
  void GetLastMessageClient(const ::msg::GetLastMessageReq &request,
                            ::msg::GetLastMessageResp &response);

private:
  std::shared_ptr<RpcCallManager> rpc_call_mgr_;     // 连接管理器
  std::unique_ptr<::msg::MessageService::Stub> stub; // gRPC Stub
  std::shared_ptr<grpc::Channel> current_channel_;   // 当前channel
  std::string current_ip_port_;                      // 当前ip_port
  std::mutex rpc_mutex_;                  // 保护stub和channel的互斥锁
  std::atomic<bool> is_destroyed_{false}; // 是否已销毁

  // 带重试的RPC调用
  template <typename Request, typename Response, typename Func>
  bool CallWithRetry(const Request &request, Response &response, Func &&func,
                     int max_attempts = 3) {

    if (is_destroyed_.load(std::memory_order_acquire)) {
      spdlog::error("调用CallWithRetry失败，对象已销毁");
      return false;
    }

    std::lock_guard<std::mutex> lock(rpc_mutex_);
    if (!stub || !current_channel_) {
      spdlog::error("RPC调用失败：无可用Stub/Channel");
      return false;
    }

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
      if (is_destroyed_.load()) {
        spdlog::error("RPC重试中对象已销毁，终止重试");
        return false;
      }

      grpc::ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() +
                           std::chrono::seconds(5));

      auto status = func(&context, request, &response);
      if (status.ok()) {
        return true;
      }

      // 不重试的错误
      if (status.error_code() == grpc::StatusCode::INVALID_ARGUMENT ||
          status.error_code() == grpc::StatusCode::PERMISSION_DENIED) {
        spdlog::error("RPC调用非重试错误: {}({}), retry: {}",
                      static_cast<int>(status.error_code()),
                      status.error_message(), attempt);
        return false;
      }

      // 需要重试的错误(节点故障)
      spdlog::warn("RPC调用失败: {}({}), retry: {}",
                   static_cast<int>(status.error_code()),
                   status.error_message(), attempt);
      rpc_call_mgr_->MarkNodeFault(current_ip_port_);
      RefreshStub();

      if (attempt == max_attempts - 1) {
        spdlog::error("RPC调用失败: {}({}), 已重试{}次",
                      static_cast<int>(status.error_code()),
                      status.error_message(), max_attempts);
        return false;
      } else {
        // 指数退避
        int sleep_ms = 100 * (1 << (attempt + 1)); // 1<<n = 2^n
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
      }
    }
    return false;
  }
};

#endif