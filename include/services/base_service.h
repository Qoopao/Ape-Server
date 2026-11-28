#ifndef BASE_SERVICE_H
#define BASE_SERVICE_H

#include <algorithm>
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
#include "util/instance_id.h"
#include "om/otel_grpc_interceptor.h"

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
        etcd_registered_(false), defer_registration_(false), ready_(false),
        watchdog_stop_(false) {}

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
    StartWatchdog();

    spdlog::info("[{}] 服务启动线程已创建", service_name_);
  }

  // 停止服务（幂等：drain handler 与析构都会调用）
  void Stop() {
    bool was_running = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (state_.load() == ServiceState::kRunning) {
        state_.store(ServiceState::kStopping);
        was_running = true;
      }
    }
    if (!was_running)
      spdlog::debug("[{}] 服务不在运行状态，跳过注销", service_name_);

    // 先置停止标志，让注册重试循环/看门狗尽快退出
    should_stop_ = true;
    g_should_stop_service<ServiceType> = true;
    watchdog_stop_ = true;
    if (watchdog_thread_.joinable())
      watchdog_thread_.join();

    if (!was_running) {
      if (server_)
        server_->Shutdown();
      if (server_thread_.joinable())
        server_thread_.join();
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (state_.load() != ServiceState::kStopped)
        state_.store(ServiceState::kStopped);
      return;
    }

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
  const std::string &GetInstanceId() const { return instance_id_; }
  std::vector<std::string> GetMethods() const { return methods_; }

  // 获取 BackbonClient（供子类通过服务发现查找其他服务）
  BackbonClient *GetBackbonClient() const { return backbon_client_.get(); }

  // 启用 etcd 服务注册（在 Start() 之前调用）
  // instance_id 为空时按 hostname:监听端口 推导；重启后必须保持稳定（Q-1）
  void EnableEtcdRegistration(const std::string &backbon_service_addr,
                              const std::vector<std::string> &methods,
                              const std::string &instance_id = "") {
    backbon_addr_ = backbon_service_addr;
    methods_ = methods;
    instance_id_ = instance_id.empty()
                       ? ape::instance::ForListenAddress(listen_address_)
                       : instance_id;
  }

  // 检查本服务是否已注册到 etcd
  bool IsEtcdRegistered() const { return etcd_registered_.load(); }

  // 默认 Start() 返回后即可注册。若子类在 Start() 之后还有自身初始化
  // （如 PushServer 的 router/Kafka consumer），在 Start() 前调用本函数，
  // 初始化完成后再调用 MarkReady()，注册线程会等就绪后再注册。
  void DeferRegistration() { defer_registration_.store(true); }
  void MarkReady() {
    if (!ready_.exchange(true))
      spdlog::info("[{}] 服务自身初始化完成，允许注册", service_name_);
  }

  // 阻塞等待本服务完成etcd注册（带超时）
  // 返回false：超时、停止中、或服务启动失败（如端口绑定失败）
  // 返回true：未启用etcd注册（没有可等待的东西）、成功注册
  bool WaitForEtcdRegistration(int timeout_seconds = 30) {
    if (backbon_addr_.empty()){   // 后端骨干服务未启动，无法注册服务
      return true;
    }
      
    const int slices = std::max(1, timeout_seconds * 2); // 每 500ms 一次
    for (int elapsed = 0; elapsed < slices; ++elapsed) {
      if (etcd_registered_.load()) {
        spdlog::info("[{}] etcd registration confirmed", service_name_);
        return true;
      }
      // 绑定失败时 RunServer 会把状态置回 kStopped，不能白等 30s
      if (state_.load() == ServiceState::kStopped) {
        spdlog::error("[{}] 服务启动失败（监听 {} 是否被占用？），中止等待注册", service_name_, listen_address_);
        return false;
      }
      if (should_stop_.load() || state_.load() == ServiceState::kStopping) {
        spdlog::warn("[{}] 服务在等待注册期间停止", service_name_);
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (etcd_registered_.load()){   // 最后检查一次
      return true;
    }
    spdlog::warn("[{}] WaitForEtcdRegistration timeout after {}s", service_name_, timeout_seconds);
    return false;
  }

private:
  // 在普通线程里同步等待一个协程结果（注册/发现调用使用）
  template <typename T, typename Fn> static T RunSync(Fn task) {
    boost::asio::io_context ioc;
    std::promise<T> promise;
    auto future = promise.get_future();
    boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void> {
          promise.set_value(co_await task());
        },
        [&promise](std::exception_ptr ep) {
          if (!ep)
            return;
          try {
            std::rethrow_exception(ep);
          } catch (...) {
            promise.set_exception(std::current_exception());
          }
        });
    ioc.run();
    return future.get();
  }

  bool SleepInterruptible(int total_ms) {
    for (int slept = 0; slept < total_ms; slept += 100) {
      if (should_stop_.load() || watchdog_stop_.load() ||
          g_should_stop_service<ServiceType>.load())
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
  }

  // 单次注册尝试。失败返回 false，由调用方决定是否重试。
  bool TryRegisterOnce() {
    try {
      if (!backbon_client_) {
        backbon_channel_ = grpc::CreateChannel(
            backbon_addr_, grpc::InsecureChannelCredentials());
        backbon_client_ = std::make_unique<BackbonClient>(backbon_channel_);
      }

      // 上报真实可路由地址：bind 0.0.0.0 可以，但注册值不能是 0.0.0.0（C-5）
      ServiceInfo info;
      info.service = service_name_;
      info.instance_id = instance_id_;
      info.address = ape::instance::AdvertiseAddress(listen_address_);
      info.methods = methods_;

      auto resp = RunSync<backbon::RegisterServiceResp>(
          [this, &info]() { return backbon_client_->RegisterService(info); });
      if (resp.success()) {
        spdlog::info("[{}] 已成功注册到 etcd (via BackbonService)",
                     service_name_);
        return true;
      }
      spdlog::error("[{}] etcd 注册失败: {}", service_name_, resp.error());
    } catch (const std::exception &e) {
      spdlog::error("[{}] etcd 注册异常: {}", service_name_, e.what());
    }
    return false;
  }

  // 带指数退避的重试（500ms → 8s），直到成功或收到停止信号。
  // 调用方必须持有 registration_mutex_。
  void RegisterWithRetryLocked() {
    int backoff_ms = 500;
    while (!should_stop_.load() &&
           !g_should_stop_service<ServiceType>.load()) {
      if (TryRegisterOnce()) {
        etcd_registered_.store(true);
        return;
      }
      spdlog::warn("[{}] etcd 注册未成功，{}ms 后重试", service_name_,
                   backoff_ms);
      if (!SleepInterruptible(backoff_ms))
        return;
      backoff_ms = std::min(backoff_ms * 2, 8000);
    }
  }

  void RegisterWithRetry() {
    if (backbon_addr_.empty())
      return;
    std::lock_guard<std::mutex> lock(registration_mutex_);
    RegisterWithRetryLocked();
  }

  // 看门狗：etcd 重启/租约丢失后注册会消失，定期确认本实例仍在，
  // 不在则重新注册（KeepAlive 失效无法从客户端侧直接感知）。
  void StartWatchdog() {
    if (backbon_addr_.empty())
      return;
    watchdog_stop_.store(false);
    watchdog_thread_ = std::thread([this]() {
      // 独立 channel：长期查询，避免与注册线程共享可变状态
      auto channel = grpc::CreateChannel(backbon_addr_,
                                         grpc::InsecureChannelCredentials());
      BackbonClient client(channel);
      while (!watchdog_stop_.load() && !should_stop_.load() &&
             !g_should_stop_service<ServiceType>.load()) {
        if (!SleepInterruptible(30000))
          return;
        if (!etcd_registered_.load())
          continue;
        bool present = false;
        try {
          auto resp = RunSync<backbon::GetServiceResp>(
              [&client, this]() { return client.GetServicesList(service_name_); });
          for (const auto &instance : resp.instances()) {
            if (instance.instance_id() == instance_id_) {
              present = true;
              break;
            }
          }
        } catch (const std::exception &e) {
          spdlog::warn("[{}] 注册看门狗查询失败: {}", service_name_, e.what());
          continue;
        }
        if (present)
          continue;
        spdlog::warn("[{}] etcd 中的注册已消失，重新注册", service_name_);
        std::lock_guard<std::mutex> lock(registration_mutex_);
        if (should_stop_.load() || watchdog_stop_.load())
          return;
        etcd_registered_.store(false);
        RegisterWithRetryLocked();
      }
    });
  }

  // 从 etcd 注销本服务
  void UnregisterFromEtcd() {
    std::lock_guard<std::mutex> lock(registration_mutex_);
    etcd_registered_.store(false);
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
            auto resp = co_await backbon_client_->UnregisterService(service_name_,
                                                              instance_id_);
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

      // 等子类完成自身初始化（默认立即就绪，见 DeferRegistration）
      while (defer_registration_.load() && !ready_.load() &&
             !should_stop_.load() &&
             !g_should_stop_service<ServiceType>.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (should_stop_.load() || g_should_stop_service<ServiceType>.load()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.store(ServiceState::kStopped);
        return;
      }

      // 向 etcd 注册本服务（带退避重试，直到成功或停止）
      RegisterWithRetry();
      if (should_stop_.load() || g_should_stop_service<ServiceType>.load()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.store(ServiceState::kStopped);
        return;
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

  std::string service_name_;
  std::string listen_address_;
  std::atomic<ServiceState> state_; // 服务状态（原子变量）
  std::atomic<bool> should_stop_;   // 本地停止标志

  std::unique_ptr<grpc::Server> server_;
  std::thread server_thread_; // 服务器运行线程
  std::mutex state_mutex_;    // 状态保护锁

  // etcd 服务发现相关
  std::string backbon_addr_;
  std::string instance_id_;
  std::vector<std::string> methods_;
  std::shared_ptr<grpc::Channel> backbon_channel_;
  std::unique_ptr<BackbonClient> backbon_client_;
  std::atomic<bool> etcd_registered_;

  // 注册流程控制
  std::mutex registration_mutex_; // 注册/注销互斥（含看门狗重新注册）
  std::atomic<bool> defer_registration_{false}; // 等 MarkReady 再注册
  std::atomic<bool> ready_{false};
  std::atomic<bool> watchdog_stop_{false};
  std::thread watchdog_thread_;
};

#endif