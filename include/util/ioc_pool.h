#pragma once

#include <boost/asio.hpp>

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

  // 获取指定 index 的 io_context 引用
  boost::asio::io_context &get_ioc(int index) {
    return _workers[index]->io_context;
  }

  // 获取 pool size
  int pool_size() const { return _pool_size; }

private:
  int _pool_size;
  std::vector<std::unique_ptr<Worker>> _workers;
  std::vector<std::thread> _threads;
  std::mutex _mutex;
};