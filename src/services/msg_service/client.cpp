#include "services/msg_service/client.h"
#include "util/grpc_async_util.h"
#include "om/otel_trace_propagation.h"
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>

boost::asio::awaitable<::sdkws::GetMaxSeqResp> MsgClient::GetMaxSeq(const ::sdkws::GetMaxSeqReq& request) {
  ::sdkws::GetMaxSeqResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetMaxSeq(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("GetMaxSeq failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::SeqsInfoResp> MsgClient::GetMaxSeqs(const ::msg::GetMaxSeqsReq& request) {
  ::msg::SeqsInfoResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetMaxSeqs(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("GetMaxSeqs failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::SeqsInfoResp> MsgClient::GetHasReadSeqs(const ::msg::GetHasReadSeqsReq& request) {
  ::msg::SeqsInfoResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetHasReadSeqs(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("GetHasReadSeqs failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::GetMsgByConversationIDsResp> MsgClient::GetMsgByConversationIDs(const ::msg::GetMsgByConversationIDsReq& request) {
  ::msg::GetMsgByConversationIDsResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetMsgByConversationIDs(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("GetMsgByConversationIDs failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::GetConversationMaxSeqResp> MsgClient::GetConversationMaxSeq(const ::msg::GetConversationMaxSeqReq& request) {
  ::msg::GetConversationMaxSeqResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetConversationMaxSeq(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("GetConversationMaxSeq failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::sdkws::PullMessageBySeqsResp> MsgClient::PullMessageBySeqs(const ::sdkws::PullMessageBySeqsReq& request) {
  ::sdkws::PullMessageBySeqsResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->PullMessageBySeqs(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("PullMessageBySeqs failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::GetSeqMessageResp> MsgClient::GetSeqMessage(const ::msg::GetSeqMessageReq& request) {
  ::msg::GetSeqMessageResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetSeqMessage(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("GetSeqMessage failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::SearchMessageResp> MsgClient::SearchMessage(const ::msg::SearchMessageReq& request) {
  ::msg::SearchMessageResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->SearchMessage(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("SearchMessage failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

// SendMessages — 需要注入 traceparent
boost::asio::awaitable<::sdkws::SendMessageResp> MsgClient::SendMessages(const ::sdkws::SendMessageReq& request) {
  ::sdkws::SendMessageResp reply;
  grpc::ClientContext context;
  ape::otel::InjectTraceContextToGrpcMetadata(context);

  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->SendMessages(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("SendMessages failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::SendSimpleMsgResp> MsgClient::SendSimpleMsg(const ::msg::SendSimpleMsgReq& request) {
  ::msg::SendSimpleMsgResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->SendSimpleMsg(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("SendSimpleMsg failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::SetUserConversationsMinSeqResp> MsgClient::SetUserConversationsMinSeq(const ::msg::SetUserConversationsMinSeqReq& request) {
  ::msg::SetUserConversationsMinSeqResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->SetUserConversationsMinSeq(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("SetUserConversationsMinSeq failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::ClearConversationsMsgResp> MsgClient::ClearConversationsMsg(const ::msg::ClearConversationsMsgReq& request) {
  ::msg::ClearConversationsMsgResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->ClearConversationsMsg(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("ClearConversationsMsg failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::UserClearAllMsgResp> MsgClient::UserClearAllMsg(const ::msg::UserClearAllMsgReq& request) {
  ::msg::UserClearAllMsgResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->UserClearAllMsg(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("UserClearAllMsg failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::DeleteMsgsResp> MsgClient::DeleteMsgs(const ::msg::DeleteMsgsReq& request) {
  ::msg::DeleteMsgsResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->DeleteMsgs(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("DeleteMsgs failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::DeleteMsgPhysicalBySeqResp> MsgClient::DeleteMsgPhysicalBySeq(const ::msg::DeleteMsgPhysicalBySeqReq& request) {
  ::msg::DeleteMsgPhysicalBySeqResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->DeleteMsgPhysicalBySeq(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("DeleteMsgPhysicalBySeq failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::DeleteMsgPhysicalResp> MsgClient::DeleteMsgPhysical(const ::msg::DeleteMsgPhysicalReq& request) {
  ::msg::DeleteMsgPhysicalResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->DeleteMsgPhysical(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("DeleteMsgPhysical failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::SetSendMsgStatusResp> MsgClient::SetSendMsgStatus(const ::msg::SetSendMsgStatusReq& request) {
  ::msg::SetSendMsgStatusResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->SetSendMsgStatus(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("SetSendMsgStatus failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::GetSendMsgStatusResp> MsgClient::GetSendMsgStatus(const ::msg::GetSendMsgStatusReq& request) {
  ::msg::GetSendMsgStatusResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetSendMsgStatus(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("GetSendMsgStatus failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::RevokeMsgResp> MsgClient::RevokeMsg(const ::msg::RevokeMsgReq& request) {
  ::msg::RevokeMsgResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->RevokeMsg(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("RevokeMsg failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::MarkMsgsAsReadResp> MsgClient::MarkMsgsAsRead(const ::msg::MarkMsgsAsReadReq& request) {
  ::msg::MarkMsgsAsReadResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->MarkMsgsAsRead(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("MarkMsgsAsRead failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::MarkConversationAsReadResp> MsgClient::MarkConversationAsRead(const ::msg::MarkConversationAsReadReq& request) {
  ::msg::MarkConversationAsReadResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->MarkConversationAsRead(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("MarkConversationAsRead failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::SetConversationHasReadSeqResp> MsgClient::SetConversationHasReadSeq(const ::msg::SetConversationHasReadSeqReq& request) {
  ::msg::SetConversationHasReadSeqResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->SetConversationHasReadSeq(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("SetConversationHasReadSeq failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::GetConversationsHasReadAndMaxSeqResp> MsgClient::GetConversationsHasReadAndMaxSeq(const ::msg::GetConversationsHasReadAndMaxSeqReq& request) {
  ::msg::GetConversationsHasReadAndMaxSeqResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetConversationsHasReadAndMaxSeq(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("GetConversationsHasReadAndMaxSeq failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::GetActiveUserResp> MsgClient::GetActiveUser(const ::msg::GetActiveUserReq& request) {
  ::msg::GetActiveUserResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetActiveUser(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("GetActiveUser failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::GetActiveGroupResp> MsgClient::GetActiveGroup(const ::msg::GetActiveGroupReq& request) {
  ::msg::GetActiveGroupResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetActiveGroup(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("GetActiveGroup failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::GetServerTimeResp> MsgClient::GetServerTime(const ::msg::GetServerTimeReq& request) {
  ::msg::GetServerTimeResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetServerTime(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("GetServerTime failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::ClearMsgResp> MsgClient::ClearMsg(const ::msg::ClearMsgReq& request) {
  ::msg::ClearMsgResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->ClearMsg(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("ClearMsg failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::DestructMsgsResp> MsgClient::DestructMsgs(const ::msg::DestructMsgsReq& request) {
  ::msg::DestructMsgsResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->DestructMsgs(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("DestructMsgs failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::GetActiveConversationResp> MsgClient::GetActiveConversation(const ::msg::GetActiveConversationReq& request) {
  ::msg::GetActiveConversationResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetActiveConversation(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("GetActiveConversation failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::SetUserConversationMaxSeqResp> MsgClient::SetUserConversationMaxSeq(const ::msg::SetUserConversationMaxSeqReq& request) {
  ::msg::SetUserConversationMaxSeqResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->SetUserConversationMaxSeq(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("SetUserConversationMaxSeq failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::SetUserConversationMinSeqResp> MsgClient::SetUserConversationMinSeq(const ::msg::SetUserConversationMinSeqReq& request) {
  ::msg::SetUserConversationMinSeqResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->SetUserConversationMinSeq(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("SetUserConversationMinSeq failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::GetLastMessageSeqByTimeResp> MsgClient::GetLastMessageSeqByTime(const ::msg::GetLastMessageSeqByTimeReq& request) {
  ::msg::GetLastMessageSeqByTimeResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetLastMessageSeqByTime(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("GetLastMessageSeqByTime failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::msg::GetLastMessageResp> MsgClient::GetLastMessage(const ::msg::GetLastMessageReq& request) {
  ::msg::GetLastMessageResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetLastMessage(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("GetLastMessage failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}