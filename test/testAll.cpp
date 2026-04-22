#include <iostream>
#include <string>
#include "util/redisconnector.h"

void testMQ();
void testMsgServiceServer();
void testMsgServiceClient();
void testBackbonServiceServer();
void testBackbonServiceClient();
void test_threadpool();


// #define TEST_MQ 
// #define TEST_MSG_SERVICE
// # define TEST_THREADPOOL
 #define TEST_BACKBON_SERVICE
// #define TEST_REDIS

int main(int argc, char* argv[]) {
#ifdef TEST_MQ
    testMQ();
#endif

#ifdef TEST_MSG_SERVICE
    if(argc != 2){
        std::cerr << "Usage: " << argv[0] << " <server|client>" << std::endl;
        return 1;
    }
    std::string role = argv[1];
    if(role == "server"){
        testMsgServiceServer();
    }else if(role == "client"){
        testMsgServiceClient();
    }else{
        std::cerr << "Invalid role. Use 'server' or 'client'." << std::endl;
        return 1;
    }
#endif

#ifdef TEST_BACKBON_SERVICE
    if(argc != 2){
        std::cerr << "Usage: " << argv[0] << " <server|client>" << std::endl;
        return 1;
    }
    std::string role = argv[1];
    if(role == "server"){
        testBackbonServiceServer();
    }else if(role == "client"){
        testBackbonServiceClient();
    }else{
        std::cerr << "Invalid role. Use 'server' or 'client'." << std::endl;
        return 1;
    }
#endif

#ifdef TEST_REDIS
    RedisConnector& redisConnector = RedisConnector::get_instance();
    Redis& redis = redisConnector.get_redis();
    bool flag = redis.set("key", "value");
    if(flag){
        std::cout << "===== Redis连接测试全部通过 =====" << std::endl;
    }else{
        std::cerr << "Redis连接测试失败";
    }
    
#endif

#ifdef TEST_THREADPOOL
    test_threadpool();
#endif

    return 0;
}