#include "gateway/ws_session.h"
#include <boost/asio/use_future.hpp>
#include <iostream>
#include <stdexcept>
#include <set>

struct WSSessionTestAccess {
  static void check(bool ok, const char* error) { if (!ok) throw std::runtime_error(error); }
  static boost::asio::awaitable<void> pagination(std::shared_ptr<WSSession> s) {
    s->account_=1;
    std::vector<sdkws::MsgData> body;
    for(int64_t seq=1;seq<=130;++seq) {
      sdkws::MsgData m; m.set_seq(seq); m.set_sendid(2); m.set_recvid(1);
      m.set_sessiontype(1); m.set_servermsgid(std::to_string(seq)); body.push_back(m);
    }
    std::set<int64_t> acknowledged{5,79}; // High ACK must not skip earlier unreceived messages.
    std::vector<int64_t> cursors, delivered;
    auto fetch=[&](int64_t after,int limit)->boost::asio::awaitable<std::vector<sdkws::MsgData>> {
      cursors.push_back(after); std::vector<sdkws::MsgData> result;
      for(const auto& m:body) if(m.seq()>after && result.size()<static_cast<size_t>(limit)) result.push_back(m);
      co_return result;
    };
    bool interrupt=true;
    auto deliver=[&](sdkws::MsgData m)->boost::asio::awaitable<void> {
      if(acknowledged.contains(m.seq())) co_return;
      delivered.push_back(m.seq());
      if(interrupt && m.seq()==17) {
        // Message was sent, but its ACK was lost before disconnect.
        s->disconnected_=true; co_return;
      }
      acknowledged.insert(m.seq());
    };
    co_await s->scanOfflineMessages(fetch,deliver);
    check(delivered.back()==17 && !acknowledged.contains(17),"disconnect advanced unconfirmed message");
    s->disconnected_=false; interrupt=false; cursors.clear(); delivered.clear();
    co_await s->scanOfflineMessages(fetch,deliver);
    check(cursors==std::vector<int64_t>({0,50,100,130}),"pagination skipped or repeated a page");
    check(delivered.front()==17 && acknowledged.size()==130,"relogin missed unconfirmed lower sequence");
    check(delivered.size()==113,"acknowledged messages were redelivered");
    // A fully filtered page still advances; repeated login emits no already-ACKed messages.
    delivered.clear(); co_await s->scanOfflineMessages(fetch,deliver);
    check(delivered.empty(),"repeat sync ignored persistent receipt filtering");
    bool rejected=false;
    auto corrupt=[&](int64_t,int)->boost::asio::awaitable<std::vector<sdkws::MsgData>> {
      auto wrong=body.front(); wrong.set_recvid(3); co_return std::vector<sdkws::MsgData>{wrong};
    };
    try { co_await s->scanOfflineMessages(corrupt,deliver); }
    catch(const std::runtime_error&) { rejected=true; }
    check(rejected,"wrong recipient was accepted");
    rejected=false;
    auto unordered=[&](int64_t,int)->boost::asio::awaitable<std::vector<sdkws::MsgData>> {
      co_return std::vector<sdkws::MsgData>{body[1],body[0]};
    };
    try { co_await s->scanOfflineMessages(unordered,deliver); }
    catch(const std::runtime_error&) { rejected=true; }
    check(rejected,"backward cursor was accepted");
    s->account_=0; // No database cleanup is needed for this in-memory transport fixture.
    co_await s->onDisconnect();
  }
  static boost::asio::awaitable<void> run(std::shared_ptr<WSSession> s, int scenario) {
    namespace asio = boost::asio;
    s->pendingOfflineMsgs_["test"] = {1,"1_2"};
    if (scenario == 0) {
      co_await s->onDisconnect();
      co_await s->waitOfflineBatch(std::chrono::milliseconds(10));
      check(s->pendingOfflineMsgs_.empty(), "disconnect did not clear pending messages");
    } else if (scenario == 1 || scenario == 2) {
      auto trigger = std::make_shared<asio::steady_timer>(co_await asio::this_coro::executor);
      trigger->expires_after(std::chrono::milliseconds(1));
      trigger->async_wait([s,trigger,scenario](auto ec) {
        if (ec) return;
        if (scenario == 1) {
          s->pendingOfflineMsgs_.clear();
          if (s->batch_ack_timer_) s->batch_ack_timer_->cancel();
        } else {
          asio::co_spawn(s->ws_.get_executor(), [s]() -> asio::awaitable<void> {
            co_await s->onDisconnect();
          }, asio::detached);
        }
      });
      co_await s->waitOfflineBatch(std::chrono::milliseconds(100));
      check(s->disconnected_ == (scenario==2), "ACK/close wait outcome incorrect");
      check(!s->batch_ack_timer_, "ACK timer retained after wait");
    } else if (scenario == 3) {
      co_await s->waitOfflineBatch(std::chrono::milliseconds(1));
      check(s->disconnected_, "missing ACK did not close session");
    } else if (scenario == 4) {
      // Fill the actual write channel without running a network drain.
      s->write_queue_ = std::make_unique<asio::experimental::channel<void(
          boost::system::error_code,std::shared_ptr<std::string>)>>(s->ws_.get_executor(),1);
      co_await s->queuedWrite(std::make_shared<std::string>("first"));
      bool rejected=false;
      try { co_await s->queuedWrite(std::make_shared<std::string>("overflow")); }
      catch (const boost::system::system_error&) { rejected=true; }
      check(rejected && s->disconnected_, "queue overflow silently dropped message");
      rejected=false;
      try { co_await s->queuedWrite(std::make_shared<std::string>("closed")); }
      catch (const boost::system::system_error&) { rejected=true; }
      check(rejected,"closed queue accepted write");
    } else if (scenario == 5) {
      s->pendingOfflineMsgs_.clear(); // ACK arrived before the wait was established.
      co_await s->waitOfflineBatch(std::chrono::milliseconds(1));
      check(!s->disconnected_,"early ACK was lost");
    }
    co_await s->onDisconnect(); // Idempotent cleanup also covers account=0.
  }
};
int main() {
  try {
    for(int scenario=0;scenario<6;++scenario) {
      boost::asio::io_context io;
      boost::asio::ssl::context tls(boost::asio::ssl::context::tls_server);
      auto session=std::make_shared<WSSession>(boost::asio::ssl::stream<tcp::socket>(io,tls),nullptr,nullptr,nullptr);
      auto result=boost::asio::co_spawn(io,WSSessionTestAccess::run(session,scenario),boost::asio::use_future);
      io.run(); result.get();
    }
    {
      boost::asio::io_context io;
      boost::asio::ssl::context tls(boost::asio::ssl::context::tls_server);
      auto session=std::make_shared<WSSession>(boost::asio::ssl::stream<tcp::socket>(io,tls),nullptr,nullptr,nullptr);
      auto result=boost::asio::co_spawn(io,WSSessionTestAccess::pagination(session),boost::asio::use_future);
      io.run(); result.get();
    }
    std::cout<<"PASS disconnect before/during ACK wait, ACK wakeup, timeout, queue overflow and closed writes\n";
    std::cout<<"PASS ordered multi-page sync, out-of-order receipts, partial disconnect/relogin, filtered pages and invalid candidates\n";
  } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}
