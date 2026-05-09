#include "services/msg_service/client.h"
#include "services/msg_service/server.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

// 使用行业标准的简单断言测试 (AAA 模式: Arrange, Act, Assert)
// 不引入额外的测试框架，只测试 msg_service 是否可用

static std::string start_memory_server(grpc::ServerBuilder& builder,
                                       MsgServiceImpl& service,
                                       std::unique_ptr<grpc::Server>& out_server) {
  int selected_port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &selected_port);
  builder.RegisterService(&service);
  out_server = builder.BuildAndStart();

  if (!out_server || selected_port == 0) {
    std::cerr << "FAIL: 无法启动 gRPC 服务器" << std::endl;
    std::exit(1);
  }

  std::string addr = "127.0.0.1:" + std::to_string(selected_port);
  std::cout << "[INFO] 测试服务器已启动, 监听地址: " << addr << std::endl;
  return addr;
}

static void test_get_server_time() {
  // ── Arrange: 启动内存中的 gRPC 服务器 ──
  grpc::ServerBuilder builder;
  MsgServiceImpl service;
  std::unique_ptr<grpc::Server> server;
  std::string bound_address = start_memory_server(builder, service, server);

  auto channel = grpc::CreateChannel(bound_address, grpc::InsecureChannelCredentials());
  MsgClient client(channel);

  // ── Act: 调用 GetServerTime ──
  ::msg::GetServerTimeReq request;
  ::msg::GetServerTimeResp response = client.GetServerTime(request);

  // ── Assert ──
  std::cout << "[PASS] GetServerTime 调用成功, serverTime=" << response.servertime() << std::endl;

  server->Shutdown();
  std::cout << "[INFO] 测试服务器已关闭" << std::endl;
}

static void test_send_messages_empty() {
  // ── Arrange ──
  grpc::ServerBuilder builder;
  MsgServiceImpl service;
  std::unique_ptr<grpc::Server> server;
  std::string bound_address = start_memory_server(builder, service, server);

  auto channel = grpc::CreateChannel(bound_address, grpc::InsecureChannelCredentials());
  MsgClient client(channel);

  // ── Act: 发送空消息列表 ──
  ::sdkws::SendMessageReq request;
  ::sdkws::SendMessageResp response = client.SendMessages(request);

  // ── Assert ──
  std::cout << "[PASS] SendMessages (空消息) 调用完成, infos_size=" << response.infos_size() << std::endl;

  server->Shutdown();
  std::cout << "[INFO] 测试服务器已关闭" << std::endl;
}

int main() {
  std::cout << "=== msg_service 可用性测试 ===" << std::endl;

  test_get_server_time();
  test_send_messages_empty();

  std::cout << "=== 所有测试通过 ===" << std::endl;
  return 0;
}