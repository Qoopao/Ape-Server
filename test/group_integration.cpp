#include "storage/mysqlhandler.h"
#include "services/group_service/server.h"
#include "services/group_service/client.h"
#include "services/msg_service/server.h"
#include "storage/mysqlconnector.h"
#include "util/snowflake.h"
#include <boost/asio/use_future.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/mysql/format_sql.hpp>
#include <iostream>
#include <stdexcept>
#include <future>
using boost::asio::awaitable;
using boost::asio::use_awaitable;
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
groupchat::Command cmd(groupchat::Operation op,uint64_t actor,const std::string& group={}) {
    static int serial=0;
    groupchat::Command c; c.set_operation(op); c.set_actor(actor); c.set_groupid(group); c.set_limit(50); c.set_clientid("test-"+std::to_string(++serial)); return c;
}
awaitable<groupchat::Reply> run(groupchat::Command c,const std::string& expected="0") {
    auto reply=co_await MySQLHandler::ExecuteGroupCommand(std::move(c));
    if(reply.errorcode()!=expected) throw std::runtime_error("expected "+expected+", got "+reply.errorcode()+": "+reply.errormessage());
    co_return reply;
}
awaitable<void> tests() {
    {
    auto c=co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
    boost::mysql::results r;
    co_await c->async_execute("CREATE TABLE users(account BIGINT UNSIGNED PRIMARY KEY,nickname VARCHAR(256) NOT NULL) DEFAULT CHARSET=utf8mb4",r,use_awaitable);
    for(uint64_t i=1;i<=205;++i) co_await c->async_execute(boost::mysql::format_sql(c->format_opts().value(),"INSERT INTO users VALUES({},'测试成员')",i),r,use_awaitable);
    }
    co_await MySQLHandler::InitializeGroups();
    auto create=cmd(groupchat::Create,1); create.set_text("测试群");
    auto created=co_await run(create); auto group=created.groups(0).id();
    const auto publicNumber=created.groups(0).number();
    check(publicNumber.size()==10,"Public group number must have ten digits");
    auto byNumber=cmd(groupchat::Search,1,publicNumber);
    auto foundByNumber=co_await run(byNumber);
    check(foundByNumber.groups(0).id()==group,"Public group search changed conversation identity");
    {
        auto connection=co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable);
        boost::mysql::results result;
        co_await connection->async_execute("INSERT INTO im_groups(id,name,owner) VALUES(987654321098765432,'legacy group',1)",result,use_awaitable);
    }
    co_await MySQLHandler::EnsureGroupNumbers();
    auto legacy=co_await run(cmd(groupchat::Search,1,"987654321098765432"));
    check(legacy.groups(0).number().size()==10 && legacy.groups(0).id()=="987654321098765432","Legacy group numbering lost identity");
    co_await MySQLHandler::EnsureGroupNumbers();
    auto sameLegacy=co_await run(cmd(groupchat::Search,1,legacy.groups(0).number()));
    check(sameLegacy.groups(0).id()==legacy.groups(0).id(),"Group numbering backfill was not idempotent");
    std::cout<<"PASS ten-digit group numbers, public search and existing-group backfill\n";
    {
        GroupServiceImpl service("group-structure-test","127.0.0.1:0");
        grpc::ServerBuilder builder; int port=0;
        builder.AddListeningPort("127.0.0.1:0",grpc::InsecureServerCredentials(),&port);
        builder.RegisterService(&service);
        auto server=builder.BuildAndStart(); check(server!=nullptr,"GroupService test server failed");
        grpc::ChannelArguments arguments;
        arguments.SetInt("grpc.enable_http_proxy",0);
        GroupClient client(grpc::CreateCustomChannel("127.0.0.1:"+std::to_string(port),grpc::InsecureChannelCredentials(),arguments));
        auto reply=co_await client.Execute(cmd(groupchat::Search,1,group));
        check(reply.errorcode()=="0" && reply.groups_size()==1 && reply.groups(0).id()==group,"GroupClient/GroupService Execute RPC failed");
        server->Shutdown(); server->Wait();
    }
    std::cout<<"PASS GroupClient -> GroupService Execute -> MySQLHandler RPC path\n";
    auto replay=co_await run(create); check(replay.groups(0).id()==group,"create replay duplicated group");
    create.set_text("changed"); co_await run(create,"IDEMPOTENCY_CONFLICT");
    auto before=cmd(groupchat::Send,1,group); before.set_text("before joining"); co_await run(before);
    co_await run(cmd(groupchat::Pull,2,group),"NOT_MEMBER");
    co_await run(cmd(groupchat::Send,2,group),"NOT_MEMBER");
    auto apply=cmd(groupchat::Apply,2,group); apply.set_text("申请说明");
    co_await run(apply); co_await run(cmd(groupchat::Apply,2,group));
    auto pending=co_await run(cmd(groupchat::Applications,1,group));
    check(pending.applications_size()==1,"duplicate applications created multiple pending rows");
    auto review=cmd(groupchat::Review,2,group); review.set_applicationid(pending.applications(0).id()); review.set_accept(true);
    co_await run(review,"NOT_MEMBER"); review=cmd(groupchat::Review,1,group); review.set_applicationid(pending.applications(0).id()); review.set_accept(true);
    co_await run(review); co_await run(review);
    auto search=co_await run(cmd(groupchat::Search,2,group)); const auto epoch=search.groups(0).epoch();
    check(search.groups(0).joinseq()==1 && search.groups(0).membercount()==2,"join sequence/member count wrong");
    auto after=cmd(groupchat::Send,1,group); after.set_text("after joining"); auto sent=co_await run(after);
    replay=co_await run(after); check(sent.messages(0).id()==replay.messages(0).id(),"send replay duplicated message");
    auto pull=co_await run(cmd(groupchat::Pull,2,group)); check(pull.messages_size()==1 && pull.messages(0).seq()==2,"member could access pre-join history");
    auto notice=co_await MySQLHandler::GroupDeliveryBatch(2,group); check(notice.messages_size()==1,"small group failed to push body");
    auto ack=cmd(groupchat::Ack,2,group); ack.set_after(2); ack.set_epoch(epoch); co_await run(ack);
    notice=co_await MySQLHandler::GroupDeliveryBatch(2,group); check(notice.messages_size()==0,"ACK did not stop delivery retry");
    search=co_await run(cmd(groupchat::Search,2,group)); check(search.groups(0).unread()==1,"ACK incorrectly marked messages read");
    auto read=cmd(groupchat::Read,2,group); read.set_after(2); read.set_epoch(epoch); co_await run(read);
    search=co_await run(cmd(groupchat::Search,2,group)); check(search.groups(0).unread()==0,"read failed to clear unread");
    co_await run(cmd(groupchat::Dismiss,2,group),"OWNER_REQUIRED");
    co_await run(cmd(groupchat::Leave,1,group),"OWNER_CANNOT_LEAVE");
    auto rename=cmd(groupchat::Rename,1,group); rename.set_text("测试群");
    co_await run(rename,"NAME_UNCHANGED");
    rename=cmd(groupchat::Rename,2,group); rename.set_text("改名"); co_await run(rename,"OWNER_REQUIRED");
    rename=cmd(groupchat::Rename,1,group); rename.set_text("改名后的群"); co_await run(rename);
    auto renamed=co_await run(cmd(groupchat::Search,1,group)); check(renamed.groups(0).name()=="改名后的群","Rename did not persist");
    auto kick=cmd(groupchat::Kick,1,group); kick.set_target(2); co_await run(kick); co_await run(kick);
    co_await run(cmd(groupchat::Pull,2,group),"NOT_MEMBER");
    co_await run(cmd(groupchat::Apply,2,group));
    pending=co_await run(cmd(groupchat::Applications,1,group)); review=cmd(groupchat::Review,1,group); review.set_applicationid(pending.applications(0).id()); review.set_accept(true); co_await run(review);
    co_await run(ack,"MEMBERSHIP_CHANGED");
    pull=co_await run(cmd(groupchat::Pull,2,group)); check(!pull.messages_size(),"rejoin leaked messages before new membership");
    std::cout<<"PASS approval, permissions, join boundaries, idempotency, ACK/read separation and rejoin epochs\n";
    // Grow across the production threshold using approved joins, rather than changing counts directly.
    for(uint64_t actor=3;actor<=201;++actor) {
        co_await run(cmd(groupchat::Apply,actor,group)); pending=co_await run(cmd(groupchat::Applications,1,group));
        auto approval=cmd(groupchat::Review,1,group); approval.set_applicationid(pending.applications(0).id()); approval.set_accept(true); co_await run(approval);
    }
    search=co_await run(cmd(groupchat::Search,2,group)); check(search.groups(0).large(),"201-member group not large");
    auto largeMessage=cmd(groupchat::Send,1,group); largeMessage.set_text("large group message"); co_await run(largeMessage);
    notice=co_await MySQLHandler::GroupDeliveryBatch(2,group); check(notice.messages_size()==0 && notice.lastseq()==3,"large group should hint without body");
    pull=co_await run(cmd(groupchat::Pull,2,group)); check(pull.messages_size()==1,"large group pull missed message");
    kick=cmd(groupchat::Kick,1,group); kick.set_target(201); co_await run(kick);
    notice=co_await MySQLHandler::GroupDeliveryBatch(2,group); check(notice.messages_size()==1,"switch back to small group lost unacked message");
    auto page=cmd(groupchat::Members,1,group); page.set_limit(17); uint64_t total=0;
    do { auto members=co_await run(page); total+=members.members_size(); if(!members.hasmore()) break; page.set_after(members.cursor()); } while(true);
    check(total==200,"member pagination lost members");
    // Concurrent sends must receive distinct, contiguous sequence numbers.
    std::vector<std::future<groupchat::Reply>> futures;
    for(int i=0;i<12;++i) { auto send=cmd(groupchat::Send,1,group); send.set_text("concurrent "+std::to_string(i)); futures.push_back(boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),MySQLHandler::ExecuteGroupCommand(send),boost::asio::use_future)); }
    // Waiting uses a timer so this executor can run the spawned transactions.
    auto executor=co_await boost::asio::this_coro::executor; boost::asio::steady_timer timer(executor);
    for(auto& f:futures) { while(f.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) { timer.expires_after(std::chrono::milliseconds(5)); co_await timer.async_wait(use_awaitable); } check(f.get().errorcode()=="0","concurrent send failed"); }
    auto paged=cmd(groupchat::Pull,2,group); paged.set_limit(3); uint64_t count=0,last=2;
    do { auto messages=co_await run(paged); for(const auto& m:messages.messages()) { check(m.seq()==++last,"message sequence gap"); ++count; } if(!messages.hasmore()) break; paged.set_after(messages.cursor()); paged.set_snapshot(messages.snapshot()); } while(true);
    check(count==13,"message pagination lost messages");
    auto oldUnread=co_await run(cmd(groupchat::Search,2,group));
    auto own=cmd(groupchat::Send,2,group); own.set_text("my own message"); co_await run(own);
    auto newUnread=co_await run(cmd(groupchat::Search,2,group)); check(oldUnread.groups(0).unread()==newUnread.groups(0).unread(),"own message increased unread count");
    co_await run(cmd(groupchat::Apply,202,group)); pending=co_await run(cmd(groupchat::Applications,1,group));
    auto rejection=cmd(groupchat::Review,1,group); rejection.set_applicationid(pending.applications(0).id()); rejection.set_accept(false); co_await run(rejection);
    auto mine=co_await run(cmd(groupchat::MyApplications,202)); check(mine.applications_size()==1 && mine.applications(0).status()==2,"applicant cannot inspect rejection");
    auto conflicting=cmd(groupchat::Review,1,group); conflicting.set_applicationid(pending.applications(0).id()); conflicting.set_accept(true); co_await run(conflicting,"APPLICATION_HANDLED");
    {
        // Force a send to wait behind a removal, after it has already opened its transaction.
        auto holder=co_await MySQLConnector::instance().get_pool(0).async_get_connection(use_awaitable); boost::mysql::results locked;
        co_await holder->async_execute("START TRANSACTION",locked,use_awaitable);
        co_await holder->async_execute(boost::mysql::format_sql(holder->format_opts().value(),"SELECT id FROM im_groups WHERE id={} FOR UPDATE",std::stoull(group)),locked,use_awaitable);
        auto queued=cmd(groupchat::Send,200,group); queued.set_text("must not send after kick");
        auto future=boost::asio::co_spawn(executor,MySQLHandler::ExecuteGroupCommand(queued),boost::asio::use_future);
        timer.expires_after(std::chrono::milliseconds(100)); co_await timer.async_wait(use_awaitable);
        co_await holder->async_execute(boost::mysql::format_sql(holder->format_opts().value(),"UPDATE im_group_members SET active=0 WHERE group_id={} AND account=200",std::stoull(group)),locked,use_awaitable);
        co_await holder->async_execute(boost::mysql::format_sql(holder->format_opts().value(),"UPDATE im_groups SET member_count=member_count-1 WHERE id={}",std::stoull(group)),locked,use_awaitable);
        co_await holder->async_execute("COMMIT",locked,use_awaitable);
        while(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) { timer.expires_after(std::chrono::milliseconds(5)); co_await timer.async_wait(use_awaitable); }
        check(future.get().errorcode()=="NOT_MEMBER","stale transaction snapshot allowed a send after removal");
    }
    co_await run(cmd(groupchat::Dismiss,1,group));
    co_await run(cmd(groupchat::Pull,2,group),"GROUP_DISMISSED");
    search=co_await run(cmd(groupchat::Search,2,group)); check(search.groups(0).dismissed()&&!search.groups(0).joined(),"dismissal not reflected in group state");
    notice=co_await MySQLHandler::GroupDeliveryBatch(2,group); check(notice.groupid().empty(),"dismissed group still delivered messages");
    std::cout<<"PASS 200/201 transition, large-group pull, pagination, concurrent ordering, kick/send race, rejection visibility and dismissal\n";
}
int main() {
    try {
        Snowflake::init(1,1);
        boost::mysql::pool_params settings; settings.server_address.emplace_host_and_port("127.0.0.1",33079);
        settings.username="root"; settings.database="im_group_test"; settings.ssl=boost::mysql::ssl_mode::disable;
        MySQLConnector::instance().start(1,&settings);
        boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),tests(),boost::asio::use_future).get();
        {
            GroupServiceImpl service("group-lifecycle-test","127.0.0.1:0");
            service.StartDelivery(); service.StopDelivery();
            service.StartDelivery(); service.StopDelivery();
            MsgServiceImpl messages("message-lifecycle-test","127.0.0.1:0");
            bool rejected=false; try { messages.StartOutbox(); } catch(const std::runtime_error&) { rejected=true; }
            check(rejected,"Outbox worker started without its owning service's producer");
            messages.StopOutbox();
        }
        std::cout<<"PASS service-owned worker lifecycle\n";
        MySQLConnector::instance().stop(); return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<"\n"; MySQLConnector::instance().stop(); return 1; }
}
