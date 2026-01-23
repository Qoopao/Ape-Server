// C++ 重构版本：消息服务示例实现
// 这个文件展示了如何使用 MessageService 接口和服务器/客户端类

#include "messageservice.h"
#include <iostream>

// 示例消息服务实现
class ExampleMessageService : public MessageService {
public:
    // 实现所有纯虚函数
    msg::GetMaxSeqResp GetMaxSeq(const Context& ctx, const msg::GetMaxSeqReq& req) override {
        msg::GetMaxSeqResp resp;
        resp.maxSeq = 1000; // 示例值
        std::cout << "ExampleMessageService::GetMaxSeq called" << std::endl;
        return resp;
    }
    
    msg::SeqsInfoResp GetMaxSeqs(const Context& ctx, const msg::GetMaxSeqsReq& req) override {
        msg::SeqsInfoResp resp;
        for (const auto& conversationID : req.conversationIDs) {
            resp.maxSeqs[conversationID] = 2000; // 示例值
        }
        std::cout << "ExampleMessageService::GetMaxSeqs called" << std::endl;
        return resp;
    }
    
    msg::SeqsInfoResp GetHasReadSeqs(const Context& ctx, const msg::GetHasReadSeqsReq& req) override {
        msg::SeqsInfoResp resp;
        for (const auto& conversationID : req.conversationIDs) {
            resp.maxSeqs[conversationID] = 1500; // 示例值
        }
        std::cout << "ExampleMessageService::GetHasReadSeqs called" << std::endl;
        return resp;
    }
    
    msg::GetMsgByConversationIDsResp GetMsgByConversationIDs(const Context& ctx, const msg::GetMsgByConversationIDsReq& req) override {
        msg::GetMsgByConversationIDsResp resp;
        // 示例实现，实际项目中需要返回真实的消息数据
        std::cout << "ExampleMessageService::GetMsgByConversationIDs called" << std::endl;
        return resp;
    }
    
    msg::GetConversationMaxSeqResp GetConversationMaxSeq(const Context& ctx, const msg::GetConversationMaxSeqReq& req) override {
        msg::GetConversationMaxSeqResp resp;
        resp.maxSeq = 3000; // 示例值
        std::cout << "ExampleMessageService::GetConversationMaxSeq called" << std::endl;
        return resp;
    }
    
    // 以下方法仅提供空实现，实际项目中需要完整实现
    msg::PullMessageBySeqsResp PullMessageBySeqs(const Context& ctx, const msg::PullMessageBySeqsReq& req) override {
        std::cout << "ExampleMessageService::PullMessageBySeqs called" << std::endl;
        return {};
    }
    
    msg::GetSeqMessageResp GetSeqMessage(const Context& ctx, const msg::GetSeqMessageReq& req) override {
        std::cout << "ExampleMessageService::GetSeqMessage called" << std::endl;
        return {};
    }
    
    msg::SearchMessageResp SearchMessage(const Context& ctx, const msg::SearchMessageReq& req) override {
        std::cout << "ExampleMessageService::SearchMessage called" << std::endl;
        return {};
    }
    
    msg::SendMessageResp SendMessages(const Context& ctx, const msg::SendMessageReq& req) override {
        std::cout << "ExampleMessageService::SendMessages called" << std::endl;
        return {};
    }
    
    msg::SendSimpleMsgResp SendSimpleMsg(const Context& ctx, const msg::SendSimpleMsgReq& req) override {
        std::cout << "ExampleMessageService::SendSimpleMsg called" << std::endl;
        return {};
    }
    
    msg::SetUserConversationsMinSeqResp SetUserConversationsMinSeq(const Context& ctx, const msg::SetUserConversationsMinSeqReq& req) override {
        std::cout << "ExampleMessageService::SetUserConversationsMinSeq called" << std::endl;
        return {};
    }
    
    msg::ClearConversationsMsgResp ClearConversationsMsg(const Context& ctx, const msg::ClearConversationsMsgReq& req) override {
        std::cout << "ExampleMessageService::ClearConversationsMsg called" << std::endl;
        return {};
    }
    
    msg::UserClearAllMsgResp UserClearAllMsg(const Context& ctx, const msg::UserClearAllMsgReq& req) override {
        std::cout << "ExampleMessageService::UserClearAllMsg called" << std::endl;
        return {};
    }
    
    msg::DeleteMsgsResp DeleteMsgs(const Context& ctx, const msg::DeleteMsgsReq& req) override {
        std::cout << "ExampleMessageService::DeleteMsgs called" << std::endl;
        return {};
    }
    
    msg::DeleteMsgPhysicalBySeqResp DeleteMsgPhysicalBySeq(const Context& ctx, const msg::DeleteMsgPhysicalBySeqReq& req) override {
        std::cout << "ExampleMessageService::DeleteMsgPhysicalBySeq called" << std::endl;
        return {};
    }
    
    msg::DeleteMsgPhysicalResp DeleteMsgPhysical(const Context& ctx, const msg::DeleteMsgPhysicalReq& req) override {
        std::cout << "ExampleMessageService::DeleteMsgPhysical called" << std::endl;
        return {};
    }
    
    msg::SetSendMsgStatusResp SetSendMsgStatus(const Context& ctx, const msg::SetSendMsgStatusReq& req) override {
        std::cout << "ExampleMessageService::SetSendMsgStatus called" << std::endl;
        return {};
    }
    
    msg::GetSendMsgStatusResp GetSendMsgStatus(const Context& ctx, const msg::GetSendMsgStatusReq& req) override {
        std::cout << "ExampleMessageService::GetSendMsgStatus called" << std::endl;
        return {};
    }
    
    msg::RevokeMsgResp RevokeMsg(const Context& ctx, const msg::RevokeMsgReq& req) override {
        std::cout << "ExampleMessageService::RevokeMsg called" << std::endl;
        return {};
    }
    
    msg::MarkMsgsAsReadResp MarkMsgsAsRead(const Context& ctx, const msg::MarkMsgsAsReadReq& req) override {
        std::cout << "ExampleMessageService::MarkMsgsAsRead called" << std::endl;
        return {};
    }
    
    msg::MarkConversationAsReadResp MarkConversationAsRead(const Context& ctx, const msg::MarkConversationAsReadReq& req) override {
        std::cout << "ExampleMessageService::MarkConversationAsRead called" << std::endl;
        return {};
    }
    
    msg::SetConversationHasReadSeqResp SetConversationHasReadSeq(const Context& ctx, const msg::SetConversationHasReadSeqReq& req) override {
        std::cout << "ExampleMessageService::SetConversationHasReadSeq called" << std::endl;
        return {};
    }
    
    msg::GetConversationsHasReadAndMaxSeqResp GetConversationsHasReadAndMaxSeq(const Context& ctx, const msg::GetConversationsHasReadAndMaxSeqReq& req) override {
        std::cout << "ExampleMessageService::GetConversationsHasReadAndMaxSeq called" << std::endl;
        return {};
    }
    
    msg::GetActiveUserResp GetActiveUser(const Context& ctx, const msg::GetActiveUserReq& req) override {
        std::cout << "ExampleMessageService::GetActiveUser called" << std::endl;
        return {};
    }
    
    msg::GetActiveGroupResp GetActiveGroup(const Context& ctx, const msg::GetActiveGroupReq& req) override {
        std::cout << "ExampleMessageService::GetActiveGroup called" << std::endl;
        return {};
    }
    
    msg::GetServerTimeResp GetServerTime(const Context& ctx, const msg::GetServerTimeReq& req) override {
        std::cout << "ExampleMessageService::GetServerTime called" << std::endl;
        return {};
    }
    
    msg::ClearMsgResp ClearMsg(const Context& ctx, const msg::ClearMsgReq& req) override {
        std::cout << "ExampleMessageService::ClearMsg called" << std::endl;
        return {};
    }
    
    msg::DestructMsgsResp DestructMsgs(const Context& ctx, const msg::DestructMsgsReq& req) override {
        std::cout << "ExampleMessageService::DestructMsgs called" << std::endl;
        return {};
    }
    
    msg::GetActiveConversationResp GetActiveConversation(const Context& ctx, const msg::GetActiveConversationReq& req) override {
        std::cout << "ExampleMessageService::GetActiveConversation called" << std::endl;
        return {};
    }
    
    msg::SetUserConversationMaxSeqResp SetUserConversationMaxSeq(const Context& ctx, const msg::SetUserConversationMaxSeqReq& req) override {
        std::cout << "ExampleMessageService::SetUserConversationMaxSeq called" << std::endl;
        return {};
    }
    
    msg::SetUserConversationMinSeqResp SetUserConversationMinSeq(const Context& ctx, const msg::SetUserConversationMinSeqReq& req) override {
        std::cout << "ExampleMessageService::SetUserConversationMinSeq called" << std::endl;
        return {};
    }
    
    msg::GetLastMessageSeqByTimeResp GetLastMessageSeqByTime(const Context& ctx, const msg::GetLastMessageSeqByTimeReq& req) override {
        std::cout << "ExampleMessageService::GetLastMessageSeqByTime called" << std::endl;
        return {};
    }
    
    msg::GetLastMessageResp GetLastMessage(const Context& ctx, const msg::GetLastMessageReq& req) override {
        std::cout << "ExampleMessageService::GetLastMessage called" << std::endl;
        return {};
    }
};

// 主函数示例
int main() {
    std::cout << "MessageService C++ Example" << std::endl;
    
    // 示例 1: 创建服务器并启动
    {
        // 创建消息服务实现
        auto handler = std::shared_ptr<ExampleMessageService>(new ExampleMessageService());
        
        // 创建服务器
        std::vector<ServerOption> serverOptions;
        auto server = NewServer(handler, serverOptions);
        
        // 启动服务器
        server->start();
        
        // 检查服务器状态
        std::cout << "Server is running: " << (server->isRunning() ? "yes" : "no") << std::endl;
        
        // 停止服务器
        server->stop();
        std::cout << "Server is running: " << (server->isRunning() ? "yes" : "no") << std::endl;
    }
    
    // 示例 2: 创建客户端并调用方法
    {
        // 创建客户端
        std::vector<ClientOption> clientOptions;
        auto client = NewClient("MessageService", clientOptions);
        
        // 创建上下文和请求
        Context ctx;
        msg::GetMaxSeqReq req;
        
        // 调用服务方法
        try {
            auto resp = client->GetMaxSeq(ctx, req);
            std::cout << "GetMaxSeq response: " << resp.maxSeq << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error calling GetMaxSeq: " << e.what() << std::endl;
        }
        
        // 断开客户端连接
        client->disconnect();
    }
    
    std::cout << "Example completed" << std::endl;
    return 0;
}