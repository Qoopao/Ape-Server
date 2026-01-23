# C++ 重构计划：消息服务

## 1. 概述

将 `/root/roc-im-server/internal/kitex_gen/msg/messageservice` 目录下的 Go 代码重构为 C++，分为 `client.cpp` 和 `server.cpp` 两个文件。

## 2. 需要实现的功能

### 2.1 服务定义

消息服务提供以下核心功能：
- 消息发送与接收
- 消息状态管理（发送状态、已读状态等）
- 消息查询与搜索
- 消息删除与撤回
- 会话管理
- 活跃用户/群组统计

### 2.2 数据结构

需要定义以下主要数据结构：
- 消息数据（MsgData）
- 请求/响应结构（如 GetMaxSeqReq、GetMaxSeqResp 等）
- 会话信息
- 用户信息
- 群组信息

## 3. 客户端实现（client.cpp）

### 3.1 类定义

```cpp
class MessageServiceClient {
private:
    // 客户端内部状态和连接信息
    std::string serviceName;
    std::vector<ClientOption> options;
    
public:
    // 构造函数
    MessageServiceClient(const std::string& serviceName, const std::vector<ClientOption>& options = {});
    
    // 客户端方法声明
    GetMaxSeqResp GetMaxSeq(const Context& ctx, const GetMaxSeqReq& req);
    SeqsInfoResp GetMaxSeqs(const Context& ctx, const GetMaxSeqsReq& req);
    SeqsInfoResp GetHasReadSeqs(const Context& ctx, const GetHasReadSeqsReq& req);
    GetMsgByConversationIDsResp GetMsgByConversationIDs(const Context& ctx, const GetMsgByConversationIDsReq& req);
    GetConversationMaxSeqResp GetConversationMaxSeq(const Context& ctx, const GetConversationMaxSeqReq& req);
    PullMessageBySeqsResp PullMessageBySeqs(const Context& ctx, const PullMessageBySeqsReq& req);
    GetSeqMessageResp GetSeqMessage(const Context& ctx, const GetSeqMessageReq& req);
    SearchMessageResp SearchMessage(const Context& ctx, const SearchMessageReq& req);
    SendMessageResp SendMessages(const Context& ctx, const SendMessageReq& req);
    SendSimpleMsgResp SendSimpleMsg(const Context& ctx, const SendSimpleMsgReq& req);
    SetUserConversationsMinSeqResp SetUserConversationsMinSeq(const Context& ctx, const SetUserConversationsMinSeqReq& req);
    ClearConversationsMsgResp ClearConversationsMsg(const Context& ctx, const ClearConversationsMsgReq& req);
    UserClearAllMsgResp UserClearAllMsg(const Context& ctx, const UserClearAllMsgReq& req);
    DeleteMsgsResp DeleteMsgs(const Context& ctx, const DeleteMsgsReq& req);
    DeleteMsgPhysicalBySeqResp DeleteMsgPhysicalBySeq(const Context& ctx, const DeleteMsgPhysicalBySeqReq& req);
    DeleteMsgPhysicalResp DeleteMsgPhysical(const Context& ctx, const DeleteMsgPhysicalReq& req);
    SetSendMsgStatusResp SetSendMsgStatus(const Context& ctx, const SetSendMsgStatusReq& req);
    GetSendMsgStatusResp GetSendMsgStatus(const Context& ctx, const GetSendMsgStatusReq& req);
    RevokeMsgResp RevokeMsg(const Context& ctx, const RevokeMsgReq& req);
    MarkMsgsAsReadResp MarkMsgsAsRead(const Context& ctx, const MarkMsgsAsReadReq& req);
    MarkConversationAsReadResp MarkConversationAsRead(const Context& ctx, const MarkConversationAsReadReq& req);
    SetConversationHasReadSeqResp SetConversationHasReadSeq(const Context& ctx, const SetConversationHasReadSeqReq& req);
    GetConversationsHasReadAndMaxSeqResp GetConversationsHasReadAndMaxSeq(const Context& ctx, const GetConversationsHasReadAndMaxSeqReq& req);
    GetActiveUserResp GetActiveUser(const Context& ctx, const GetActiveUserReq& req);
    GetActiveGroupResp GetActiveGroup(const Context& ctx, const GetActiveGroupReq& req);
    GetServerTimeResp GetServerTime(const Context& ctx, const GetServerTimeReq& req);
    ClearMsgResp ClearMsg(const Context& ctx, const ClearMsgReq& req);
    DestructMsgsResp DestructMsgs(const Context& ctx, const DestructMsgsReq& req);
    GetActiveConversationResp GetActiveConversation(const Context& ctx, const GetActiveConversationReq& req);
    SetUserConversationMaxSeqResp SetUserConversationMaxSeq(const Context& ctx, const SetUserConversationMaxSeqReq& req);
    SetUserConversationMinSeqResp SetUserConversationMinSeq(const Context& ctx, const SetUserConversationMinSeqReq& req);
    GetLastMessageSeqByTimeResp GetLastMessageSeqByTime(const Context& ctx, const GetLastMessageSeqByTimeReq& req);
    GetLastMessageResp GetLastMessage(const Context& ctx, const GetLastMessageReq& req);
    
    // 辅助方法
    void connect();
    void disconnect();
};

// 工厂函数
std::unique_ptr<MessageServiceClient> NewClient(const std::string& serviceName, const std::vector<ClientOption>& options = {});
```

## 4. 服务器端实现（server.cpp）

### 4.1 类定义

```cpp
// 消息服务接口类（需要用户实现）
class MessageService {
public:
    virtual ~MessageService() = default;
    
    // 所有服务方法的纯虚函数声明
    virtual GetMaxSeqResp GetMaxSeq(const Context& ctx, const GetMaxSeqReq& req) = 0;
    virtual SeqsInfoResp GetMaxSeqs(const Context& ctx, const GetMaxSeqsReq& req) = 0;
    virtual SeqsInfoResp GetHasReadSeqs(const Context& ctx, const GetHasReadSeqsReq& req) = 0;
    virtual GetMsgByConversationIDsResp GetMsgByConversationIDs(const Context& ctx, const GetMsgByConversationIDsReq& req) = 0;
    virtual GetConversationMaxSeqResp GetConversationMaxSeq(const Context& ctx, const GetConversationMaxSeqReq& req) = 0;
    virtual PullMessageBySeqsResp PullMessageBySeqs(const Context& ctx, const PullMessageBySeqsReq& req) = 0;
    virtual GetSeqMessageResp GetSeqMessage(const Context& ctx, const GetSeqMessageReq& req) = 0;
    virtual SearchMessageResp SearchMessage(const Context& ctx, const SearchMessageReq& req) = 0;
    virtual SendMessageResp SendMessages(const Context& ctx, const SendMessageReq& req) = 0;
    virtual SendSimpleMsgResp SendSimpleMsg(const Context& ctx, const SendSimpleMsgReq& req) = 0;
    virtual SetUserConversationsMinSeqResp SetUserConversationsMinSeq(const Context& ctx, const SetUserConversationsMinSeqReq& req) = 0;
    virtual ClearConversationsMsgResp ClearConversationsMsg(const Context& ctx, const ClearConversationsMsgReq& req) = 0;
    virtual UserClearAllMsgResp UserClearAllMsg(const Context& ctx, const UserClearAllMsgReq& req) = 0;
    virtual DeleteMsgsResp DeleteMsgs(const Context& ctx, const DeleteMsgsReq& req) = 0;
    virtual DeleteMsgPhysicalBySeqResp DeleteMsgPhysicalBySeq(const Context& ctx, const DeleteMsgPhysicalBySeqReq& req) = 0;
    virtual DeleteMsgPhysicalResp DeleteMsgPhysical(const Context& ctx, const DeleteMsgPhysicalReq& req) = 0;
    virtual SetSendMsgStatusResp SetSendMsgStatus(const Context& ctx, const SetSendMsgStatusReq& req) = 0;
    virtual GetSendMsgStatusResp GetSendMsgStatus(const Context& ctx, const GetSendMsgStatusReq& req) = 0;
    virtual RevokeMsgResp RevokeMsg(const Context& ctx, const RevokeMsgReq& req) = 0;
    virtual MarkMsgsAsReadResp MarkMsgsAsRead(const Context& ctx, const MarkMsgsAsReadReq& req) = 0;
    virtual MarkConversationAsReadResp MarkConversationAsRead(const Context& ctx, const MarkConversationAsReadReq& req) = 0;
    virtual SetConversationHasReadSeqResp SetConversationHasReadSeq(const Context& ctx, const SetConversationHasReadSeqReq& req) = 0;
    virtual GetConversationsHasReadAndMaxSeqResp GetConversationsHasReadAndMaxSeq(const Context& ctx, const GetConversationsHasReadAndMaxSeqReq& req) = 0;
    virtual GetActiveUserResp GetActiveUser(const Context& ctx, const GetActiveUserReq& req) = 0;
    virtual GetActiveGroupResp GetActiveGroup(const Context& ctx, const GetActiveGroupReq& req) = 0;
    virtual GetServerTimeResp GetServerTime(const Context& ctx, const GetServerTimeReq& req) = 0;
    virtual ClearMsgResp ClearMsg(const Context& ctx, const ClearMsgReq& req) = 0;
    virtual DestructMsgsResp DestructMsgs(const Context& ctx, const DestructMsgsReq& req) = 0;
    virtual GetActiveConversationResp GetActiveConversation(const Context& ctx, const GetActiveConversationReq& req) = 0;
    virtual SetUserConversationMaxSeqResp SetUserConversationMaxSeq(const Context& ctx, const SetUserConversationMaxSeqReq& req) = 0;
    virtual SetUserConversationMinSeqResp SetUserConversationMinSeq(const Context& ctx, const SetUserConversationMinSeqReq& req) = 0;
    virtual GetLastMessageSeqByTimeResp GetLastMessageSeqByTime(const Context& ctx, const GetLastMessageSeqByTimeReq& req) = 0;
    virtual GetLastMessageResp GetLastMessage(const Context& ctx, const GetLastMessageReq& req) = 0;
};

// 服务器类
class MessageServiceServer {
private:
    std::unique_ptr<Server> server;
    std::shared_ptr<MessageService> handler;
    
public:
    // 构造函数
    MessageServiceServer(std::shared_ptr<MessageService> handler, const std::vector<ServerOption>& options = {});
    
    // 服务器控制方法
    void start();
    void stop();
    bool isRunning() const;
};

// 工厂函数
std::unique_ptr<MessageServiceServer> NewServer(std::shared_ptr<MessageService> handler, const std::vector<ServerOption>& options = {});
```

## 5. 实现注意事项

### 5.1 通信协议

需要实现与 Kitex 兼容的通信协议，包括：
- 序列化/反序列化机制
- 网络传输层
- 错误处理

### 5.2 线程安全

确保客户端和服务器端的实现是线程安全的，尤其是在并发访问场景下。

### 5.3 错误处理

使用异常或错误码机制进行错误处理，确保错误信息的传递和处理。

### 5.4 性能考虑

- 连接池管理
- 异步处理
- 批量操作优化

## 6. 测试计划

实现以下测试用例：
- 客户端连接测试
- 消息发送/接收测试
- 消息状态管理测试
- 错误处理测试
- 并发性能测试

## 7. 依赖

- 网络库（如 gRPC、Thrift 或自定义协议）
- 序列化库（如 Protobuf、FlatBuffers 等）
- 日志库
- 配置管理库

## 8. 时间估计

| 阶段 | 时间估计 |
|------|----------|
| 设计与规划 | 1 天 |
| 数据结构定义 | 2 天 |
| 客户端实现 | 3 天 |
| 服务器端实现 | 3 天 |
| 测试与调试 | 2 天 |
| 文档编写 | 1 天 |
| **总计** | **12 天** |