#include <bsoncxx/builder/stream/array.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <google/protobuf/util/json_util.h>
#include <mongocxx/client.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>

#include <boost/asio.hpp>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>

#include "storage/mongoconnector.h"
#include "storage/mongohandler.h"
#include "user/userinfo.h"

// mongodb仅存消息记录，不存用户
mongocxx::collection
MongoHandler::get_collection(const std::string db_name,
                             const std::string collection_name) {
  auto &mongoconnector = MongoConnector::instance();
  try {
    auto client = mongoconnector.acquire_client();
    return (*client)[db_name][collection_name];
  } catch (...) {
    throw;
  }
}

// Set
bool MongoHandler::SaveMsgToMongo(const sdkws::MsgData &msg) {
  try {
    // 直接获取 pool entry 并保持其生命周期直到 insert_one 完成，
    // 避免 get_collection() 返回后 pool entry 析构导致 client 失效
    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["msg"];

    auto doc = bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp("serverMsgID", msg.servermsgid()),
        bsoncxx::builder::basic::kvp("sendID", msg.sendid()),
        bsoncxx::builder::basic::kvp("recvID", msg.recvid()),
        bsoncxx::builder::basic::kvp("convID", msg.convid()),
        bsoncxx::builder::basic::kvp("clientMsgID", msg.clientmsgid()),
        bsoncxx::builder::basic::kvp("senderPlatformID",
                                     msg.senderplatformid()),
        bsoncxx::builder::basic::kvp("senderNickname", msg.sendernickname()),
        bsoncxx::builder::basic::kvp("senderFaceURL", msg.senderfaceurl()),
        bsoncxx::builder::basic::kvp("sessionType", msg.sessiontype()),
        bsoncxx::builder::basic::kvp("msgFrom", msg.msgfrom()),
        bsoncxx::builder::basic::kvp("contentType", msg.contenttype()),
        bsoncxx::builder::basic::kvp("content", msg.content()),
        bsoncxx::builder::basic::kvp("seq", msg.seq()),
        bsoncxx::builder::basic::kvp("sendTime", msg.sendtime()),
        bsoncxx::builder::basic::kvp("status", msg.status()),
        bsoncxx::builder::basic::kvp("isRead", msg.isread()),
        bsoncxx::builder::basic::kvp("isDeleted", msg.isdeleted()),
        bsoncxx::builder::basic::kvp("isRecalled", msg.isrecalled()),
        bsoncxx::builder::basic::kvp("isPinned", msg.ispinned()),
        bsoncxx::builder::basic::kvp("isGroupMsg", msg.isgroupmsg()),
        bsoncxx::builder::basic::kvp("ext", msg.ext())
      );

    // 观察doc的值
    // auto view = doc.view();
    // spdlog::info("Inserting doc: {}", bsoncxx::to_json(view));

    auto result = collection.insert_one(doc.view());
    if (result) {
      spdlog::info("SaveMsgToMongo success, serverMsgID={}", msg.servermsgid());
      return true;
    } else {
      spdlog::error("SaveMsgToMongo insert_one failed, serverMsgID={}",
                    msg.servermsgid());
      return false;
    }
  } catch (const mongocxx::exception &e) {
    spdlog::error("SaveMsgToMongo exception: {}, serverMsgID={}", e.what(),
                  msg.servermsgid());
    return false;
  }
}

// Get
bool MongoHandler::GetMsgByServerMsgID(const std::string &serverMsgID,
                                       sdkws::MsgData &outMsg) {
  try {
    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["msg"];

    bsoncxx::builder::stream::document filter_doc{};
    filter_doc << "serverMsgID" << serverMsgID
               << bsoncxx::builder::stream::finalize;

    auto find_opt = mongocxx::options::find{};
    find_opt.limit(1);
    auto cursor = collection.find(filter_doc.view(), find_opt);

    auto it = cursor.begin();
    if (it == cursor.end()) {
      spdlog::warn("GetMsgByServerMsgID not found, serverMsgID={}",
                   serverMsgID);
      return false;
    }

    bsoncxx::document::view doc = *it;

    // 每个字段先检查存在性再取值
    if (auto e = doc["serverMsgID"])
      outMsg.set_servermsgid(std::string{e.get_string().value});
    if (auto e = doc["sendID"])
      outMsg.set_sendid(std::string{e.get_string().value});
    if (auto e = doc["recvID"])
      outMsg.set_recvid(std::string{e.get_string().value});
    if (auto e = doc["convID"])
      outMsg.set_convid(std::string{e.get_string().value});
    if (auto e = doc["clientMsgID"])
      outMsg.set_clientmsgid(std::string{e.get_string().value});
    if (auto e = doc["senderPlatformID"])
      outMsg.set_senderplatformid(e.get_int32().value);
    if (auto e = doc["senderNickname"])
      outMsg.set_sendernickname(std::string{e.get_string().value});
    if (auto e = doc["senderFaceURL"])
      outMsg.set_senderfaceurl(std::string{e.get_string().value});
    if (auto e = doc["sessionType"])
      outMsg.set_sessiontype(e.get_int32().value);
    if (auto e = doc["msgFrom"])
      outMsg.set_msgfrom(e.get_int32().value);
    if (auto e = doc["contentType"])
      outMsg.set_contenttype(e.get_int32().value);
    if (auto e = doc["content"])
      outMsg.set_content(std::string{e.get_string().value});
    if (auto e = doc["seq"])
      outMsg.set_seq(e.get_int64().value);
    if (auto e = doc["sendTime"])
      outMsg.set_sendtime(e.get_double().value);
    if (auto e = doc["status"])
      outMsg.set_status(e.get_int32().value);
    if (auto e = doc["isRead"])
      outMsg.set_isread(e.get_bool().value);
    if (auto e = doc["isDeleted"])
      outMsg.set_isdeleted(e.get_bool().value);
    if (auto e = doc["isRecalled"])
      outMsg.set_isrecalled(e.get_bool().value);
    if (auto e = doc["isPinned"])
      outMsg.set_ispinned(e.get_bool().value);
    if (auto e = doc["isGroupMsg"])
      outMsg.set_isgroupmsg(e.get_bool().value);
    if (auto e = doc["ext"])
      outMsg.set_ext(std::string{e.get_string().value});

    spdlog::info("GetMsgByServerMsgID success, serverMsgID={}", serverMsgID);
    return true;
  } catch (const mongocxx::exception &e) {
    spdlog::error("GetMsgByServerMsgID exception: {}, serverMsgID={}", e.what(),
                  serverMsgID);
    return false;
  }
}

std::vector<sdkws::MsgData>
MongoHandler::GetMsgsBySeqFromMongo(const std::string &userId, int64_t afterSeq,
                                    int limit) {
  std::vector<sdkws::MsgData> results;
  try {
    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["msg"];

    bsoncxx::builder::stream::document filter{};
    filter << "recvID" << userId << "seq"
           << bsoncxx::builder::stream::open_document << "$gt" << afterSeq
           << bsoncxx::builder::stream::close_document;

    mongocxx::options::find opts{};
    opts.sort(bsoncxx::builder::stream::document{}
              << "seq" << 1 << bsoncxx::builder::stream::finalize);
    opts.limit(limit);

    auto cursor = collection.find(filter.view(), opts);

    for (auto &&doc : cursor) {
      try {
        sdkws::MsgData msg;
        if (auto e = doc["serverMsgID"])
          msg.set_servermsgid(std::string{e.get_string().value});
        if (auto e = doc["sendID"])
          msg.set_sendid(std::string{e.get_string().value});
        if (auto e = doc["recvID"])
          msg.set_recvid(std::string{e.get_string().value});
        if (auto e = doc["convID"])
          msg.set_convid(std::string{e.get_string().value});
        if (auto e = doc["clientMsgID"])
          msg.set_clientmsgid(std::string{e.get_string().value});
        if (auto e = doc["senderNickname"])
          msg.set_sendernickname(std::string{e.get_string().value});
        if (auto e = doc["sessionType"])
          msg.set_sessiontype(std::int32_t{e.get_int32().value});
        if (auto e = doc["contentType"])
          msg.set_contenttype(std::int32_t{e.get_int32().value});
        if (auto e = doc["content"])
          msg.set_content(std::string{e.get_string().value});
        if (auto e = doc["seq"])
          msg.set_seq(e.get_int64().value);
        if (auto e = doc["sendTime"])
          msg.set_sendtime(double{e.get_double().value});
        if (auto e = doc["isGroupMsg"])
          msg.set_isgroupmsg(e.get_bool().value);
        results.push_back(std::move(msg));
      } catch (...) {
        spdlog::error("GetMsgsBySeqFromMongo parse failed for user={}", userId);
      }
    }

    spdlog::info("GetMsgsBySeqFromMongo success, userId={}, count={}", userId,
                 results.size());
    return results;
  } catch (const mongocxx::exception &e) {
    spdlog::error("GetMsgsBySeqFromMongo exception: {}, userId={}", e.what(),
                  userId);
    return results;
  }
}

bool MongoHandler::MarkMsgsAsDelivered(const std::string &userId,
                                       const std::vector<std::string> &msgIds) {
  try {
    if (msgIds.empty())
      return true;

    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["msg"];

    for (const auto &msgId : msgIds) {
      bsoncxx::builder::stream::document filter{};
      filter << "recvID" << userId << "serverMsgID" << msgId;

      bsoncxx::builder::stream::document update{};
      update << "$set" << bsoncxx::builder::stream::open_document << "status"
             << 2 << bsoncxx::builder::stream::close_document;

      collection.update_one(filter.view(), update.view());
    }

    spdlog::info("MarkMsgsAsDelivered success, userId={}, count={}", userId,
                 msgIds.size());
    return true;
  } catch (const mongocxx::exception &e) {
    spdlog::error("MarkMsgsAsDelivered exception: {}, userId={}", e.what(),
                  userId);
    return false;
  }
}

// ==================== 异步版本（IOCPool worker 卸载） ====================

boost::asio::awaitable<bool>
MongoHandler::SaveMsgToMongoAsync(sdkws::MsgData msg) {
  co_return co_await MongoConnector::instance().async_run(
      [msg = std::move(msg)]() mutable { return SaveMsgToMongo(msg); });
}

boost::asio::awaitable<std::optional<sdkws::MsgData>>
MongoHandler::GetMsgByServerMsgIDAsync(std::string serverMsgID) {
  co_return co_await MongoConnector::instance().async_run(
      [serverMsgID =
           std::move(serverMsgID)]() -> std::optional<sdkws::MsgData> {
        sdkws::MsgData msg;
        if (GetMsgByServerMsgID(serverMsgID, msg))
          return msg;
        return std::nullopt;
      });
}

boost::asio::awaitable<std::vector<sdkws::MsgData>>
MongoHandler::GetMsgsBySeqFromMongoAsync(std::string userId, int64_t afterSeq,
                                         int limit) {
  co_return co_await MongoConnector::instance().async_run(
      [userId = std::move(userId), afterSeq, limit]() {
        return GetMsgsBySeqFromMongo(userId, afterSeq, limit);
      });
}

boost::asio::awaitable<bool>
MongoHandler::MarkMsgsAsDeliveredAsync(std::string userId,
                                       std::vector<std::string> msgIds) {
  co_return co_await MongoConnector::instance().async_run(
      [userId = std::move(userId), msgIds = std::move(msgIds)]() {
        return MarkMsgsAsDelivered(userId, msgIds);
      });
}

bool MongoHandler::MarkMsgAsDelivered(const std::string &serverMsgID) {
  try {
    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["msg"];
    bsoncxx::builder::stream::document filter{};
    filter << "serverMsgID" << serverMsgID;
    bsoncxx::builder::stream::document update{};
    update << "$set" << bsoncxx::builder::stream::open_document << "status" << 2
           << bsoncxx::builder::stream::close_document;
    collection.update_one(filter.view(), update.view());
    spdlog::info("MarkMsgAsDelivered success, serverMsgID={}", serverMsgID);
    return true;
  } catch (const mongocxx::exception &e) {
    spdlog::error("MarkMsgAsDelivered exception: {}, serverMsgID={}", e.what(),
                  serverMsgID);
    return false;
  }
}

bool MongoHandler::MarkMsgAsPushFailed(const std::string &serverMsgID) {
  try {
    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["msg"];
    bsoncxx::builder::stream::document filter{};
    filter << "serverMsgID" << serverMsgID;
    bsoncxx::builder::stream::document update{};
    update << "$set" << bsoncxx::builder::stream::open_document << "status" << 3
           << bsoncxx::builder::stream::close_document << "$bit"
           << bsoncxx::builder::stream::open_document << "dStatus"
           << bsoncxx::builder::stream::open_document << "or" << 1
           << bsoncxx::builder::stream::close_document
           << bsoncxx::builder::stream::close_document;
    collection.update_one(filter.view(), update.view());
    spdlog::info("MarkMsgAsPushFailed success, serverMsgID={}", serverMsgID);
    return true;
  } catch (const mongocxx::exception &e) {
    spdlog::error("MarkMsgAsPushFailed exception: {}, serverMsgID={}", e.what(),
                  serverMsgID);
    return false;
  }
}

bool MongoHandler::MarkMsgAsOffline(const std::string &serverMsgID) {
  try {
    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["msg"];
    bsoncxx::builder::stream::document filter{};
    filter << "serverMsgID" << serverMsgID;
    bsoncxx::builder::stream::document update{};
    update << "$bit" << bsoncxx::builder::stream::open_document << "dStatus"
           << bsoncxx::builder::stream::open_document << "or" << 1
           << bsoncxx::builder::stream::close_document
           << bsoncxx::builder::stream::close_document;
    collection.update_one(filter.view(), update.view());
    spdlog::info("MarkMsgAsOffline success, serverMsgID={}", serverMsgID);
    return true;
  } catch (const mongocxx::exception &e) {
    spdlog::error("MarkMsgAsOffline exception: {}, serverMsgID={}", e.what(),
                  serverMsgID);
    return false;
  }
}

boost::asio::awaitable<bool>
MongoHandler::MarkMsgAsDeliveredAsync(std::string serverMsgID) {
  co_return co_await MongoConnector::instance().async_run(
      [serverMsgID = std::move(serverMsgID)]() {
        return MarkMsgAsDelivered(serverMsgID);
      });
}

boost::asio::awaitable<bool>
MongoHandler::MarkMsgAsPushFailedAsync(std::string serverMsgID) {
  co_return co_await MongoConnector::instance().async_run(
      [serverMsgID = std::move(serverMsgID)]() {
        return MarkMsgAsPushFailed(serverMsgID);
      });
}

boost::asio::awaitable<bool>
MongoHandler::MarkMsgAsOfflineAsync(std::string serverMsgID) {
  co_return co_await MongoConnector::instance().async_run(
      [serverMsgID = std::move(serverMsgID)]() {
        return MarkMsgAsOffline(serverMsgID);
      });
}

// ==================== 已消费检查（幂等兜底） ====================

bool MongoHandler::IsMsgAlreadyConsumed(const std::string &serverMsgID) {
  try {
    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["msg"];

    bsoncxx::builder::stream::document filter{};
    filter << "serverMsgID" << serverMsgID;

    mongocxx::options::find opts{};
    opts.limit(1);
    // 只取 status 和 dStatus 字段，减少 IO
    opts.projection(bsoncxx::builder::stream::document{}
                    << "status" << 1 << "dStatus" << 1 << "_id" << 0
                    << bsoncxx::builder::stream::finalize);

    auto cursor = collection.find(filter.view(), opts);
    auto it = cursor.begin();
    if (it == cursor.end()) {
      spdlog::warn("IsMsgAlreadyConsumed: msg not found, serverMsgID={}",
                   serverMsgID);
      return false; // 消息不存在，视为非终态
    }

    auto doc = *it;
    int32_t status = 0;
    int32_t dStatus = 0;
    if (auto e = doc["status"]) {
      status = e.get_int32().value;
    }
    if (auto e = doc["dStatus"]) {
      dStatus = e.get_int32().value;
    }
    // status >= 2（已送达/失败/删除/撤回）或 dStatus bit0=1（已标记离线）
    return status >= 2 || (dStatus & 1) != 0;
  } catch (const mongocxx::exception &e) {
    spdlog::error("IsMsgAlreadyConsumed exception: {}, serverMsgID={}",
                  e.what(), serverMsgID);
    return false; // MongoDB 异常时返回 false，宁重复不丢
  }
}

boost::asio::awaitable<bool>
MongoHandler::IsMsgAlreadyConsumedAsync(std::string serverMsgID) {
  co_return co_await MongoConnector::instance().async_run(
      [serverMsgID = std::move(serverMsgID)]() {
        return IsMsgAlreadyConsumed(serverMsgID);
      });
}

// ==================== 会话管理 ====================

bool MongoHandler::DoesConversationExist(const std::string &convID) {
  try {
    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["conversations"];

    bsoncxx::builder::stream::document filter{};
    filter << "conversationID" << convID;

    int64_t count = collection.count_documents(filter.view());
    return count > 0;
  } catch (const mongocxx::exception &e) {
    spdlog::error("DoesConversationExist exception: {}", e.what());
    return false;
  }
}

boost::asio::awaitable<bool>
MongoHandler::DoesConversationExistAsync(std::string convID) {
  co_return co_await MongoConnector::instance().async_run(
      [convID = std::move(convID)]() { return DoesConversationExist(convID); });
}

bool MongoHandler::CreateSingleChatConversations(const std::string &sendID,
                                                 const std::string &recvID,
                                                 const std::string &convID) {
  try {
    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["conversations"];

    // 为双方各创建一条 Conversation 记录（同一 convID，不同 ownerUserID）
    struct {
      std::string owner;
      std::string user;
    } records[2] = {{sendID, recvID}, {recvID, sendID}};

    for (const auto &r : records) {
      bsoncxx::builder::stream::document filter{};
      filter << "ownerUserID" << r.owner << "conversationID" << convID;

      bsoncxx::builder::stream::document doc{};
      doc << "ownerUserID" << r.owner << "conversationID" << convID
          << "conversationType" << 1 // 单聊
          << "userID" << r.user << "groupID"
          << ""
          << "isPinned" << false << "isPrivateChat" << false << "recvMsgOpt"
          << 0 << "groupAtType" << 0 << "burnDuration" << 0 << "minSeq"
          << int64_t{0} << "maxSeq" << int64_t{0} << "msgDestructTime"
          << int64_t{0} << "latestMsgDestructTime" << int64_t{0}
          << "isMsgDestruct" << false << "ex"
          << ""
          << "attachedInfo"
          << "";

      bsoncxx::builder::stream::document update{};
      update << "$setOnInsert" << doc.view();

      mongocxx::options::update opts{};
      opts.upsert(true);
      collection.update_one(filter.view(), update.view(), opts);
    }

    spdlog::info(
        "CreateSingleChatConversations: convID={}, sendID={}, recvID={}",
        convID, sendID, recvID);
    return true;
  } catch (const mongocxx::exception &e) {
    spdlog::error("CreateSingleChatConversations exception: {}", e.what());
    return false;
  }
}

boost::asio::awaitable<bool> MongoHandler::CreateSingleChatConversationsAsync(
    std::string sendID, std::string recvID, std::string convID) {
  co_return co_await MongoConnector::instance().async_run(
      [sendID = std::move(sendID), recvID = std::move(recvID),
       convID = std::move(convID)]() {
        return CreateSingleChatConversations(sendID, recvID, convID);
      });
}

// ── 创建群聊会话 ──

bool MongoHandler::CreateGroupChatConversations(
    const std::string &groupID, const std::vector<std::string> &userIDs) {
  try {
    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["conversations"];

    for (const auto &uid : userIDs) {
      bsoncxx::builder::stream::document filter{};
      filter << "ownerUserID" << uid << "conversationID" << groupID;

      bsoncxx::builder::stream::document doc{};
      doc << "ownerUserID" << uid << "conversationID" << groupID
          << "conversationType" << 2 // 群聊
          << "userID" << uid << "groupID" << groupID << "isPinned" << false
          << "isPrivateChat" << false << "recvMsgOpt" << 0 << "groupAtType" << 0
          << "burnDuration" << 0 << "minSeq" << int64_t{0} << "maxSeq"
          << int64_t{0} << "msgDestructTime" << int64_t{0}
          << "latestMsgDestructTime" << int64_t{0} << "isMsgDestruct" << false
          << "ex"
          << ""
          << "attachedInfo"
          << "";

      bsoncxx::builder::stream::document update{};
      update << "$setOnInsert" << doc.view();

      mongocxx::options::update opts{};
      opts.upsert(true);
      collection.update_one(filter.view(), update.view(), opts);
    }

    spdlog::info("CreateGroupChatConversations: groupID={}, memberCount={}",
                 groupID, userIDs.size());
    return true;
  } catch (const mongocxx::exception &e) {
    spdlog::error("CreateGroupChatConversations exception: {}", e.what());
    return false;
  }
}

boost::asio::awaitable<bool> MongoHandler::CreateGroupChatConversationsAsync(
    std::string groupID, std::vector<std::string> userIDs) {
  co_return co_await MongoConnector::instance().async_run(
      [groupID = std::move(groupID), userIDs = std::move(userIDs)]() {
        return CreateGroupChatConversations(groupID, userIDs);
      });
}

// ── 按群 ID 列表批量查询群消息 ──

std::vector<sdkws::MsgData> MongoHandler::GetGroupMsgsBySeqFromMongo(
    const std::vector<std::string> &groupIDs, int64_t afterSeq, int limit) {
  std::vector<sdkws::MsgData> messages;
  if (groupIDs.empty())
    return messages;

  try {
    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["msg"];

    using bsoncxx::builder::stream::close_array;
    using bsoncxx::builder::stream::close_document;
    using bsoncxx::builder::stream::document;
    using bsoncxx::builder::stream::open_array;
    using bsoncxx::builder::stream::open_document;

    // Build $in array for groupIDs
    bsoncxx::builder::stream::array group_array;
    for (const auto &gid : groupIDs) {
      group_array << gid;
    }

    document filter{};
    filter << "isGroupMsg" << true << "convID" << open_document << "$in"
           << group_array.view() << close_document << "seq" << open_document
           << "$gt" << afterSeq << close_document;

    mongocxx::options::find opts{};
    opts.limit(limit);
    opts.sort(document{} << "seq" << int32_t{1}
                         << bsoncxx::builder::stream::finalize);

    auto cursor = collection.find(filter.view(), opts);
    for (auto &doc : cursor) {
      try {
        sdkws::MsgData msg;
        auto json = bsoncxx::to_json(doc);
        auto parseStatus =
            google::protobuf::util::JsonStringToMessage(json, &msg);
        if (!parseStatus.ok()) {
          spdlog::warn(
              "GetGroupMsgsBySeqFromMongo: JsonStringToMessage failed: {}",
              parseStatus.message());
          continue;
        }
        messages.push_back(std::move(msg));
      } catch (const std::exception &e) {
        spdlog::warn("GetGroupMsgsBySeqFromMongo: parse error: {}", e.what());
      }
    }

    spdlog::debug(
        "GetGroupMsgsBySeqFromMongo: groupIDs.size()={}, afterSeq={}, "
        "found={}",
        groupIDs.size(), afterSeq, messages.size());
  } catch (const mongocxx::exception &e) {
    spdlog::error("GetGroupMsgsBySeqFromMongo exception: {}", e.what());
  }
  return messages;
}

boost::asio::awaitable<std::vector<sdkws::MsgData>>
MongoHandler::GetGroupMsgsBySeqFromMongoAsync(std::vector<std::string> groupIDs,
                                              int64_t afterSeq, int limit) {
  co_return co_await MongoConnector::instance().async_run(
      [groupIDs = std::move(groupIDs), afterSeq, limit]() {
        return GetGroupMsgsBySeqFromMongo(groupIDs, afterSeq, limit);
      });
}
