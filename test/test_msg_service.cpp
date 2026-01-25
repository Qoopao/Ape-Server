#include "services/msg_service/server.h"

void testMsgServiceStartup();


void testMsgService(){
    MsgServiceImpl msgImpl;
    std::unique_ptr<EtcdServiceRegistry> etcd_registry = 
        std::make_unique<EtcdServiceRegistry>("msg_service", "127.0.0.1", 50001);
    msgImpl.setRegistry(std::move(etcd_registry));

    msgImpl.startup();  // ctrl+c 后keepalive析构，自动删除key
}
