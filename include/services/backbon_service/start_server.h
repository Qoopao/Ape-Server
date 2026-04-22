#ifndef START_SERVER_H
#define START_SERVER_H

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

template <class ServiceType> void signal_handler(int signal) {
  spdlog::info("接收到信号 {}，准备停止服务", signal);
  if (g_service_instance<ServiceType>) {
    g_service_instance<ServiceType>->Stop(); // 调用 Stop()
  } else {
    spdlog::warn("服务实例为空，无需停止");
  }
}

template <class ServiceType> class RpcServer {
public:
  RpcServer(const std::string &service_name, const std::string &listen_address)
      : service_name_(service_name), listen_address_(listen_address),
        state_(ServiceState::kStopped) {

    setupSignalHandler();
  }

  ~RpcServer() {
    if (state_.load() == ServiceState::kStopped ||
        state_.load() == ServiceState::kStopping) {
      return;
    }

    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      state_.store(ServiceState::kStopping);
      spdlog::info("[{}] 正在停止服务...", service_name_);

      state_.store(ServiceState::kStopped);
      spdlog::info("[{}] 服务已停止", service_name_);
    }
  }

  // 异步回调式启动服务，比基于完成队列的更容易使用
  void Start() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (state_.load() != ServiceState::kStopped) {
        spdlog::warn("[{}] 服务不在停止状态，无法启动", service_name_);
        return;
      }
      state_.store(ServiceState::kStarting);
    }
    spdlog::info("[{}] 正在启动服务，监听地址 {}", service_name_,
                 listen_address_);

    try {
      grpc::EnableDefaultHealthCheckService(true);
      grpc::reflection::InitProtoReflectionServerBuilderPlugin();
      grpc::ServerBuilder builder;

      builder.AddListeningPort(listen_address_,
                               grpc::InsecureServerCredentials());
      builder.RegisterService(this);
      server_ = builder.BuildAndStart();

      if (!server_) {
        spdlog::error("[{}] gRPC服务器启动失败", service_name_);
        state_.store(ServiceState::kStopped);
        return;
      }

      // 启动成功，更新状态为 kRunning
      {
        spdlog::info("[{}] 服务器启动成功", service_name_);
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.store(ServiceState::kRunning);
        spdlog::info("[{}] 当前 state_ 值 = {}", service_name_,
                     static_cast<int>(state_.load()));
      }

      server_->Wait();

      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.store(ServiceState::kStopped);
      }
      spdlog::info("[{}] 服务器已停止", service_name_);

    } catch (const std::exception &e) {
      spdlog::error("[{}] 启动异常: {}", service_name_, e.what());
      state_.store(ServiceState::kStopped);
    }
  }

  // 停止服务
  void Stop() {
    spdlog::info("[{}] Stop()被调用， 当前 state_ 值 = {}", service_name_,
                 static_cast<int>(state_.load()));
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (state_.load() != ServiceState::kRunning) {
        spdlog::warn("[{}] 服务不在运行状态，无法停止", service_name_);
        return;
      }
    }
    state_.store(ServiceState::kStopping);
    spdlog::info("[{}] 正在停止服务...", service_name_);

    server_->Shutdown();

    state_.store(ServiceState::kStopped);
    spdlog::info("[{}] 服务已停止", service_name_);
  }

  ServiceState GetState() const { return state_.load(); }
  std::string GetServiceName() const { return service_name_; }
  std::string GetListenAddress() const { return listen_address_; }

private:
  void setupSignalHandler() {
    g_service_instance<ServiceType> = this;
    struct sigaction sa;
    sa.sa_handler = signal_handler<ServiceType>;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    // Ctrl + C
    sigaction(SIGINT, &sa, nullptr);
    // kill -9
    sigaction(SIGTERM, &sa, nullptr);
  }

  std::string service_name_;
  std::string listen_address_;
  std::atomic<ServiceState>
      state_; // 服务可能被多线程访问（如另一个线程停止改服务），所以用原子变量

  std::unique_ptr<grpc::Server> server_;

  std::mutex state_mutex_;
};

#endif

#endif
