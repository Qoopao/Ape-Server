#include "services/msg_service/client.h"
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>

::sdkws::GetMaxSeqResp MsgClient::GetMaxSeq(const ::sdkws::GetMaxSeqReq& request) {
  ::sdkws::GetMaxSeqResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetMaxSeq(&context, &request, &reply,
                            [&mu, &cv, &done, &status](grpc::Status s) {
                              status = std::move(s);
                              std::lock_guard<std::mutex> lock(mu);
                              done = true;
                              cv.notify_one();
                            });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("GetMaxSeq failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::SeqsInfoResp MsgClient::GetMaxSeqs(const ::msg::GetMaxSeqsReq& request) {
  ::msg::SeqsInfoResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetMaxSeqs(&context, &request, &reply,
                             [&mu, &cv, &done, &status](grpc::Status s) {
                               status = std::move(s);
                               std::lock_guard<std::mutex> lock(mu);
                               done = true;
                               cv.notify_one();
                             });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("GetMaxSeqs failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::SeqsInfoResp MsgClient::GetHasReadSeqs(const ::msg::GetHasReadSeqsReq& request) {
  ::msg::SeqsInfoResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetHasReadSeqs(&context, &request, &reply,
                                 [&mu, &cv, &done, &status](grpc::Status s) {
                                   status = std::move(s);
                                   std::lock_guard<std::mutex> lock(mu);
                                   done = true;
                                   cv.notify_one();
                                 });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("GetHasReadSeqs failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::GetMsgByConversationIDsResp MsgClient::GetMsgByConversationIDs(const ::msg::GetMsgByConversationIDsReq& request) {
  ::msg::GetMsgByConversationIDsResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetMsgByConversationIDs(&context, &request, &reply,
                                          [&mu, &cv, &done, &status](grpc::Status s) {
                                            status = std::move(s);
                                            std::lock_guard<std::mutex> lock(mu);
                                            done = true;
                                            cv.notify_one();
                                          });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("GetMsgByConversationIDs failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::GetConversationMaxSeqResp MsgClient::GetConversationMaxSeq(const ::msg::GetConversationMaxSeqReq& request) {
  ::msg::GetConversationMaxSeqResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetConversationMaxSeq(&context, &request, &reply,
                                        [&mu, &cv, &done, &status](grpc::Status s) {
                                          status = std::move(s);
                                          std::lock_guard<std::mutex> lock(mu);
                                          done = true;
                                          cv.notify_one();
                                        });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("GetConversationMaxSeq failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::sdkws::PullMessageBySeqsResp MsgClient::PullMessageBySeqs(const ::sdkws::PullMessageBySeqsReq& request) {
  ::sdkws::PullMessageBySeqsResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->PullMessageBySeqs(&context, &request, &reply,
                                    [&mu, &cv, &done, &status](grpc::Status s) {
                                      status = std::move(s);
                                      std::lock_guard<std::mutex> lock(mu);
                                      done = true;
                                      cv.notify_one();
                                    });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("PullMessageBySeqs failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::GetSeqMessageResp MsgClient::GetSeqMessage(const ::msg::GetSeqMessageReq& request) {
  ::msg::GetSeqMessageResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetSeqMessage(&context, &request, &reply,
                                [&mu, &cv, &done, &status](grpc::Status s) {
                                  status = std::move(s);
                                  std::lock_guard<std::mutex> lock(mu);
                                  done = true;
                                  cv.notify_one();
                                });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("GetSeqMessage failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::SearchMessageResp MsgClient::SearchMessage(const ::msg::SearchMessageReq& request) {
  ::msg::SearchMessageResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->SearchMessage(&context, &request, &reply,
                                [&mu, &cv, &done, &status](grpc::Status s) {
                                  status = std::move(s);
                                  std::lock_guard<std::mutex> lock(mu);
                                  done = true;
                                  cv.notify_one();
                                });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("SearchMessage failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::sdkws::SendMessageResp MsgClient::SendMessages(const ::sdkws::SendMessageReq& request) {
  ::sdkws::SendMessageResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->SendMessages(&context, &request, &reply,
                               [&mu, &cv, &done, &status](grpc::Status s) {
                                 status = std::move(s);
                                 std::lock_guard<std::mutex> lock(mu);
                                 done = true;
                                 cv.notify_one();
                               });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("SendMessages failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::SendSimpleMsgResp MsgClient::SendSimpleMsg(const ::msg::SendSimpleMsgReq& request) {
  ::msg::SendSimpleMsgResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->SendSimpleMsg(&context, &request, &reply,
                                [&mu, &cv, &done, &status](grpc::Status s) {
                                  status = std::move(s);
                                  std::lock_guard<std::mutex> lock(mu);
                                  done = true;
                                  cv.notify_one();
                                });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("SendSimpleMsg failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::SetUserConversationsMinSeqResp MsgClient::SetUserConversationsMinSeq(const ::msg::SetUserConversationsMinSeqReq& request) {
  ::msg::SetUserConversationsMinSeqResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->SetUserConversationsMinSeq(&context, &request, &reply,
                                             [&mu, &cv, &done, &status](grpc::Status s) {
                                               status = std::move(s);
                                               std::lock_guard<std::mutex> lock(mu);
                                               done = true;
                                               cv.notify_one();
                                             });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("SetUserConversationsMinSeq failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::ClearConversationsMsgResp MsgClient::ClearConversationsMsg(const ::msg::ClearConversationsMsgReq& request) {
  ::msg::ClearConversationsMsgResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->ClearConversationsMsg(&context, &request, &reply,
                                        [&mu, &cv, &done, &status](grpc::Status s) {
                                          status = std::move(s);
                                          std::lock_guard<std::mutex> lock(mu);
                                          done = true;
                                          cv.notify_one();
                                        });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("ClearConversationsMsg failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::UserClearAllMsgResp MsgClient::UserClearAllMsg(const ::msg::UserClearAllMsgReq& request) {
  ::msg::UserClearAllMsgResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->UserClearAllMsg(&context, &request, &reply,
                                  [&mu, &cv, &done, &status](grpc::Status s) {
                                    status = std::move(s);
                                    std::lock_guard<std::mutex> lock(mu);
                                    done = true;
                                    cv.notify_one();
                                  });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("UserClearAllMsg failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::DeleteMsgsResp MsgClient::DeleteMsgs(const ::msg::DeleteMsgsReq& request) {
  ::msg::DeleteMsgsResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->DeleteMsgs(&context, &request, &reply,
                             [&mu, &cv, &done, &status](grpc::Status s) {
                               status = std::move(s);
                               std::lock_guard<std::mutex> lock(mu);
                               done = true;
                               cv.notify_one();
                             });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("DeleteMsgs failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::DeleteMsgPhysicalBySeqResp MsgClient::DeleteMsgPhysicalBySeq(const ::msg::DeleteMsgPhysicalBySeqReq& request) {
  ::msg::DeleteMsgPhysicalBySeqResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->DeleteMsgPhysicalBySeq(&context, &request, &reply,
                                         [&mu, &cv, &done, &status](grpc::Status s) {
                                           status = std::move(s);
                                           std::lock_guard<std::mutex> lock(mu);
                                           done = true;
                                           cv.notify_one();
                                         });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("DeleteMsgPhysicalBySeq failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::DeleteMsgPhysicalResp MsgClient::DeleteMsgPhysical(const ::msg::DeleteMsgPhysicalReq& request) {
  ::msg::DeleteMsgPhysicalResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->DeleteMsgPhysical(&context, &request, &reply,
                                    [&mu, &cv, &done, &status](grpc::Status s) {
                                      status = std::move(s);
                                      std::lock_guard<std::mutex> lock(mu);
                                      done = true;
                                      cv.notify_one();
                                    });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("DeleteMsgPhysical failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::SetSendMsgStatusResp MsgClient::SetSendMsgStatus(const ::msg::SetSendMsgStatusReq& request) {
  ::msg::SetSendMsgStatusResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->SetSendMsgStatus(&context, &request, &reply,
                                   [&mu, &cv, &done, &status](grpc::Status s) {
                                     status = std::move(s);
                                     std::lock_guard<std::mutex> lock(mu);
                                     done = true;
                                     cv.notify_one();
                                   });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("SetSendMsgStatus failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::GetSendMsgStatusResp MsgClient::GetSendMsgStatus(const ::msg::GetSendMsgStatusReq& request) {
  ::msg::GetSendMsgStatusResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetSendMsgStatus(&context, &request, &reply,
                                   [&mu, &cv, &done, &status](grpc::Status s) {
                                     status = std::move(s);
                                     std::lock_guard<std::mutex> lock(mu);
                                     done = true;
                                     cv.notify_one();
                                   });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("GetSendMsgStatus failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::RevokeMsgResp MsgClient::RevokeMsg(const ::msg::RevokeMsgReq& request) {
  ::msg::RevokeMsgResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->RevokeMsg(&context, &request, &reply,
                            [&mu, &cv, &done, &status](grpc::Status s) {
                              status = std::move(s);
                              std::lock_guard<std::mutex> lock(mu);
                              done = true;
                              cv.notify_one();
                            });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("RevokeMsg failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::MarkMsgsAsReadResp MsgClient::MarkMsgsAsRead(const ::msg::MarkMsgsAsReadReq& request) {
  ::msg::MarkMsgsAsReadResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->MarkMsgsAsRead(&context, &request, &reply,
                                 [&mu, &cv, &done, &status](grpc::Status s) {
                                   status = std::move(s);
                                   std::lock_guard<std::mutex> lock(mu);
                                   done = true;
                                   cv.notify_one();
                                 });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("MarkMsgsAsRead failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::MarkConversationAsReadResp MsgClient::MarkConversationAsRead(const ::msg::MarkConversationAsReadReq& request) {
  ::msg::MarkConversationAsReadResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->MarkConversationAsRead(&context, &request, &reply,
                                         [&mu, &cv, &done, &status](grpc::Status s) {
                                           status = std::move(s);
                                           std::lock_guard<std::mutex> lock(mu);
                                           done = true;
                                           cv.notify_one();
                                         });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("MarkConversationAsRead failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::SetConversationHasReadSeqResp MsgClient::SetConversationHasReadSeq(const ::msg::SetConversationHasReadSeqReq& request) {
  ::msg::SetConversationHasReadSeqResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->SetConversationHasReadSeq(&context, &request, &reply,
                                            [&mu, &cv, &done, &status](grpc::Status s) {
                                              status = std::move(s);
                                              std::lock_guard<std::mutex> lock(mu);
                                              done = true;
                                              cv.notify_one();
                                            });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("SetConversationHasReadSeq failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::GetConversationsHasReadAndMaxSeqResp MsgClient::GetConversationsHasReadAndMaxSeq(const ::msg::GetConversationsHasReadAndMaxSeqReq& request) {
  ::msg::GetConversationsHasReadAndMaxSeqResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetConversationsHasReadAndMaxSeq(&context, &request, &reply,
                                                   [&mu, &cv, &done, &status](grpc::Status s) {
                                                     status = std::move(s);
                                                     std::lock_guard<std::mutex> lock(mu);
                                                     done = true;
                                                     cv.notify_one();
                                                   });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("GetConversationsHasReadAndMaxSeq failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::GetActiveUserResp MsgClient::GetActiveUser(const ::msg::GetActiveUserReq& request) {
  ::msg::GetActiveUserResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetActiveUser(&context, &request, &reply,
                                [&mu, &cv, &done, &status](grpc::Status s) {
                                  status = std::move(s);
                                  std::lock_guard<std::mutex> lock(mu);
                                  done = true;
                                  cv.notify_one();
                                });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("GetActiveUser failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::GetActiveGroupResp MsgClient::GetActiveGroup(const ::msg::GetActiveGroupReq& request) {
  ::msg::GetActiveGroupResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetActiveGroup(&context, &request, &reply,
                                 [&mu, &cv, &done, &status](grpc::Status s) {
                                   status = std::move(s);
                                   std::lock_guard<std::mutex> lock(mu);
                                   done = true;
                                   cv.notify_one();
                                 });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("GetActiveGroup failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::GetServerTimeResp MsgClient::GetServerTime(const ::msg::GetServerTimeReq& request) {
  ::msg::GetServerTimeResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetServerTime(&context, &request, &reply,
                                [&mu, &cv, &done, &status](grpc::Status s) {
                                  status = std::move(s);
                                  std::lock_guard<std::mutex> lock(mu);
                                  done = true;
                                  cv.notify_one();
                                });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("GetServerTime failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::ClearMsgResp MsgClient::ClearMsg(const ::msg::ClearMsgReq& request) {
  ::msg::ClearMsgResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->ClearMsg(&context, &request, &reply,
                           [&mu, &cv, &done, &status](grpc::Status s) {
                             status = std::move(s);
                             std::lock_guard<std::mutex> lock(mu);
                             done = true;
                             cv.notify_one();
                           });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("ClearMsg failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::DestructMsgsResp MsgClient::DestructMsgs(const ::msg::DestructMsgsReq& request) {
  ::msg::DestructMsgsResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->DestructMsgs(&context, &request, &reply,
                               [&mu, &cv, &done, &status](grpc::Status s) {
                                 status = std::move(s);
                                 std::lock_guard<std::mutex> lock(mu);
                                 done = true;
                                 cv.notify_one();
                               });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("DestructMsgs failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::GetActiveConversationResp MsgClient::GetActiveConversation(const ::msg::GetActiveConversationReq& request) {
  ::msg::GetActiveConversationResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetActiveConversation(&context, &request, &reply,
                                        [&mu, &cv, &done, &status](grpc::Status s) {
                                          status = std::move(s);
                                          std::lock_guard<std::mutex> lock(mu);
                                          done = true;
                                          cv.notify_one();
                                        });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("GetActiveConversation failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::SetUserConversationMaxSeqResp MsgClient::SetUserConversationMaxSeq(const ::msg::SetUserConversationMaxSeqReq& request) {
  ::msg::SetUserConversationMaxSeqResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->SetUserConversationMaxSeq(&context, &request, &reply,
                                            [&mu, &cv, &done, &status](grpc::Status s) {
                                              status = std::move(s);
                                              std::lock_guard<std::mutex> lock(mu);
                                              done = true;
                                              cv.notify_one();
                                            });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("SetUserConversationMaxSeq failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::SetUserConversationMinSeqResp MsgClient::SetUserConversationMinSeq(const ::msg::SetUserConversationMinSeqReq& request) {
  ::msg::SetUserConversationMinSeqResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->SetUserConversationMinSeq(&context, &request, &reply,
                                            [&mu, &cv, &done, &status](grpc::Status s) {
                                              status = std::move(s);
                                              std::lock_guard<std::mutex> lock(mu);
                                              done = true;
                                              cv.notify_one();
                                            });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("SetUserConversationMinSeq failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::GetLastMessageSeqByTimeResp MsgClient::GetLastMessageSeqByTime(const ::msg::GetLastMessageSeqByTimeReq& request) {
  ::msg::GetLastMessageSeqByTimeResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetLastMessageSeqByTime(&context, &request, &reply,
                                          [&mu, &cv, &done, &status](grpc::Status s) {
                                            status = std::move(s);
                                            std::lock_guard<std::mutex> lock(mu);
                                            done = true;
                                            cv.notify_one();
                                          });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("GetLastMessageSeqByTime failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}

::msg::GetLastMessageResp MsgClient::GetLastMessage(const ::msg::GetLastMessageReq& request) {
  ::msg::GetLastMessageResp reply;
  grpc::ClientContext context;

  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetLastMessage(&context, &request, &reply,
                                 [&mu, &cv, &done, &status](grpc::Status s) {
                                   status = std::move(s);
                                   std::lock_guard<std::mutex> lock(mu);
                                   done = true;
                                   cv.notify_one();
                                 });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  if (!status.ok()) {
    spdlog::error("GetLastMessage failed: {} {}", static_cast<int>(status.error_code()), status.error_message());
  }

  return reply;
}