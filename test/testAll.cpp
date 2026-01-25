void testMQ();
void testMsgService();


// #define TEST_MQ 
#define TEST_MSG_SERVICE

int main() {
#ifdef TEST_MQ
    testMQ();
#endif

#ifdef TEST_MSG_SERVICE
    testMsgService();
#endif

    return 0;
}