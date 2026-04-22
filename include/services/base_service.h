#ifndef BASE_SERVICE_H
#define BASE_SERVICE_H

#include <atomic>
#include <csignal>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>
#include <memory>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>

template <class ServiceType> class BaseServiceServer;

// 服务状态
enum class ServiceState {
  kStopped = 0,
  kStarting = 1,
  kRunning = 2,
  kStopping = 3
};

template <class ServiceType>
BaseServiceServer<ServiceType> *g_service_instance = nullptr;

// 全局停止标志（原子变量，信号处理函数仅修改此标志）
template <class ServiceType>
std::atomic<bool> g_should_stop_service(false);

template <class ServiceType> void signal_handler(int signal) {
  spdlog::info("接收到信号 {}，准备停止服务", signal);
  // 信号处理函数仅设置停止标志，不执行复杂操作
  g_should_stop_service<ServiceType> = true;
}

template <class ServiceType> class BaseServiceServer {
public:
  BaseServiceServer(const std::string &service_name,
                    const std::string &listen_address)
      : service_name_(service_name), listen_address_(listen_address),
        state_(ServiceState::kStopped), server_thread_(),
        should_stop_(false) {
    setupSignalHandler();
  }

  ~BaseServiceServer() {
    Stop(); // 析构时确保停止服务
    if (server_thread_.joinable()) {
      server_thread_.join(); // 等待服务器线程退出
    }
    spdlog::info("[{}] 服务析构完成", service_name_);
  }

  // 异步启动服务（主线程不阻塞，服务器运行在独立线程）
  void Start() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (state_.load() != ServiceState::kStopped) {
        spdlog::warn("[{}] 服务不在停止状态，无法启动", service_name_);
        return;
      }
      state_.store(ServiceState::kStarting);
    }

    // 重置停止标志
    g_should_stop_service<ServiceType> = false;
    should_stop_ = false;

    // 启动独立线程运行服务器，避免主线程阻塞
    server_thread_ = std::thread([this]() {
      RunServer();
    });

    spdlog::info("[{}] 服务启动线程已创建", service_name_);
  }

  // 停止服务（安全的停止逻辑）
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (state_.load() != ServiceState::kRunning) {
        spdlog::warn("[{}] 服务不在运行状态，无法停止", service_name_);
        return;
      }
      state_.store(ServiceState::kStopping);
    }

    // 设置停止标志
    should_stop_ = true;
    g_should_stop_service<ServiceType> = true;

    // 安全关闭gRPC服务器（非阻塞方式）
    if (server_) {
      spdlog::info("[{}] 正在关闭gRPC服务器...", service_name_);
      server_->Shutdown();
    }

    // 等待服务器线程退出
    if (server_thread_.joinable()) {
      spdlog::info("[{}] 等待服务器线程退出...", service_name_);
      server_thread_.join();
    }

    // 更新状态为停止
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      state_.store(ServiceState::kStopped);
    }
    spdlog::info("[{}] 服务已停止", service_name_);
  }

  ServiceState GetState() const { return state_.load(); }
  std::string GetServiceName() const { return service_name_; }
  std::string GetListenAddress() const { return listen_address_; }

private:
  // 服务器运行的核心逻辑（在独立线程中执行）
  void RunServer() {
    spdlog::info("[{}] 正在启动服务，监听地址 {}", service_name_,
                 listen_address_);

    try {
      grpc::EnableDefaultHealthCheckService(true);
      grpc::reflection::InitProtoReflectionServerBuilderPlugin();
      grpc::ServerBuilder builder;

      builder.AddListeningPort(listen_address_,
                               grpc::InsecureServerCredentials());
      builder.RegisterService(static_cast<ServiceType*>(this));
      server_ = builder.BuildAndStart();

      if (!server_) {
        spdlog::error("[{}] gRPC服务器启动失败", service_name_);
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          state_.store(ServiceState::kStopped);
        }
        return;
      }

      // 启动成功，更新状态为 kRunning
      {
        spdlog::info("[{}] 服务器启动成功", service_name_);
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.store(ServiceState::kRunning);
      }

      spdlog::info("[{}] 准备调用 server_->Wait()", service_name_);
      // 等待服务器关闭（同时监控停止标志）
      std::thread monitor_thread([this]() {
        // 监控停止标志，避免Wait()永久阻塞
        while (!should_stop_ && !g_should_stop_service<ServiceType>) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (server_) {
          server_->Shutdown();
        }
      });

      server_->Wait();
      monitor_thread.join(); // 等待监控线程退出
      spdlog::info("[{}] server_->Wait() 返回", service_name_);

      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.store(ServiceState::kStopped);
      }
      spdlog::info("[{}] 服务器已停止", service_name_);

    } catch (const std::exception &e) {
      spdlog::error("[{}] 服务器运行异常: {}", service_name_, e.what());
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.store(ServiceState::kStopped);
      }
    }
  }

  void setupSignalHandler() {
    g_service_instance<ServiceType> = this;
    struct sigaction sa;
    sa.sa_handler = signal_handler<ServiceType>;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    // Ctrl + C
    sigaction(SIGINT, &sa, nullptr);
    // kill 命令默认信号
    sigaction(SIGTERM, &sa, nullptr);
    // 忽略SIGPIPE（避免写入关闭的socket崩溃）
    sigaction(SIGPIPE, &sa, nullptr);
  }

  std::string service_name_;
  std::string listen_address_;
  std::atomic<ServiceState> state_; // 服务状态（原子变量）
  std::atomic<bool> should_stop_;   // 本地停止标志

  std::unique_ptr<grpc::Server> server_;
  std::thread server_thread_;       // 服务器运行线程
  std::mutex state_mutex_;          // 状态保护锁
};

#endif