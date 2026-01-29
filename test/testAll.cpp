void testMQ();
void testMsgService();
void test_threadpool();


// #define TEST_MQ 
// #define TEST_MSG_SERVICE
# define TEST_THREADPOOL

int main() {
#ifdef TEST_MQ
    testMQ();
#endif

#ifdef TEST_MSG_SERVICE
    testMsgService();
#endif

#ifdef TEST_THREADPOOL
    test_threadpool();
#endif

    return 0;
}