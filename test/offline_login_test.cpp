#include "storage/redisconnector.h"
#include "util/message_validation.h"
#include "storage/mongohandler.h"
#include <bsoncxx/json.hpp>
#include <iostream>
#include <stdexcept>
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() {
    try {
        for(const auto& keys:std::vector<std::vector<std::string>>{{},{"one"},{"one","two"}}) {
            for(const auto& args:std::vector<std::vector<std::string>>{{},{"368169632542101504"},{"value with spaces","second"}}) {
                auto request=RedisConnector::MakeEvalRequest("return 1",keys,args);
                check(request.get_expected_responses()==1,"Lua arguments became separate commands");
                check(request.payload().starts_with("*"+std::to_string(3+keys.size()+args.size())+"\r\n"),"Lua command has wrong argument count");
            }
        }
        auto exact=RedisConnector::MakeEvalRequest("return 1",{"key"},{"arg"});
        check(exact.payload()=="*5\r\n$4\r\nEVAL\r\n$8\r\nreturn 1\r\n$1\r\n1\r\n$3\r\nkey\r\n$3\r\narg\r\n","Lua wire format incorrect");
        for(const auto* time:{"1700000000000","1700000000000.0"}) {
            auto document=bsoncxx::from_json(std::string(R"({"serverMsgID":"legacy","sendID":1000002337,"recvID":1000000001,"sessionType":1,"seq":{"$numberLong":"368169632542101504"},"sendTime":)")+time+"}");
            auto message=MongoHandler::DecodeMessage(document.view());
            check(message.sendtime()==1700000000000LL && message.seq()==368169632542101504LL,"Numeric metadata was changed during decoding");
            check(message_validation::IsSingleMessageFor(1000000001,message),"Intended recipient rejected");
            check(!message_validation::IsSingleMessageFor(1000002337,message),"Offline message sent to wrong account");
            message.set_isgroupmsg(true); check(!message_validation::IsSingleMessageFor(1000000001,message),"Group message accepted as single chat");
        }
        auto missing=bsoncxx::from_json(R"({"serverMsgID":"missing-recipient","sendID":1000002337,"sessionType":1})");
        check(!message_validation::IsSingleMessageFor(1000000001,MongoHandler::DecodeMessage(missing.view())),"Missing recipient accepted");
        auto invalid=bsoncxx::from_json(R"({"seq":1.5})"); bool rejected=false;
        try { MongoHandler::DecodeMessage(invalid.view()); } catch(const std::runtime_error&) { rejected=true; }
        check(rejected,"Fractional sequence silently rounded");
        std::cout<<"PASS single-command EVAL wire format, response counts, BSON numeric metadata and offline recipient validation\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<"\n"; return 1; }
}
