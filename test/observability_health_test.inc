// 健康检查端到端小测试：起 HealthServer，用裸 TCP 发 HTTP 请求。
// 由 observability_test.cpp 在匿名命名空间内 include。

int HttpStatus(uint16_t port, const std::string &path) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0) {
      timeval timeout{2, 0};
      ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      const std::string request =
          std::string("GET ") + path +
          " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
      ::send(fd, request.data(), request.size(), 0);
      char buffer[256]{};
      const ssize_t received = ::recv(fd, buffer, sizeof(buffer) - 1, 0);
      ::close(fd);
      if (received > 0) {
        int status = 0;
        if (std::sscanf(buffer, "HTTP/1.1 %d", &status) == 1)
          return status;
      }
      return -1;
    }
    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return -1;
}

void TestHealthServer() {
  ape::ops::HealthServer server;
  bool ready = false;
  Check(server.Start(18991, [&ready]() { return ready; }), "health server starts");
  Check(HttpStatus(18991, "/healthz") == 200, "healthz returns 200");
  Check(HttpStatus(18991, "/readyz") == 503, "readyz returns 503 before ready");
  ready = true;
  Check(HttpStatus(18991, "/readyz") == 200, "readyz returns 200 after ready");
  server.Stop();
}
