// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../../NOTICE.

#ifndef WIN_NETWORK_LAYER_H
#define WIN_NETWORK_LAYER_H

#ifdef _WIN32

#include "win_platform.h"
#include <stdint.h>

class NetworkLayer {
 public:
  explicit NetworkLayer(uint16_t port);
  ~NetworkLayer();

  bool begin();
  void poll();
  bool connected() const;
  SOCKET acceptClient();  // 返回新客户端 socket, INVALID_SOCKET 表示无连接

 private:
  bool startServer_();

  uint16_t port_;
  SOCKET listen_socket_;
  bool server_started_;
  bool wsa_initialized_;
};

#endif // _WIN32
#endif // WIN_NETWORK_LAYER_H
