// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../../NOTICE.

#ifdef _WIN32

#include "win_network_layer.h"
#include <stdio.h>

NetworkLayer::NetworkLayer(uint16_t port)
    : port_(port),
      listen_socket_(INVALID_SOCKET),
      server_started_(false),
      wsa_initialized_(false) {}

NetworkLayer::~NetworkLayer() {
  if (listen_socket_ != INVALID_SOCKET) {
    closesocket(listen_socket_);
  }
  if (wsa_initialized_) {
    WSACleanup();
  }
}

bool NetworkLayer::begin() {

  // 初始化 Winsock
  WSADATA wsa_data;
  int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
  if (result != 0) {
    printf("WSAStartup failed: %d\n", result);
    return false;
  }
  wsa_initialized_ = true;

  return startServer_();
}

void NetworkLayer::poll() {
  // Windows 版不需要 WiFi 重连逻辑
}

bool NetworkLayer::connected() const {
  return server_started_;
}

SOCKET NetworkLayer::acceptClient() {
  if (!server_started_) return INVALID_SOCKET;

  // 非阻塞 accept
  struct sockaddr_in client_addr;
  int addr_len = sizeof(client_addr);
  SOCKET client = accept(listen_socket_, (struct sockaddr*)&client_addr, &addr_len);

  if (client == INVALID_SOCKET) {
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) return INVALID_SOCKET;
    return INVALID_SOCKET;
  }

  // 设置客户端 socket 为非阻塞
  win_set_nonblocking(client);

  // 禁用 Nagle 算法
  int flag = 1;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));

  char addr_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
  printf("New connection from %s:%d\n", addr_str, ntohs(client_addr.sin_port));

  return client;
}

bool NetworkLayer::startServer_() {
  listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_socket_ == INVALID_SOCKET) {
    printf("socket() failed: %d\n", WSAGetLastError());
    return false;
  }

  // 允许端口复用
  int opt = 1;
  setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

  // 绑定
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (bind(listen_socket_, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
    printf("bind() failed: %d\n", WSAGetLastError());
    closesocket(listen_socket_);
    listen_socket_ = INVALID_SOCKET;
    return false;
  }

  if (listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR) {
    printf("listen() failed: %d\n", WSAGetLastError());
    closesocket(listen_socket_);
    listen_socket_ = INVALID_SOCKET;
    return false;
  }

  // 设置监听 socket 为非阻塞
  win_set_nonblocking(listen_socket_);

  server_started_ = true;
  printf("Server listening on 0.0.0.0:%d\n", port_);
  printf("Connect with: localhost:%d\n", port_);
  return true;
}

#endif // _WIN32
