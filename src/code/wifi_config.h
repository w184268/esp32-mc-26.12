// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.
//
// 纯 AP 模式配网：设备常驻一个热点，不再连接路由器。
// 网页只负责设置/清除热点密码；连上热点后访问 http://192.168.4.1/ 即可。

#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// 启动热点 + 网页（含 DNS 劫持）。hold_ms：长按 BOOT 多久清空热点密码。
void wifiCfgInit(uint8_t io0_pin, unsigned long hold_ms);
void wifiCfgPoll(void);

const char *wifiCfgApSsid(void);
const char *wifiCfgApPassword(void);   // 空串表示开放热点
bool wifiCfgApIsOpen(void);
const char *wifiCfgApIpString(void);
bool wifiCfgApReady(void);

// 可选 STA（连家里路由器）：未配置时始终返回 0 / "0.0.0.0"
const char *wifiCfgStaIpString(void);
bool wifiCfgStaConnected(void);
bool wifiCfgStaEnabled(void);        // 本固件编译开关：true=AP+STA，false=纯热点
const char *wifiCfgStaSsid(void);    // NVS 里保存的路由器名字（没配过是空串）
const char *wifiCfgModeString(void); // 给日志用的一句话：当前到底是哪种网络模式

// 清空热点密码（恢复开放）并重启，串口命令与长按 BOOT 都走这里
void wifiCfgResetPasswordToOpen(void);

#endif
