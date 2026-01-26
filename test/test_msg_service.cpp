#include "services/msg_service/server.h"
#include "services/register/etcd_service_register.h"

void testMsgServiceStartup();


void testMsgService(){
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
