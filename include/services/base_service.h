#ifndef BASE_SERVICE_H
#define BASE_SERVICE_H

#include <atomic>
#include <boost/asio.hpp>
#include <csignal>
#include <future>
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
#include <vector>

#include "services/backbon_service/client.h"
#include "util/otel_grpc_interceptor.h"

template <class ServiceType> class BaseServiceServer;

// 服务状态
enum class ServiceState {
  kStopped = 0,
  kStarting = 1,
  kRunning = 2,
  kStopping = 3
};

template <class ServiceType>
inline BaseServiceServer<ServiceType> *g_service_instance = nullptr;

// 全局停止标志（原子变量，信号处理函数仅修改此标志）
template <class ServiceType>
inline std::atomic<bool> g_should_stop_service(false);

template <class ServiceType> class BaseServiceServer {
public:
  BaseServiceServer(const std::string &service_name,
                    const std::string &listen_address)
      : service_name_(service_name), listen_address_(listen_address),
        state_(ServiceState::kStopped), server_thread_(), should_stop_(false),
        etcd_registered_(false) {}

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
    server_thread_ = std::thread([this]() { RunServer(); });

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

    // 从 etcd 注销本服务
    UnregisterFromEtcd();

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

  // 获取 BackbonClient（供子类通过服务发现查找其他服务）
  BackbonClient *GetBackbonClient() const { return backbon_client_.get(); }

  // 启用 etcd 服务注册（在 Start() 之前调用）
  void EnableEtcdRegistration(const std::string &backbon_service_addr,
                              const std::vector<std::string> &methods) {
    backbon_addr_ = backbon_service_addr;
    methods_ = methods;
  }

  // 检查本服务是否已注册到 etcd
  bool IsEtcdRegistered() const { return etcd_registered_.load(); }

  // 阻塞等待本服务注册到 etcd（带超时）
  // 返回 true 表示注册成功，false 表示超时或未启用 etcd
  bool WaitForEtcdRegistration(int timeout_seconds = 30) {
    if (backbon_addr_.empty()) {
      // 未启用 etcd 注册，直接返回成功
      return true;
    }
    int elapsed = 0;
    while (!etcd_registered_.load()) {
      if (elapsed >= timeout_seconds * 2) {  // 每 500ms 检查一次
        spdlog::warn("[{}] WaitForEtcdRegistration timeout after {}s",
                     service_name_, timeout_seconds);
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      elapsed++;
    }
    spdlog::info("[{}] etcd registration confirmed", service_name_);
    return true;
  }

private:
  // 向 BackbonService 注册本服务到 etcd
  void RegisterToEtcd() {
    if (backbon_addr_.empty())
      return;

    try {
      backbon_channel_ = grpc::CreateChannel(
          backbon_addr_, grpc::InsecureChannelCredentials());
      backbon_client_ = std::make_unique<BackbonClient>(backbon_channel_);

      ServiceInfo info;
      info.service = service_name_;
      info.ipports.push_back(listen_address_);
      info.methods = methods_;

      // 用 promise/future 同步等待协程结果
      // 使用自定义 completion handler 而非 detached，避免协程异常触发 std::terminate()
      boost::asio::io_context ioc;
      std::promise<backbon::RegisterServiceResp> promise;
      auto future = promise.get_future();

      boost::asio::co_spawn(
          ioc,
          [&]() -> boost::asio::awaitable<void> {
            auto resp = co_await backbon_client_->RegisterService(info);
            promise.set_value(std::move(resp));
          },
          [&promise](std::exception_ptr ep) {
            if (ep) {
              promise.set_exception(ep);
            }
          });

      ioc.run();
      auto resp = future.get();

      if (resp.success()) {
        etcd_registered_.store(true);
        spdlog::info("[{}] 已成功注册到 etcd (via BackbonService)",
                     service_name_);
      } else {
        spdlog::error("[{}] etcd 注册失败: {}", service_name_, resp.error());
      }
    } catch (const std::exception &e) {
      spdlog::error("[{}] etcd 注册异常: {}", service_name_, e.what());
    }
  }

  // 从 etcd 注销本服务
  void UnregisterFromEtcd() {
    if (!backbon_client_)
      return;

    try {
      // 用 promise/future 同步等待协程结果
      // 使用自定义 completion handler 而非 detached，避免协程异常触发 std::terminate()
      boost::asio::io_context ioc;
      std::promise<backbon::UnregisterServiceResp> promise;
      auto future = promise.get_future();

      boost::asio::co_spawn(
          ioc,
          [&]() -> boost::asio::awaitable<void> {
            auto resp = co_await backbon_client_->UnregisterService();
            promise.set_value(std::move(resp));
          },
          [&promise](std::exception_ptr ep) {
            if (ep) {
              promise.set_exception(ep);
            }
          });

      ioc.run();
      auto resp = future.get();

      if(resp.success()){
        spdlog::info("[{}] 已从 etcd 注销", service_name_);
      }else{
        spdlog::error("[{}] 注销失败: {}", service_name_, resp.error());
      }
    } catch (const std::exception &e) {
      spdlog::error("[{}] etcd 注销异常: {}", service_name_, e.what());
    }

    backbon_client_.reset();
    backbon_channel_.reset();
  }

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
      builder.RegisterService(static_cast<ServiceType *>(this));

      // 注册 OTel gRPC 拦截器 (自动为所有 RPC 方法创建 Trace Span)
      auto otel_factory =
          ape::otel::CreateOtelServerInterceptorFactory(service_name_);
      std::vector<std::unique_ptr<
          grpc::experimental::ServerInterceptorFactoryInterface>>
          interceptor_creators;
      interceptor_creators.push_back(std::move(otel_factory));
      builder.experimental().SetInterceptorCreators(
          std::move(interceptor_creators));

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

      // 向 etcd 注册本服务
      RegisterToEtcd();

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

  std::string service_name_;
  std::string listen_address_;
  std::atomic<ServiceState> state_; // 服务状态（原子变量）
  std::atomic<bool> should_stop_;   // 本地停止标志

  std::unique_ptr<grpc::Server> server_;
  std::thread server_thread_; // 服务器运行线程
  std::mutex state_mutex_;    // 状态保护锁

  // etcd 服务发现相关
  std::string backbon_addr_;
  std::vector<std::string> methods_;
  std::shared_ptr<grpc::Channel> backbon_channel_;
  std::unique_ptr<BackbonClient> backbon_client_;
  std::atomic<bool> etcd_registered_;
};

#endif