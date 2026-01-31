#include <iostream>
#include <string>

void testMQ();
void testMsgServiceServer();
void testMsgServiceClient();
void test_threadpool();


// #define TEST_MQ 
 #define TEST_MSG_SERVICE
// # define TEST_THREADPOOL

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

#ifdef TEST_THREADPOOL
    test_threadpool();
#endif

    return 0;
}