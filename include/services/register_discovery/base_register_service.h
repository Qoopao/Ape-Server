#ifndef BASE_REGISTER_SERVICE_H
#define BASE_REGISTER_SERVICE_H

#include "services/register_discovery/etcd_service_register.h"
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>
#include <memory>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>

class BaseRegisterService {
public:
  // 初始化信号处理
  BaseRegisterService() = default;

  // 虚析构，避免子类内存泄漏
  virtual ~BaseRegisterService() {
    this->shutdown();
  }

  // 绑定EtcdRegistry实例（单服务单实例，外部保证唯一）
  void setRegistry(std::unique_ptr<EtcdServiceRegistry> registry) {
    if (this->etcd_service_registry) {
      spdlog::warn("EtcdRegistry已设置，禁止重复赋值！");
      return;
    }
    this->etcd_service_registry = std::move(registry);
  }

  // 通用启动函数，这里后面要改成多线程来处理大量请求（写一个）
  void startup(const std::string &server_addr) {
    // 1. 检查是否设置了etcd注册中心
    if (!etcd_service_registry) {
      spdlog::error("请先调用setRegistry设置etcd注册中心");
      return;
    }

    try {
      // 2. 注册服务
      etcd_service_registry->RegisterService();

      // 3. 启动服务
      grpc::ServerBuilder builder;
      // 基于这个builder.AddCompletionQueue();加上线程池来处理请求
      builder.AddListeningPort(server_addr, grpc::InsecureServerCredentials());

      // 调用子类的注册函数
      registerGrpcService(builder);
      this->server = builder.BuildAndStart();
      spdlog::info("msg_service启动成功, 监听地址: {}", server_addr);

      // 另开线程等待服务停止
      std::thread wait_thread([this]() {
        if (this->server)
          server->Wait();
      });

      // 调用子类的独立等待函数，避免使用全局信号处理
      independentWait();

      // 等待服务线程结束
      this->shutdown();
      if (wait_thread.joinable()) {
        wait_thread.join(); // 此时server已Shutdown，线程可正常退出
      }

    } catch (const std::runtime_error &e) {
      // 处理运行时错误
      spdlog::error(e.what());
      this->shutdown();
      return;
    } catch (const std::exception &e) {
      // 捕获所有标准异常
      spdlog::error(e.what());
      this->shutdown();
    } catch (...) {
      // 捕获非标准异常
      spdlog::error("启动服务时未知异常触发");
      this->shutdown();
    }
  }

  // 通用关闭函数
  void shutdown() {
    static std::atomic<bool> has_shutdown(false);
    if (has_shutdown.exchange(true)) {
      return;
    }

    if (etcd_service_registry) {
      try {
        // 1. 停止续约
        etcd_service_registry->UnregisterService();

        // 2. 停止服务
        if (this->server) {
          spdlog::info("msg_service服务将于最多5秒后停止");
          // 阻塞的，如果没有rpc请求，那么立即返回
          this->server->Shutdown(std::chrono::system_clock::now() +
                                 std::chrono::seconds(5));
          spdlog::info("msg_service服务已停止");
          this->server.reset();
        } else {
          spdlog::warn("服务未启动，无法停止服务");
        }
      } catch (const std::exception &e) {
        spdlog::error(e.what());
      } catch (...) {
        spdlog::error("停止服务时出现未知异常");
      }
    } else {
      spdlog::warn("请先调用setRegistry设置etcd注册中心");
    }
  }

  virtual void registerGrpcService(grpc::ServerBuilder &builder) = 0;
  virtual void independentWait() = 0;

private:
  std::unique_ptr<EtcdServiceRegistry> etcd_service_registry;
  std::unique_ptr<grpc::Server> server;

};

#endif
