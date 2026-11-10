// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.

#ifndef _WIN32  // Windows 版使用 win_network_layer.cpp, 此文件仅 ESP32 编译

#include "network_layer.h"

NetworkLayer::NetworkLayer(uint16_t port)
    : server_(port),
      server_started_(false) {}

bool NetworkLayer::begin() {
  // 热点已由 wifi_config 启动，这里只负责在热点网卡上开监听
  startServer_();
  return server_started_;
}

// 热点是否还活着：10 秒查一次就够，主循环不再每圈碰 WiFi.softAPIP()。
static bool s_ap_ok = true;
static unsigned long s_next_ap_check_ms = 0;

void NetworkLayer::poll() {
  if ((long)(millis() - s_next_ap_check_ms) >= 0) {
    s_next_ap_check_ms = millis() + 10000UL;
    s_ap_ok = ((uint32_t)WiFi.softAPIP()) != 0;
  }
  if (!s_ap_ok) {
    stopServer_();                 // 热点没了，监听也没意义
    return;
  }
  if (!server_started_) {
    startServer_();                // 热点回来后重建监听
  }
}

bool NetworkLayer::connected() const {
  return s_ap_ok;
}

WiFiClient NetworkLayer::accept() {
  if (!server_started_) {
    startServer_();
    if (!server_started_) return WiFiClient();
  }
  return server_.available();
}

void NetworkLayer::startServer_() {
  if (server_started_) return;

  server_.begin();
  server_.setNoDelay(true);
  server_started_ = true;
  Serial.printf("Server listening on port 25565 (hotspot %s)\r\n",
                WiFi.softAPIP().toString().c_str());
}

void NetworkLayer::stopServer_() {
  if (!server_started_) return;
  server_.end();
  server_started_ = false;
  Serial.println("+MC:SRV_DOWN,AP_LOST");
}

#endif
