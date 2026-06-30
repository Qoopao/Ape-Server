#include "services/push_service/server.h"
#include "messagequeue/message_consumer.h"
#include "messagequeue/message_handler.h"
#include "services/backbon_service/client.h"
#include "services/gateway_push_service/client.h"
#include "services/push_service/push_msg_handler.h"
#include "util/mongohandler.h"
#include "util/redisconnector.h"
#include "util/redishandler.h"
#include <chrono>
#include <spdlog/spdlog.h>

PushServer::PushServer(const std::string &service_name,
                       const std::string &listen_address)
    : BaseServiceServer<PushServer>(service_name, listen_address) {}

PushServer::~PushServer() {
  if (consumer_) {
    consumer_->shutdown();
  }
  if (consumer_thread_.joinable()) {
    consumer_thread_.join();
  }
}

void PushServer::SetConsumer(std::unique_ptr<IMessageConsumer> consumer) {
  consumer_ = std::move(consumer);
}

void PushServer::SetIOCPool(IOC_Pool *ioc_pool) { ioc_pool_ = ioc_pool; }

void PushServer::Start() {

  // 先启动 gRPC 服务器（继承自 BaseServiceServer）
  BaseServiceServer<PushServer>::Start();

  // 创建 PushHandler，gRPC 调用本服务的 PushMsg
  auto pushHandler = std::make_shared<PushHandler>(grpc::CreateChannel(
      GetListenAddress(), grpc::InsecureChannelCredentials()));

  // 配置并启动消息消费者
  if (consumer_) {
    consumer_->setHandler(pushHandler);
    if (ioc_pool_) {
      consumer_->setIOCPool(ioc_pool_);
    }
    consumer_thread_ = std::thread([this]() { consumer_->start(); });
    spdlog::info("PushServer: consumer started in background thread");
  }

  // 启动后台协程：定期扫描超时的在线 pending_ack，回退到离线存储
  boost::asio::co_spawn(RedisConnector::instance().get_executor(),
                        CheckExpiredPendingAck(), boost::asio::detached);
  spdlog::info(
      "PushServer: CheckExpiredPendingAck background coroutine started");
}

::grpc::ServerUnaryReactor *
PushServer::PushMsg(::grpc::CallbackServerContext *context,
                    const ::push::PushMsgReq *request,
                    ::push::PushMsgResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

  // 获取发送者信息
  if (!request->has_msgdata()) {
    spdlog::error("PushServer::PushMsg: no msgdata in request");
    reactor->Finish(::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                   "msgdata is required"));
    return reactor;
  }

  const auto msgData = request->msgdata(); // 拷贝到协程安全
  std::string conversationID = request->conversationid();

  spdlog::info(
      "[PushService] PushMsg: sender={}, recv={}, convID={}, content_type={}",
      msgData.sendid(), msgData.recvid(), conversationID,
      msgData.contenttype());

  // TODO: sender token 验证
  // 当前 PushMsg 由 msg_service (Kafka 消费链路) 内部调用，msg_service 已校验
  // sender 后续若开放外部调用，需从此处 context metadata 提取 authorization
  // token 并验证 sender
  spdlog::info(
      "[PushService] PushMsg sender={} - token validation deferred (TODO)",
      msgData.sendid());

  // 如果是群聊，需要推送给群内所有在线成员
  if (msgData.isgroupmsg()) {
    spdlog::info("PushServer::PushMsg: group message for group={}, pushing to "
                 "group members",
                 conversationID);
    // TODO: 大群用户拉，小群网关推
    //   1. 查询群成员列表
    //   2. 对每个在线成员走 Gateway 推送
    //   3. 对每个离线成员走 MongoDB 冷存储 + pending_ack
    reactor->Finish(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED,
                                   "group push not implemented yet"));
    return reactor;
  }

  // ── 单聊推送 ──
  // 前置：SendMessages 已完成 MongoDB(IM-System.msg, status=1) + Redis 落库。
  //
  //   recvOnline? ─Yes→ 尝试 Gateway 推送
  //     │                    ↓
  //     │              pushed? ─Yes→ pending_ack:online (等 ACK → status=2)
  //     │                    ↓
  //     │                    No/异常 → status=3, dStatus|=1, pending_ack:offline
  //     ↓
  //     No → dStatus|=1, pending_ack:offline (等 ACK → status=2)
  //
  // CheckExpiredPendingAck 定时扫描 deadlines → 超时未 ACK 降级为 offline pending_ack。
  //
  // AckMsg(延迟双删): ZREM → MarkMsgsAsDelivered(status=2) → sleep(500ms) → ZREM

  // 预初始化 GatewayPushClient
  getGatewayPushClient();
  GatewayPushClient *pushClient = gateway_push_client_.get();

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, msgData = std::move(msgData),
       conversationID = std::move(conversationID),
       pushClient]() -> boost::asio::awaitable<void> {
        bool recvOnline = false;

        // 查 Redis 在线状态
        try {
          auto onlineVal = co_await RedisConnector::instance().get(
              "user:" + msgData.recvid() + ":online");
          if (onlineVal) {
            recvOnline = (*onlineVal == "1" || *onlineVal == "true");
          }
        } catch (const std::exception &e) {
          spdlog::error("PushServer::PushMsg: Redis error: {}", e.what());
        }

        std::string recvID = msgData.recvid();
        std::string serverMsgID = msgData.servermsgid();
        std::string msgDataBin = msgData.SerializeAsString();

        // 在线，尝试 Gateway 推送
        bool pushed = false;
        if (recvOnline) {
          spdlog::info("PushServer::PushMsg: receiver {} is online, pushing "
                       "via GatewayPushService gRPC",
                       recvID);
          try {
            pushed = co_await pushClient->PushToUser(recvID, msgDataBin,
                                                      conversationID);
          } catch (const std::exception &e) {
            spdlog::error("PushServer::PushMsg: online push exception: {}, "
                          "recvID={}",
                          e.what(), recvID);
          }
        }

        if (pushed) {
          // 在线推送成功：Gateway 已接受投递，等待客户端 ACK 后将 status 置 2
          spdlog::info("PushServer::PushMsg: online push succeeded, "
                       "recvID={}, serverMsgID={}",
                       recvID, serverMsgID);

          std::string ackKey = "pending_ack:online:" + serverMsgID;
          co_await RedisConnector::instance().sadd(ackKey, recvID);
          co_await RedisConnector::instance().expire(ackKey, 300);

          int64_t deadline =
              std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count() +
              300;
          co_await RedisConnector::instance().zadd("pending_ack:deadlines",
                                                   deadline, serverMsgID);

          spdlog::info("PushServer::PushMsg: added to pending_ack, "
                       "key={}, user={}, deadline={}",
                       ackKey, recvID, deadline);
        } else if (recvOnline) {
          // 在线但推送失败 → status=3, dStatus|=1（延迟双删保证缓存一致）
          co_await RedisHandler::DeleteMsgCache(serverMsgID);         // 删缓存
          try {
            co_await MongoHandler::MarkMsgAsPushFailedAsync(serverMsgID); // 更新数据库
            auto timer = boost::asio::steady_timer(
                co_await boost::asio::this_coro::executor);
            timer.expires_after(boost::asio::chrono::milliseconds(500)); // 延迟等待
            co_await timer.async_wait(boost::asio::use_awaitable);
            co_await RedisHandler::DeleteMsgCache(serverMsgID);       // 删缓存
          } catch (const std::exception &e) {
            spdlog::error("PushServer::PushMsg: MarkMsgAsPushFailed failed: {}",
                          e.what());
          }
          try {
            std::string offlineAckKey =
                "pending_ack:offline:" + recvID + ":" + serverMsgID;
            co_await RedisConnector::instance().set(offlineAckKey, "1");
            co_await RedisConnector::instance().expire(offlineAckKey,
                                                        86400 * 30);
            spdlog::info("PushServer::PushMsg: push failed, created offline "
                         "pending_ack, key={}",
                         offlineAckKey);
          } catch (const std::exception &e) {
            spdlog::error("PushServer::PushMsg: failed to create offline "
                          "pending_ack: {}",
                          e.what());
          }
        } else {
          // 接收者离线 → dStatus|=1（延迟双删保证缓存一致）
          co_await RedisHandler::DeleteMsgCache(serverMsgID);         // 删缓存
          try {
            co_await MongoHandler::MarkMsgAsOfflineAsync(serverMsgID); // 更新数据库
            auto timer = boost::asio::steady_timer(
                co_await boost::asio::this_coro::executor);
            timer.expires_after(boost::asio::chrono::milliseconds(500)); // 延迟等待
            co_await timer.async_wait(boost::asio::use_awaitable);
            co_await RedisHandler::DeleteMsgCache(serverMsgID);       // 删缓存
          } catch (const std::exception &e) {
            spdlog::error("PushServer::PushMsg: MarkMsgAsOffline failed: {}",
                          e.what());
          }
          try {
            std::string offlineAckKey =
                "pending_ack:offline:" + recvID + ":" + serverMsgID;
            co_await RedisConnector::instance().set(offlineAckKey, "1");
            co_await RedisConnector::instance().expire(offlineAckKey,
                                                        86400 * 30);
            spdlog::info("PushServer::PushMsg: created offline pending_ack, "
                         "key={}",
                         offlineAckKey);
          } catch (const std::exception &e) {
            spdlog::error("PushServer::PushMsg: failed to create offline "
                          "pending_ack: {}",
                          e.what());
          }
        }

        // TODO: serverMsgID→convID 映射应由 ConversationService 管理，
        // 目前临时存 Redis（TTL 7 天），后续迁移到会话服务的持久化存储
        // 缓存 serverMsgID → convID 映射，供 AckMsg 查 convID 更新会话级 last_seq
        try {
          co_await RedisConnector::instance().set("msg_conv:" + serverMsgID,
                                                   conversationID);
          co_await RedisConnector::instance().expire("msg_conv:" + serverMsgID,
                                                      7 * 86400);
        } catch (const std::exception &e) {
          spdlog::error("PushServer::PushMsg: failed to cache msg_conv: {}",
                        e.what());
        }

        spdlog::info("PushServer::PushMsg: push completed for conv={}",
                     conversationID);
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

::grpc::ServerUnaryReactor *
PushServer::AckMsg(::grpc::CallbackServerContext *context,
                   const ::push::AckMsgReq *request,
                   ::push::AckMsgResp *response) {

  //  ---
  // 一、用到的 Redis Key（统一 user_msgs ZSET 架构后）

  // 在线ACK:
  //   pending_ack:online:{msgID}      SET   成员=recvID    TTL=300s (5分钟)
  //   pending_ack:deadlines            ZSET  score=截止时间戳 member=msgID

  // 离线ACK:
  //   pending_ack:offline:{userID}:{msgID}  STRING  value="1"   TTL=30天

  // 全量消息缓存（在线/离线统一）:
  //   user_msgs:{userID}              ZSET  score=seq  member=base64(msg)
  //   TTL=7天 msg:{msgID}                     STRING  消息体 (MsgService存的)

  // 公共:
  //   user:{userID}:{convID}:last_seq  STRING  会话级最后ACK的seq

  // ---
  // 二、ACK 延迟双删流（统一架构）

  // 所有消息出生时已在 SendMessages 写入 user_msgs:{recvID} ZSET。
  // ACK 采用延迟双删保证副本集下缓存一致性：

  // ┌─ AckMsg ───────────────────────────────────────────────────────────┐
  // │                                                                     │
  // │  ① ZREM user_msgs:{userID}                  ← 第一次删缓存          │
  // │  ② MongoDB MarkMsgsAsDelivered (status=2) ← 更新真相源          │
  // │     writeConcern: majority                                        │
  // │  ③ sleep(500ms)                              ← 等副本集同步         │
  // │  ④ ZREM user_msgs:{userID}                  ← 第二次删（清脏缓存）   │
  // │                                                                     │
  // │  并发 Reader 在①②之间从 MongoDB Secondary 读到 status=1        │
  // │  的脏数据并回温到 user_msgs ZSET → ④ 的第二次 ZREM 将其清掉         │
  // └─────────────────────────────────────────────────────────────────────┘

  // ackType=1（在线ACK）:
  //   在线推送成功 → SADD pending_ack:online + ZADD deadlines
  //   ACK → SREM pending_ack → ZREM user_msgs → 全部ACK? → MongoDB status=2
  //        → sleep(500ms) → ZREM user_msgs（延迟双删）
  //        → DEL pending_ack + ZREM deadlines

  // ackType=2（离线ACK）:
  //   离线存储 → SET pending_ack:offline
  //   ACK → ZREM user_msgs → DEL pending_ack:offline → MongoDB status=2
  //        → sleep(500ms) → ZREM user_msgs（延迟双删）

  // 关键点:
  //   ZREM 幂等，多删无害。第二次 ZREM 专门清理并发 Reader 在
  //   MongoDB 副本集复制延迟窗口内回温进 Redis 的脏缓存。
  //   pending_ack:online 用 SET 是为了兼容群聊——多人各自 ACK。

  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

  std::string userID = request->userid();
  std::string serverMsgID = request->servermsgid();
  int64_t seq = request->seq();
  int32_t ackType = request->acktype();

  spdlog::info(
      "[PushService] AckMsg: userID={}, serverMsgID={}, seq={}, ackType={}",
      userID, serverMsgID, seq, ackType);

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, userID = std::move(userID),
       serverMsgID = std::move(serverMsgID), seq,
       ackType]() -> boost::asio::awaitable<void> {
        try {
          // ═══════════════════════════════════════════════════════
          // 统一 ACK 处理 —— 延迟双删模式
          //
          // 所有消息（在线/离线）均在 user_msgs:{userID} ZSET 中。
          // ACK 时先删缓存 → 写 MongoDB → 等副本集同步 → 再删缓存，
          // 防止并发 Reader 在复制延迟窗口内将 Secondary 的脏数据回温到 Redis。
          // ═══════════════════════════════════════════════════════

          if (ackType == 1) {
            // === 在线消息ACK (延迟双删) ===

            // ① 第一次删：从 user_msgs ZSET 移除
            co_await RedisHandler::AckOfflineMsg(userID, serverMsgID);

            // ② 维护 pending_ack:online SET
            std::string ackKey = "pending_ack:online:" + serverMsgID;
            co_await RedisConnector::instance().srem(ackKey, userID);

            // ③ 检查全部 ACK 完成 → 写 MongoDB + 延迟双删
            long long remaining =
                co_await RedisConnector::instance().scard(ackKey);
            if (remaining == 0) {
              std::vector<std::string> msgIds = {serverMsgID};
              co_await MongoHandler::MarkMsgsAsDeliveredAsync(userID, msgIds);

              // ④ 等副本集同步
              auto timer = boost::asio::steady_timer(
                  RedisConnector::instance().get_executor());
              timer.expires_after(boost::asio::chrono::milliseconds(500));
              co_await timer.async_wait(boost::asio::use_awaitable);

              // ⑤ 第二次删：清理由 ①→③ 之间并发 Reader 回温的脏缓存
              co_await RedisHandler::AckOfflineMsg(userID, serverMsgID);

              // 清理 pending_ack 追踪
              co_await RedisConnector::instance().del(ackKey);
              co_await RedisConnector::instance().zrem("pending_ack:deadlines",
                                                       serverMsgID);

              spdlog::info("[PushService] AckMsg(online): all ACK done with "
                           "delayed double delete, serverMsgID={}",
                           serverMsgID);
            } else {
              // 群聊：还有其他人没 ACK，先只做自己的第一次 ZREM
              // 延迟双删等 SCARD==0 时再做
              spdlog::info("[PushService] AckMsg(online): remaining={}, "
                           "serverMsgID={}",
                           remaining, serverMsgID);
            }
          } else if (ackType == 2) {
            // === 离线消息ACK (延迟双删) ===

            // ① 第一次删：从 user_msgs ZSET 移除
            co_await RedisHandler::AckOfflineMsg(userID, serverMsgID);

            // ② 清理 pending_ack:offline
            std::string offlineAckKey =
                "pending_ack:offline:" + userID + ":" + serverMsgID;
            co_await RedisConnector::instance().del(offlineAckKey);

            // ③ 写 MongoDB
            std::vector<std::string> msgIds = {serverMsgID};
            co_await MongoHandler::MarkMsgsAsDeliveredAsync(userID, msgIds);

            // ④ 等副本集同步
            auto timer = boost::asio::steady_timer(
                RedisConnector::instance().get_executor());
            timer.expires_after(boost::asio::chrono::milliseconds(500));
            co_await timer.async_wait(boost::asio::use_awaitable);

            // ⑤ 第二次删：清理由 ①→③ 之间并发 Reader 回温的脏缓存
            co_await RedisHandler::AckOfflineMsg(userID, serverMsgID);

            spdlog::info("[PushService] AckMsg(offline): delayed double delete "
                         "done, user={}, serverMsgID={}",
                         userID, serverMsgID);
          } else {
            spdlog::warn("[PushService] AckMsg: unknown ackType={}", ackType);
          }

          // TODO: 会话水位应由 ConversationService.SetConversationMaxSeq 管理
          // （双写 Redis + MongoDB conversations.maxSeq），目前只写 Redis
          // 更新会话级 last_seq，记录每个用户在每个会话已确认的最高水位
          std::string convID;
          auto convOpt =
              co_await RedisConnector::instance().get("msg_conv:" + serverMsgID);
          if (convOpt && !convOpt->empty()) {
            convID = *convOpt;
          } else {
            // 缓存未命中，从 MongoDB 回源拿 convID
            auto msgOpt =
                co_await MongoHandler::GetMsgByServerMsgIDAsync(serverMsgID);
            if (msgOpt.has_value()) {
              convID = msgOpt->convid();
            }
          }
          if (!convID.empty()) {
            std::string seqKey = "user:" + userID + ":" + convID + ":last_seq";
            auto lastSeqOpt = co_await RedisConnector::instance().get(seqKey);
            int64_t currentSeq = lastSeqOpt ? std::stoll(*lastSeqOpt) : 0;
            if (seq > currentSeq) {
              co_await RedisConnector::instance().set(seqKey,
                                                      std::to_string(seq));
            }
          }

        } catch (const std::exception &e) {
          spdlog::error("[PushService] AckMsg: error: {}", e.what());
          reactor->Finish(
              ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what()));
          co_return;
        }

        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

::grpc::ServerUnaryReactor *
PushServer::AddPendingOfflineAck(::grpc::CallbackServerContext *context,
                                 const ::push::AddPendingOfflineAckReq *request,
                                 ::push::AddPendingOfflineAckResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

  std::string userID = request->userid();
  std::string serverMsgID = request->servermsgid();
  int64_t seq = request->seq();

  spdlog::info(
      "[PushService] AddPendingOfflineAck: userID={}, serverMsgID={}, seq={}",
      userID, serverMsgID, seq);

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, userID = std::move(userID),
       serverMsgID = std::move(serverMsgID)]() -> boost::asio::awaitable<void> {
        try {
          // 将离线消息添加到pending_ack集合，等待客户端ACK
          std::string ackKey =
              "pending_ack:offline:" + userID + ":" + serverMsgID;
          co_await RedisConnector::instance().set(ackKey, "1");
          co_await RedisConnector::instance().expire(
              ackKey, 86400 * 30); // 30天超时，防止遗留
          spdlog::info("[PushService] AddPendingOfflineAck: created "
                       "pending_ack entry, key={}",
                       ackKey);
        } catch (const std::exception &e) {
          spdlog::error("[PushService] AddPendingOfflineAck: Redis error: {}",
                        e.what());
          reactor->Finish(
              ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what()));
          co_return;
        }

        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

// 如果某个消息一直没被ack，那发送方永远不会知道接收方到底收没收，需要考虑
boost::asio::awaitable<void> PushServer::CheckExpiredPendingAck() {
  // 每 30 秒扫描一次 deadline ZSET，将超时未 ACK 的在线消息回退到离线存储
  auto &redis = RedisConnector::instance();
  auto timer = boost::asio::steady_timer(redis.get_executor());

  while (true) {
    timer.expires_after(boost::asio::chrono::seconds(30));
    boost::system::error_code ec;
    co_await timer.async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    if (ec) {
      spdlog::warn("CheckExpiredPendingAck: timer error: {}", ec.message());
      continue;
    }

    try {
      int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

      // 查询所有已过截止时间的 serverMsgID
      auto expired =
          co_await redis.zrangebyscore("pending_ack:deadlines", 0, now);

      for (const auto &serverMsgID : expired) {
        // 先 Redis 缓存，未命中则回退 MongoDB
        auto msgDataOpt = co_await RedisHandler::GetMsg(serverMsgID);
        if (!msgDataOpt.has_value()) {
          msgDataOpt =
              co_await MongoHandler::GetMsgByServerMsgIDAsync(serverMsgID);
        }

        if (msgDataOpt.has_value()) {
          sdkws::MsgData msgData = msgDataOpt.value();
          std::string recvID = msgData.recvid();

          spdlog::warn(
              "CheckExpiredPendingAck: online ACK timeout for "
              "serverMsgID={}, recvID={}, falling back to offline storage",
              serverMsgID, recvID);

          // 消息已在 SendMessages 存入 IM-System.msg，此处只需标记离线待 ACK
          // 创建 pending_ack:offline 记录
          try {
            std::string offlineAckKey =
                "pending_ack:offline:" + recvID + ":" + serverMsgID;
            co_await redis.set(offlineAckKey, "1");
            co_await redis.expire(offlineAckKey, 86400 * 30);
          } catch (const std::exception &e) {
            spdlog::error("CheckExpiredPendingAck: failed to create "
                          "pending_ack:offline: {}",
                          e.what());
          }
        }

        // 清理过期条目
        co_await redis.del("pending_ack:online:" + serverMsgID);
        co_await redis.zrem("pending_ack:deadlines", serverMsgID);

        spdlog::info(
            "CheckExpiredPendingAck: cleaned up expired pending_ack for "
            "serverMsgID={}",
            serverMsgID);
      }
    } catch (const std::exception &e) {
      spdlog::error("CheckExpiredPendingAck: scan error: {}", e.what());
    }
  }
}

GatewayPushClient &PushServer::getGatewayPushClient() {
  if (gateway_push_client_) {
    return *gateway_push_client_;
  }

  // 延迟初始化：通过 BackbonService (etcd) 服务发现获取 GatewayPushService 地址
  std::string gateway_addr = "localhost:50055"; // fallback

  // 当前回退到硬编码 fallback 地址，后续需将 gateway_push_client_
  // 的初始化改为异步
  auto *backbon = GetBackbonClient();
  if (backbon) {
    spdlog::warn("[PushService] BackbonClient::GetServicesList is async "
                 "(co_await required), "
                 "using fallback gateway address={}",
                 gateway_addr);
  }

  gateway_channel_ =
      grpc::CreateChannel(gateway_addr, grpc::InsecureChannelCredentials());
  gateway_push_client_ = std::make_unique<GatewayPushClient>(gateway_channel_);
  return *gateway_push_client_;
}

::grpc::ServerUnaryReactor *
PushServer::DelUserPushToken(::grpc::CallbackServerContext *context,
                             const ::push::DelUserPushTokenReq *request,
                             ::push::DelUserPushTokenResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  // To-do

  reactor->Finish(::grpc::Status::OK);
  return reactor;
}
