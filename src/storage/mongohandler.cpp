#include <bsoncxx/builder/stream/array.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <cstdint>
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
#include <stdexcept>
#include <cmath>

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
bool MongoHandler::SaveMsgToMongo(const sdkws::MsgData &msg, bool committed) {
  try {
    // 直接获取 pool entry 并保持其生命周期直到 insert_one 完成，
    // 避免 get_collection() 返回后 pool entry 析构导致 client 失效
    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["msg"];

    auto doc = bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp("serverMsgID", msg.servermsgid()),
        bsoncxx::builder::basic::kvp("sendID", static_cast<int64_t>(msg.sendid())),
        bsoncxx::builder::basic::kvp("recvID", static_cast<int64_t>(msg.recvid())),
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
        bsoncxx::builder::basic::kvp("ext", msg.ext()),
        bsoncxx::builder::basic::kvp("committed", committed)
      );

    // 观察doc的值
    // auto view = doc.view();
    // spdlog::info("Inserting doc: {}", bsoncxx::to_json(view));

    auto filter = bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp("serverMsgID", msg.servermsgid()));
    auto update = bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp("$setOnInsert", doc.view()));
    mongocxx::options::update options;
    options.upsert(true);
    auto result = collection.update_one(filter.view(), update.view(), options);
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

    outMsg = DecodeMessage(doc);

    spdlog::info("GetMsgByServerMsgID success, serverMsgID={}", serverMsgID);
    return true;
  } catch (const mongocxx::exception &e) {
    spdlog::error("GetMsgByServerMsgID exception: {}, serverMsgID={}", e.what(),
                  serverMsgID);
    throw;
  }
}

std::vector<sdkws::MsgData>
MongoHandler::GetMsgsBySeqFromMongo(const uint64_t userId, int64_t afterSeq,
                                    int limit) {
  std::vector<sdkws::MsgData> results;
  if (limit <= 0 || !userId) return results;
  try {
    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["msg"];

    bsoncxx::builder::stream::document filter{};
    filter << "recvID" << static_cast<int64_t>(userId) << "seq"
           << bsoncxx::builder::stream::open_document << "$gt" << afterSeq
           << bsoncxx::builder::stream::close_document;
    // 此查询只为单聊恢复提供严格升序候选；新群聊由 GroupService 独立同步。
    filter << "isGroupMsg" << bsoncxx::builder::stream::open_document << "$ne" << true
           << bsoncxx::builder::stream::close_document;
    filter << "sessionType" << 1;
    filter << "committed" << bsoncxx::builder::stream::open_document << "$ne" << false
           << bsoncxx::builder::stream::close_document;
    filter << "status" << bsoncxx::builder::stream::open_document << "$ne" << 2
           << bsoncxx::builder::stream::close_document;

    mongocxx::options::find opts{};
    opts.sort(bsoncxx::builder::stream::document{}
              << "seq" << 1 << bsoncxx::builder::stream::finalize);
    opts.limit(limit);

    auto cursor = collection.find(filter.view(), opts);

    for (auto &&doc : cursor) {
      try {
        auto msg = DecodeMessage(doc);
        results.push_back(std::move(msg));
      } catch (...) {
        spdlog::error("GetMsgsBySeqFromMongo parse failed for user={}", userId);
        throw;
      }
    }

    spdlog::info("GetMsgsBySeqFromMongo success, userId={}, count={}", userId,
                 results.size());
    return results;
  } catch (const mongocxx::exception &e) {
    spdlog::error("GetMsgsBySeqFromMongo exception: {}, userId={}", e.what(),
                  userId);
    throw;
  }
}

bool MongoHandler::MarkMsgsAsDelivered(const uint64_t userId,
                                       const std::vector<std::string> &msgIds) {
  try {
    if (msgIds.empty())
      return true;

    auto &mongoconnector = MongoConnector::instance();
    auto client = mongoconnector.acquire_client();
    auto collection = (*client)["IM-System"]["msg"];

    for (const auto &msgId : msgIds) {
      bsoncxx::builder::stream::document filter{};
      filter << "recvID" << static_cast<int64_t>(userId) << "serverMsgID" << msgId;

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
MongoHandler::SaveMsgToMongoAsync(sdkws::MsgData msg, bool committed) {
  co_return co_await MongoConnector::instance().async_run(
      [msg = std::move(msg), committed]() mutable { return SaveMsgToMongo(msg, committed); });
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
MongoHandler::GetMsgsBySeqFromMongoAsync(uint64_t userId, int64_t afterSeq,
                                         int limit) {
  co_return co_await MongoConnector::instance().async_run(
      [userId = std::move(userId), afterSeq, limit]() {
        return GetMsgsBySeqFromMongo(userId, afterSeq, limit);
      });
}

boost::asio::awaitable<bool>
MongoHandler::MarkMsgsAsDeliveredAsync(uint64_t userId,
                                       std::vector<std::string> msgIds) {
  co_return co_await MongoConnector::instance().async_run(
      [userId = std::move(userId), msgIds = std::move(msgIds)]() {
        return MarkMsgsAsDelivered(userId, msgIds);
      });
}

namespace {
boost::asio::awaitable<std::vector<std::string>>
singleChatHistory(std::string conversation, bool erase, int64_t cutoff)
{
    co_return co_await MongoConnector::instance().async_run([conversation, erase, cutoff] {
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;
        auto client = MongoConnector::instance().acquire_client();
        auto db = (*client)["IM-System"];
        auto filter = make_document(kvp("convID", conversation), kvp("isGroupMsg", false),
            kvp("seq", make_document(kvp("$lte", cutoff))));
        std::vector<std::string> ids;
        for (auto doc : db["msg"].find(filter.view())) {
            if (auto id = doc["serverMsgID"])
                ids.emplace_back(id.get_string().value);
        }
        if (erase && !db["msg"].delete_many(filter.view()))
            throw std::runtime_error("Unable to delete MongoDB chat messages");
        return ids;
    });
}

}
boost::asio::awaitable<std::vector<std::string>> MongoHandler::GetSingleChatMessageIDsAsync(std::string conversation, int64_t cutoff)
{
    co_return co_await singleChatHistory(std::move(conversation), false, cutoff);
}
boost::asio::awaitable<std::vector<sdkws::MsgData>> MongoHandler::GetSingleChatHistoryAsync(
    std::string conversation, int64_t afterSeq, int limit)
{
    co_return co_await MongoConnector::instance().async_run(
        [conversation = std::move(conversation), afterSeq, limit] {
            using bsoncxx::builder::basic::kvp;
            using bsoncxx::builder::basic::make_document;
            auto client = MongoConnector::instance().acquire_client();
            auto collection = (*client)["IM-System"]["msg"];
            auto filter = make_document(kvp("convID", conversation), kvp("isGroupMsg", false),
                kvp("committed", make_document(kvp("$ne", false))),
                kvp("seq", make_document(kvp("$gt", afterSeq))));
            mongocxx::options::find options;
            options.limit(limit);
            options.sort(make_document(kvp("seq", 1)));
            std::vector<sdkws::MsgData> result;
            for (auto doc : collection.find(filter.view(), options)) {
                sdkws::MsgData message;
                if (auto field = doc["serverMsgID"])
                    message.set_servermsgid(std::string(field.get_string().value));
                if (auto field = doc["clientMsgID"])
                    message.set_clientmsgid(std::string(field.get_string().value));
                if (auto field = doc["sendID"])
                    message.set_sendid(field.get_int64().value);
                if (auto field = doc["recvID"])
                    message.set_recvid(field.get_int64().value);
                if (auto field = doc["seq"])
                    message.set_seq(field.get_int64().value);
                if (auto field = doc["sendTime"]) {
                    if (field.type() == bsoncxx::type::k_double)
                        message.set_sendtime(field.get_double().value);
                    else if (field.type() == bsoncxx::type::k_int64)
                        message.set_sendtime(field.get_int64().value);
                }
                if (auto field = doc["contentType"])
                    message.set_contenttype(field.get_int32().value);
                if (auto field = doc["content"])
                    message.set_content(std::string(field.get_string().value));
                if (auto field = doc["senderNickname"])
                    message.set_sendernickname(std::string(field.get_string().value));
                message.set_convid(conversation);
                message.set_sessiontype(1);
                result.push_back(std::move(message));
            }
            return result;
        });
}
boost::asio::awaitable<std::vector<std::string>> MongoHandler::DeleteSingleChatHistoryAsync(std::string conversation, int64_t cutoff)
{
    co_return co_await singleChatHistory(std::move(conversation), true, cutoff);
}

boost::asio::awaitable<void> MongoHandler::PublishMessage(std::string serverId)
{
    co_await MongoConnector::instance().async_run([serverId = std::move(serverId)] {
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;
        auto client = MongoConnector::instance().acquire_client();
        auto filter = make_document(kvp("serverMsgID", serverId));
        auto update = make_document(kvp("$set", make_document(kvp("committed", true))));
        auto result = (*client)["IM-System"]["msg"].update_one(filter.view(), update.view());
        if (!result || result->matched_count() == 0)
            throw std::runtime_error("Message body is missing");
    });
}

sdkws::MsgData MongoHandler::DecodeMessage(bsoncxx::document::view doc) {
    sdkws::MsgData message;
    auto integer=[](bsoncxx::document::element element)->int64_t {
        if(element.type()==bsoncxx::type::k_int64) return element.get_int64().value;
        if(element.type()==bsoncxx::type::k_int32) return element.get_int32().value;
        if(element.type()==bsoncxx::type::k_double) {
            auto value=element.get_double().value;
            if(std::isfinite(value) && std::trunc(value)==value && value>=0 && value<9223372036854775808.0)
                return static_cast<int64_t>(value);
        }
        throw std::runtime_error("Invalid numeric message metadata");
    };
#define READ_STRING(field,setter) if(auto value=doc[field]) message.setter(std::string{value.get_string().value});
#define READ_INTEGER(field,setter) if(auto value=doc[field]) message.setter(integer(value));
#define READ_BOOL(field,setter) if(auto value=doc[field]) message.setter(value.get_bool().value);
    READ_STRING("serverMsgID",set_servermsgid)
    READ_STRING("convID",set_convid)
    READ_STRING("clientMsgID",set_clientmsgid)
    READ_STRING("senderNickname",set_sendernickname)
    READ_STRING("senderFaceURL",set_senderfaceurl)
    READ_STRING("content",set_content)
    READ_STRING("ext",set_ext)
    READ_INTEGER("sendID",set_sendid)
    READ_INTEGER("recvID",set_recvid)
    READ_INTEGER("seq",set_seq)
    READ_INTEGER("sendTime",set_sendtime)
    READ_INTEGER("senderPlatformID",set_senderplatformid)
    READ_INTEGER("sessionType",set_sessiontype)
    READ_INTEGER("msgFrom",set_msgfrom)
    READ_INTEGER("contentType",set_contenttype)
    READ_INTEGER("status",set_status)
    READ_BOOL("isRead",set_isread)
    READ_BOOL("isDeleted",set_isdeleted)
    READ_BOOL("isRecalled",set_isrecalled)
    READ_BOOL("isPinned",set_ispinned)
    READ_BOOL("isGroupMsg",set_isgroupmsg)
#undef READ_STRING
#undef READ_INTEGER
#undef READ_BOOL
    return message;
}
