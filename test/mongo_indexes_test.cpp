#include "storage/mongoconnector.h"
#include <bsoncxx/json.hpp>
#include <iostream>
#include <stdexcept>
int main() {
    struct Case { const char* json; bool expected; };
    const Case cases[]={
        {R"({"name":"serverMsgID_1","key":{"serverMsgID":1},"unique":true})",true},
        {R"({"name":"message_server_id_unique","key":{"serverMsgID":1},"unique":true})",true},
        {R"({"name":"arbitrary_name","key":{"serverMsgID":1},"unique":true,"sparse":false})",true},
        {R"({"key":{"serverMsgID":{"$numberLong":"1"}},"unique":true})",true},
        {R"({"key":{"serverMsgID":1.0},"unique":true})",true},
        {R"({"name":"serverMsgID_1","key":{"serverMsgID":1}})",false},
        {R"({"key":{"serverMsgID":1},"unique":true,"sparse":true})",false},
        {R"({"key":{"serverMsgID":1},"unique":true,"partialFilterExpression":{"committed":true}})",false},
        {R"({"key":{"serverMsgID":1,"convID":1},"unique":true})",false},
        {R"({"key":{"serverMsgID":-1},"unique":true})",false},
        {R"({"key":{"convID":1},"unique":true})",false}
    };
    for(const auto& test:cases) {
        auto index=bsoncxx::from_json(test.json);
        if(MongoConnector::IsFullUniqueMessageIdIndex(index.view())!=test.expected) {
            std::cerr<<"FAIL "<<test.json<<"\n"; return 1;
        }
    }
    std::cout<<"PASS Mongo index compatibility: legacy/custom names, uniqueness, sparse/partial and key shape\n";
}
