// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.
//
// 纯 AP 模式：设备自己就是热点，游戏服务端直接监听在该热点网卡上
// （192.168.4.1:25565），不再去连接任何外部路由器。

#ifndef NETWORK_LAYER_H
#define NETWORK_LAYER_H

#include <Arduino.h>
#include <WiFi.h>

class NetworkLayer {
 public:
  explicit NetworkLayer(uint16_t port);

  bool begin();
  void poll();
  bool connected() const;
  WiFiClient accept();

 private:
  void startServer_();
  void stopServer_();

  WiFiServer server_;
  bool server_started_;
};

#endif
