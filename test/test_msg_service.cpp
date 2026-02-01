#include "services/msg_service/server.h"
#include "services/msg_service/client.h"
#include "services/register_discovery/etcd_service_register.h"
#include <ctime>

void testMsgServiceStartup();


void testMsgServiceServer(){
    std::unique_ptr<BaseRegisterService> ServiceImpl = std::make_unique<MsgServiceImpl>();
    ServiceConfig sconfig{
        .service_name = "msg_service",
        .service_ip = "127.0.0.1",
        .service_port = 50001,
        .etcd_endpoint = "http://127.0.0.1:2379",
    };
    std::unique_ptr<EtcdServiceRegistry> etcd_registry = 
        std::make_unique<EtcdServiceRegistry>(sconfig);
    ServiceImpl->setRegistry(std::move(etcd_registry)); 

    std::string server_addr = sconfig.service_ip + ":" + std::to_string(sconfig.service_port);
    ServiceImpl->startup(server_addr);  // ctrl+c 后触发信号处理，自动析构，如果不写信号处理的话，只能等系统回收资源后，ttl到自动删除 
}

void testMsgServiceClient(){
    auto client_handler = std::make_shared<MsgClientHandler>("msg_service");

    // 构造一个简单的消息
    ::sdkws::SendMessageReq request;

    // 添加一条消息
    ::sdkws::MsgData* msg = request.add_msgs();
    // 填充必要字段
    msg->set_sendid("user_123");              // 发送者ID
    msg->set_recvid("user_456");              // 接收者ID
    msg->set_convid("conv_789");              // 会话ID
    msg->set_clientmsgid("client_msg_123");   // 客户端消息ID
    msg->set_content("Hello, this is a test message!");  // 消息内容
    msg->set_sessiontype(0);                  // 0: 单聊, 1: 群聊
    msg->set_msgfrom(0);                      // 0: 用户发送
    msg->set_contenttype(1);                  // 1: 文本消息
    msg->set_sendtime(time(nullptr));         // 当前时间戳

    ::sdkws::MsgData* msg2 = request.add_msgs();
    msg2->set_sendid("user_123");
    msg2->set_recvid("user_456");
    msg2->set_convid("conv_789");
    msg2->set_clientmsgid("client_msg_456");
    msg2->set_content("This is another test message!");
    msg2->set_sessiontype(0);
    msg2->set_msgfrom(0);
    msg2->set_contenttype(1);
    msg2->set_sendtime(time(nullptr));

    ::sdkws::SendMessageResp response;
    client_handler->SendMessagesClient(request, response);

    //std::this_thread::sleep_for(std::chrono::seconds(1));
}
