// C++ 重构版本：消息服务头文件
// 原始 Go 代码由 Kitex v0.13.1 生成

#ifndef MESSAGESERVICE_H
#define MESSAGESERVICE_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <stdexcept>
#include <stdint.h>

// 前置声明
namespace sdkws {
    struct MsgData;
    struct UserInfo;
    struct GroupInfo;
    struct RequestPagination;
}

// 上下文类
class Context {
public:
    Context() = default;
    // 可以添加上下文相关的方法
};

// 请求/响应结构体定义
namespace msg {
    // 消息数据传输结构体
    struct MsgDataToMQ {
        std::string token;
        sdkws::MsgData* msgData;
    };

    struct MsgDataToDB {
        sdkws::MsgData* msgData;
    };

    struct PushMsgDataToMQ {
        sdkws::MsgData* msgData;
        std::string conversationID;
    };

    struct MsgDataToMongoByMQ {
        int64_t lastSeq;
        std::string conversationID;
        std::vector<sdkws::MsgData*> msgData;
    };

    // 请求/响应结构体
    struct GetMaxSeqReq {
        // 这里可以根据实际需求添加字段
    };

    struct GetMaxSeqResp {
        int64_t maxSeq;
    };

    struct GetMaxSeqsReq {
        std::vector<std::string> conversationIDs;
    };

    struct SeqsInfoResp {
        std::map<std::string, int64_t> maxSeqs;
    };

    struct GetHasReadSeqsReq {
        std::string userID;
        std::vector<std::string> conversationIDs;
    };

    struct GetMsgByConversationIDsReq {
        std::vector<std::string> conversationIDs;
        std::map<std::string, int64_t> maxSeqs;
    };

    struct GetMsgByConversationIDsResp {
        std::map<std::string, sdkws::MsgData*> msgDatas;
    };

    struct GetConversationMaxSeqReq {
        std::string conversationID;
    };

    struct GetConversationMaxSeqResp {
        int64_t maxSeq;
    };

    struct PullMessageBySeqsReq {
        // 这里可以根据实际需求添加字段
    };

    struct PullMessageBySeqsResp {
        // 这里可以根据实际需求添加字段
    };

    struct GetSeqMessageReq {
        // 这里可以根据实际需求添加字段
    };

    struct GetSeqMessageResp {
        // 这里可以根据实际需求添加字段
    };

    struct SearchMessageReq {
        std::string sendID;
        std::string recvID;
        int32_t contentType;
        std::string sendTime;
        int32_t sessionType;
        sdkws::RequestPagination* pagination;
    };

    struct ChatLog {
        std::string serverMsgID;
        std::string clientMsgID;
        std::string sendID;
        std::string recvID;
        std::string groupID;
        std::string recvNickname;
        int32_t senderPlatformID;
        std::string senderNickname;
        std::string senderFaceURL;
        std::string groupName;
        int32_t sessionType;
        int32_t msgFrom;
        int32_t contentType;
        std::string content;
        int32_t status;
        int64_t sendTime;
        int64_t createTime;
        std::string ex;
        std::string groupFaceURL;
        uint32_t groupMemberCount;
        int64_t seq;
        std::string groupOwner;
        int32_t groupType;
    };

    struct SearchChatLog {
        ChatLog* chatLog;
        bool isRevoked;
    };

    struct SearchMessageResp {
        std::vector<SearchChatLog*> chatLogs;
        int32_t chatLogsNum;
    };

    struct SendMessageReq {
        // 这里可以根据实际需求添加字段
    };

    struct SendMessageResp {
        std::string serverMsgID;
        std::string clientMsgID;
        int64_t sendTime;
        sdkws::MsgData* modify;
    };

    struct SendSimpleMsgReq {
        sdkws::MsgData* msgData;
    };

    struct SendSimpleMsgResp {
        std::string serverMsgID;
        std::string clientMsgID;
        int64_t sendTime;
        sdkws::MsgData* modify;
    };

    struct SetUserConversationsMinSeqReq {
        // 这里可以根据实际需求添加字段
    };

    struct SetUserConversationsMinSeqResp {
        // 这里可以根据实际需求添加字段
    };

    struct DeleteSyncOpt {
        bool isSyncSelf;
        bool isSyncOther;
    };

    struct ClearConversationsMsgReq {
        std::vector<std::string> conversationIDs;
        std::string userID;
        DeleteSyncOpt* deleteSyncOpt;
    };

    struct ClearConversationsMsgResp {
        // 这里可以根据实际需求添加字段
    };

    struct UserClearAllMsgReq {
        std::string userID;
        DeleteSyncOpt* deleteSyncOpt;
    };

    struct UserClearAllMsgResp {
        // 这里可以根据实际需求添加字段
    };

    struct DeleteMsgsReq {
        std::string conversationID;
        std::vector<int64_t> seqs;
        std::string userID;
        DeleteSyncOpt* deleteSyncOpt;
    };

    struct DeleteMsgsResp {
        // 这里可以根据实际需求添加字段
    };

    struct DeleteMsgPhysicalBySeqReq {
        std::string conversationID;
        std::vector<int64_t> seqs;
    };

    struct DeleteMsgPhysicalBySeqResp {
        // 这里可以根据实际需求添加字段
    };

    struct DeleteMsgPhysicalReq {
        std::vector<std::string> conversationIDs;
        int64_t timestamp;
    };

    struct DeleteMsgPhysicalResp {
        // 这里可以根据实际需求添加字段
    };

    struct SetSendMsgStatusReq {
        int32_t status;
    };

    struct SetSendMsgStatusResp {
        // 这里可以根据实际需求添加字段
    };

    struct GetSendMsgStatusReq {
        // 这里可以根据实际需求添加字段
    };

    struct GetSendMsgStatusResp {
        int32_t status;
    };

    struct RevokeMsgReq {
        std::string conversationID;
        int64_t seq;
        std::string userID;
    };

    struct RevokeMsgResp {
        // 这里可以根据实际需求添加字段
    };

    struct MarkMsgsAsReadReq {
        std::string conversationID;
        std::vector<int64_t> seqs;
        std::string userID;
    };

    struct MarkMsgsAsReadResp {
        // 这里可以根据实际需求添加字段
    };

    struct MarkConversationAsReadReq {
        std::string conversationID;
        std::string userID;
        int64_t hasReadSeq;
        std::vector<int64_t> seqs;
    };

    struct MarkConversationAsReadResp {
        // 这里可以根据实际需求添加字段
    };

    struct SetConversationHasReadSeqReq {
        std::string conversationID;
        std::string userID;
        int64_t hasReadSeq;
        bool noNotification;
    };

    struct SetConversationHasReadSeqResp {
        // 这里可以根据实际需求添加字段
    };

    struct Seqs {
        int64_t maxSeq;
        int64_t hasReadSeq;
        int64_t maxSeqTime;
    };

    struct GetConversationsHasReadAndMaxSeqReq {
        std::string userID;
        std::vector<std::string> conversationIDs;
        bool returnPinned;
    };

    struct GetConversationsHasReadAndMaxSeqResp {
        std::map<std::string, Seqs*> seqs;
        std::vector<std::string> pinnedConversationIDs;
    };

    struct ActiveUser {
        sdkws::UserInfo* user;
        int64_t count;
    };

    struct GetActiveUserReq {
        int64_t start;
        int64_t end;
        bool ase;
        bool group;
        sdkws::RequestPagination* pagination;
    };

    struct GetActiveUserResp {
        int64_t msgCount;
        int64_t userCount;
        std::map<std::string, int64_t> dateCount;
        std::vector<ActiveUser*> users;
    };

    struct ActiveGroup {
        sdkws::GroupInfo* group;
        int64_t count;
    };

    struct GetActiveGroupReq {
        int64_t start;
        int64_t end;
        bool ase;
        sdkws::RequestPagination* pagination;
    };

    struct GetActiveGroupResp {
        int64_t msgCount;
        int64_t groupCount;
        std::map<std::string, int64_t> dateCount;
        std::vector<ActiveGroup*> groups;
    };

    struct GetServerTimeReq {
        // 这里可以根据实际需求添加字段
    };

    struct GetServerTimeResp {
        int64_t serverTime;
    };

    struct ClearMsgReq {
        // 这里可以根据实际需求添加字段
    };

    struct ClearMsgResp {
        // 这里可以根据实际需求添加字段
    };

    struct DestructMsgsReq {
        // 这里可以根据实际需求添加字段
    };

    struct DestructMsgsResp {
        // 这里可以根据实际需求添加字段
    };

    struct GetActiveConversationReq {
        // 这里可以根据实际需求添加字段
    };

    struct GetActiveConversationResp {
        // 这里可以根据实际需求添加字段
    };

    struct SetUserConversationMaxSeqReq {
        // 这里可以根据实际需求添加字段
    };

    struct SetUserConversationMaxSeqResp {
        // 这里可以根据实际需求添加字段
    };

    struct SetUserConversationMinSeqReq {
        // 这里可以根据实际需求添加字段
    };

    struct SetUserConversationMinSeqResp {
        // 这里可以根据实际需求添加字段
    };

    struct GetLastMessageSeqByTimeReq {
        // 这里可以根据实际需求添加字段
    };

    struct GetLastMessageSeqByTimeResp {
        int64_t seq;
    };

    struct GetLastMessageReq {
        // 这里可以根据实际需求添加字段
    };

    struct GetLastMessageResp {
        sdkws::MsgData* msgData;
    };
}

// 客户端选项
struct ClientOption {
    std::string name;
    std::string value;
};

// 服务器选项
struct ServerOption {
    std::string name;
    std::string value;
};

// 服务器接口类
class MessageService {
public:
    virtual ~MessageService() = default;
    
    // 所有服务方法的纯虚函数声明
    virtual msg::GetMaxSeqResp GetMaxSeq(const Context& ctx, const msg::GetMaxSeqReq& req) = 0;
    virtual msg::SeqsInfoResp GetMaxSeqs(const Context& ctx, const msg::GetMaxSeqsReq& req) = 0;
    virtual msg::SeqsInfoResp GetHasReadSeqs(const Context& ctx, const msg::GetHasReadSeqsReq& req) = 0;
    virtual msg::GetMsgByConversationIDsResp GetMsgByConversationIDs(const Context& ctx, const msg::GetMsgByConversationIDsReq& req) = 0;
    virtual msg::GetConversationMaxSeqResp GetConversationMaxSeq(const Context& ctx, const msg::GetConversationMaxSeqReq& req) = 0;
    virtual msg::PullMessageBySeqsResp PullMessageBySeqs(const Context& ctx, const msg::PullMessageBySeqsReq& req) = 0;
    virtual msg::GetSeqMessageResp GetSeqMessage(const Context& ctx, const msg::GetSeqMessageReq& req) = 0;
    virtual msg::SearchMessageResp SearchMessage(const Context& ctx, const msg::SearchMessageReq& req) = 0;
    virtual msg::SendMessageResp SendMessages(const Context& ctx, const msg::SendMessageReq& req) = 0;
    virtual msg::SendSimpleMsgResp SendSimpleMsg(const Context& ctx, const msg::SendSimpleMsgReq& req) = 0;
    virtual msg::SetUserConversationsMinSeqResp SetUserConversationsMinSeq(const Context& ctx, const msg::SetUserConversationsMinSeqReq& req) = 0;
    virtual msg::ClearConversationsMsgResp ClearConversationsMsg(const Context& ctx, const msg::ClearConversationsMsgReq& req) = 0;
    virtual msg::UserClearAllMsgResp UserClearAllMsg(const Context& ctx, const msg::UserClearAllMsgReq& req) = 0;
    virtual msg::DeleteMsgsResp DeleteMsgs(const Context& ctx, const msg::DeleteMsgsReq& req) = 0;
    virtual msg::DeleteMsgPhysicalBySeqResp DeleteMsgPhysicalBySeq(const Context& ctx, const msg::DeleteMsgPhysicalBySeqReq& req) = 0;
    virtual msg::DeleteMsgPhysicalResp DeleteMsgPhysical(const Context& ctx, const msg::DeleteMsgPhysicalReq& req) = 0;
    virtual msg::SetSendMsgStatusResp SetSendMsgStatus(const Context& ctx, const msg::SetSendMsgStatusReq& req) = 0;
    virtual msg::GetSendMsgStatusResp GetSendMsgStatus(const Context& ctx, const msg::GetSendMsgStatusReq& req) = 0;
    virtual msg::RevokeMsgResp RevokeMsg(const Context& ctx, const msg::RevokeMsgReq& req) = 0;
    virtual msg::MarkMsgsAsReadResp MarkMsgsAsRead(const Context& ctx, const msg::MarkMsgsAsReadReq& req) = 0;
    virtual msg::MarkConversationAsReadResp MarkConversationAsRead(const Context& ctx, const msg::MarkConversationAsReadReq& req) = 0;
    virtual msg::SetConversationHasReadSeqResp SetConversationHasReadSeq(const Context& ctx, const msg::SetConversationHasReadSeqReq& req) = 0;
    virtual msg::GetConversationsHasReadAndMaxSeqResp GetConversationsHasReadAndMaxSeq(const Context& ctx, const msg::GetConversationsHasReadAndMaxSeqReq& req) = 0;
    virtual msg::GetActiveUserResp GetActiveUser(const Context& ctx, const msg::GetActiveUserReq& req) = 0;
    virtual msg::GetActiveGroupResp GetActiveGroup(const Context& ctx, const msg::GetActiveGroupReq& req) = 0;
    virtual msg::GetServerTimeResp GetServerTime(const Context& ctx, const msg::GetServerTimeReq& req) = 0;
    virtual msg::ClearMsgResp ClearMsg(const Context& ctx, const msg::ClearMsgReq& req) = 0;
    virtual msg::DestructMsgsResp DestructMsgs(const Context& ctx, const msg::DestructMsgsReq& req) = 0;
    virtual msg::GetActiveConversationResp GetActiveConversation(const Context& ctx, const msg::GetActiveConversationReq& req) = 0;
    virtual msg::SetUserConversationMaxSeqResp SetUserConversationMaxSeq(const Context& ctx, const msg::SetUserConversationMaxSeqReq& req) = 0;
    virtual msg::SetUserConversationMinSeqResp SetUserConversationMinSeq(const Context& ctx, const msg::SetUserConversationMinSeqReq& req) = 0;
    virtual msg::GetLastMessageSeqByTimeResp GetLastMessageSeqByTime(const Context& ctx, const msg::GetLastMessageSeqByTimeReq& req) = 0;
    virtual msg::GetLastMessageResp GetLastMessage(const Context& ctx, const msg::GetLastMessageReq& req) = 0;
};

// 客户端类
class MessageServiceClient {
private:
    std::string serviceName;
    std::vector<ClientOption> options;
    
public:
    // 构造函数
    MessageServiceClient(const std::string& serviceName, const std::vector<ClientOption>& options = {});
    
    // 客户端方法声明
    msg::GetMaxSeqResp GetMaxSeq(const Context& ctx, const msg::GetMaxSeqReq& req);
    msg::SeqsInfoResp GetMaxSeqs(const Context& ctx, const msg::GetMaxSeqsReq& req);
    msg::SeqsInfoResp GetHasReadSeqs(const Context& ctx, const msg::GetHasReadSeqsReq& req);
    msg::GetMsgByConversationIDsResp GetMsgByConversationIDs(const Context& ctx, const msg::GetMsgByConversationIDsReq& req);
    msg::GetConversationMaxSeqResp GetConversationMaxSeq(const Context& ctx, const msg::GetConversationMaxSeqReq& req);
    msg::PullMessageBySeqsResp PullMessageBySeqs(const Context& ctx, const msg::PullMessageBySeqsReq& req);
    msg::GetSeqMessageResp GetSeqMessage(const Context& ctx, const msg::GetSeqMessageReq& req);
    msg::SearchMessageResp SearchMessage(const Context& ctx, const msg::SearchMessageReq& req);
    msg::SendMessageResp SendMessages(const Context& ctx, const msg::SendMessageReq& req);
    msg::SendSimpleMsgResp SendSimpleMsg(const Context& ctx, const msg::SendSimpleMsgReq& req);
    msg::SetUserConversationsMinSeqResp SetUserConversationsMinSeq(const Context& ctx, const msg::SetUserConversationsMinSeqReq& req);
    msg::ClearConversationsMsgResp ClearConversationsMsg(const Context& ctx, const msg::ClearConversationsMsgReq& req);
    msg::UserClearAllMsgResp UserClearAllMsg(const Context& ctx, const msg::UserClearAllMsgReq& req);
    msg::DeleteMsgsResp DeleteMsgs(const Context& ctx, const msg::DeleteMsgsReq& req);
    msg::DeleteMsgPhysicalBySeqResp DeleteMsgPhysicalBySeq(const Context& ctx, const msg::DeleteMsgPhysicalBySeqReq& req);
    msg::DeleteMsgPhysicalResp DeleteMsgPhysical(const Context& ctx, const msg::DeleteMsgPhysicalReq& req);
    msg::SetSendMsgStatusResp SetSendMsgStatus(const Context& ctx, const msg::SetSendMsgStatusReq& req);
    msg::GetSendMsgStatusResp GetSendMsgStatus(const Context& ctx, const msg::GetSendMsgStatusReq& req);
    msg::RevokeMsgResp RevokeMsg(const Context& ctx, const msg::RevokeMsgReq& req);
    msg::MarkMsgsAsReadResp MarkMsgsAsRead(const Context& ctx, const msg::MarkMsgsAsReadReq& req);
    msg::MarkConversationAsReadResp MarkConversationAsRead(const Context& ctx, const msg::MarkConversationAsReadReq& req);
    msg::SetConversationHasReadSeqResp SetConversationHasReadSeq(const Context& ctx, const msg::SetConversationHasReadSeqReq& req);
    msg::GetConversationsHasReadAndMaxSeqResp GetConversationsHasReadAndMaxSeq(const Context& ctx, const msg::GetConversationsHasReadAndMaxSeqReq& req);
    msg::GetActiveUserResp GetActiveUser(const Context& ctx, const msg::GetActiveUserReq& req);
    msg::GetActiveGroupResp GetActiveGroup(const Context& ctx, const msg::GetActiveGroupReq& req);
    msg::GetServerTimeResp GetServerTime(const Context& ctx, const msg::GetServerTimeReq& req);
    msg::ClearMsgResp ClearMsg(const Context& ctx, const msg::ClearMsgReq& req);
    msg::DestructMsgsResp DestructMsgs(const Context& ctx, const msg::DestructMsgsReq& req);
    msg::GetActiveConversationResp GetActiveConversation(const Context& ctx, const msg::GetActiveConversationReq& req);
    msg::SetUserConversationMaxSeqResp SetUserConversationMaxSeq(const Context& ctx, const msg::SetUserConversationMaxSeqReq& req);
    msg::SetUserConversationMinSeqResp SetUserConversationMinSeq(const Context& ctx, const msg::SetUserConversationMinSeqReq& req);
    msg::GetLastMessageSeqByTimeResp GetLastMessageSeqByTime(const Context& ctx, const msg::GetLastMessageSeqByTimeReq& req);
    msg::GetLastMessageResp GetLastMessage(const Context& ctx, const msg::GetLastMessageReq& req);
    
    // 辅助方法
    void connect();
    void disconnect();
};

// 服务器类
class MessageServiceServer {
private:
    // 这里可以添加服务器内部状态
    bool running;
    
public:
    // 构造函数
    MessageServiceServer(std::shared_ptr<MessageService> handler, const std::vector<ServerOption>& options = {});
    
    // 服务器控制方法
    void start();
    void stop();
    bool isRunning() const;
};

// 工厂函数
std::unique_ptr<MessageServiceClient> NewClient(const std::string& serviceName, const std::vector<ClientOption>& options = {});
std::unique_ptr<MessageServiceServer> NewServer(std::shared_ptr<MessageService> handler, const std::vector<ServerOption>& options = {});

#endif // MESSAGESERVICE_H