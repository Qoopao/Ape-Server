#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast.hpp>

#include <climits>
#include <cstdint>
#include <grpcpp/channel.h>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

class AuthClient;
class MsgClient;
class BackbonClient;
class PushClient;

using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::use_awaitable;
using boost::asio::ip::tcp;

class IOC_Pool {

  // 一个Worker对应一个ioc
  struct Worker {

    Worker()
        : io_context(), work_guard(boost::asio::make_work_guard(io_context)) {};

    boost::asio::io_context io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        work_guard;
    std::atomic<int> active_tasks{0};
  };

public:
  IOC_Pool(int pool_size) : _pool_size(pool_size) {
    // 创建_workers
    _workers.reserve(pool_size);
    for (int i = 0; i < pool_size; ++i) {
      _workers.emplace_back(std::make_unique<Worker>());
    }
  };
  ~IOC_Pool() { stopPool(); };

  void startPool() {
    for (auto &w : _workers) {
      _threads.emplace_back([worker = w.get()] { worker->io_context.run(); });
    }
  }

  void stopPool() {
    // 释放 work_guard
    for (auto &w : _workers) {
      w->work_guard.reset();
    }
    // 停止ioc
    for (auto &w : _workers) {
      w->io_context.stop();
    }
    // 线程等待退出
    for (auto &t : _threads) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  // 提交任务到选中的ioc
  template <typename F> void spawn(F &&f) {
    // 获取连接数最少的ioc，做负载均衡
    std::lock_guard<std::mutex> lock{_mutex};
    int min_load = INT_MAX;
    Worker *selected = nullptr;

    for (auto &w : _workers) {
      int load = w->active_tasks.load(std::memory_order_acquire);
      if (load < min_load) {
        min_load = load;
        selected = w.get();
      }
    }

    auto &ioc = selected->io_context;

    // 活跃连接+1
    selected->active_tasks.fetch_add(1, std::memory_order_release);

    boost::asio::co_spawn(
        ioc, std::forward<F>(f), [selected](std::exception_ptr) {
          // 任务结束自动 -1
          selected->active_tasks.fetch_sub(1, std::memory_order_release);
        });
  }

private:
  int _pool_size;
  std::vector<std::unique_ptr<Worker>> _workers;
  std::vector<std::thread> _threads;
  std::mutex _mutex;
};

class WebServer {

public:
  WebServer(int ioc_pool_size, uint16_t port,
            std::unique_ptr<BackbonClient> backbon_client);
  ~WebServer() = default;

  boost::asio::awaitable<void> listener();

  void start();
  void stop();

  void discover_Service(const std::string service_name,
                        const std::string &service_addr);

  boost::asio::io_context &get_acceptor_ioc() { return _acceptor_ioc; }

  // 供 WSSession 获取 AuthClient、MsgClient、PushClient
  AuthClient *getAuthClient() { return auth_client_.get(); }
  MsgClient *getMsgClient() { return msg_client_.get(); }
  PushClient *getPushClient() { return push_client_.get(); }

private:
  boost::asio::ip::tcp::acceptor acceptor_;

  IOC_Pool _iocPool;
  boost::asio::io_context _acceptor_ioc;
  std::shared_ptr<grpc::Channel> auth_channel_;
  std::unique_ptr<AuthClient> auth_client_;
  std::shared_ptr<grpc::Channel> msg_channel_;
  std::unique_ptr<MsgClient> msg_client_;
  std::shared_ptr<grpc::Channel> push_channel_;
  std::unique_ptr<PushClient> push_client_;
  std::unique_ptr<BackbonClient> backbon_client_;
};

#endif