// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.

/**
 * ESP32MC - Minecraft Java Server on ESP32
 * 协议版本 26.1.2 / 775
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_attr.h>
#include "mc_server.h"
#include "wifi_config.h"
#include "packet_codec.h"
#include "world_store.h"
#include "game_state.h"

static const uint16_t MC_PORT = 25565;
static const uint8_t kWebCfgButtonPin = 0;      // IO0 / BOOT 键

// IO9 只在 ESP32-S3 上是普通 GPIO。经典 ESP32（ESP32-D0WD/WROOM-32）的
// GPIO6~11 全部接着板载 SPI Flash，其中 GPIO9 是 SD2——一旦 pinMode/digitalWrite
// 就会让 CPU 取指失败，直接触发 TG1 看门狗复位（rst:0x8）。
#if defined(CONFIG_IDF_TARGET_ESP32S3)
static const uint8_t kLedPin = 9;               // IO9 活动灯（S3）
#else
static const uint8_t kLedPin = 2;               // IO2 活动灯（经典 ESP32 DevKit）
#endif

// 经典 ESP32 上 6~11 是 flash 引脚，绝不能当 GPIO 用
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
static const bool kLedUsable = !(kLedPin >= 6 && kLedPin <= 11);
#else
static const bool kLedUsable = true;
#endif

// ---- 简易 LED 驱动 ----
static bool s_led_enabled = false;
static uint32_t s_led_off_at_ms = 0;
static const uint32_t kLedPulseMs = 35;

static void ledBegin() {
  if (!kLedUsable) return;
  pinMode(kLedPin, OUTPUT);
  digitalWrite(kLedPin, HIGH);  // 熄灭（高电平灭）
}

static void ledSetEnabled(bool en) {
  if (!kLedUsable) return;
  s_led_enabled = en;
  if (!en) {
    s_led_off_at_ms = 0;
    digitalWrite(kLedPin, HIGH);  // 熄灭
  }
}

// 在 loop 里调用，处理脉冲超时熄灭
static void ledTick() {
  if (s_led_off_at_ms && (int32_t)(millis() - s_led_off_at_ms) >= 0) {
    s_led_off_at_ms = 0;
    if (s_led_enabled) digitalWrite(kLedPin, HIGH);  // 熄灭
  }
}

// 发一次短脉冲（有数据收发时调用）
static void ledPulse() {
  if (!s_led_enabled) return;
  digitalWrite(kLedPin, LOW);  // 点亮
  s_led_off_at_ms = millis() + kLedPulseMs;
}
// ---- LED 驱动结束 ----

static MinecraftServer server(MC_PORT);
static bool server_started = false;

// 掉电不清、复位也不清的 RTC 内存：记录“连续崩溃次数”。
// 连续崩 3 次就自动进安全模式（只开热点 + 网页，不启 MC 服务端），
// 保证至少能连上热点、能打开设置页，而不是无限重启。
RTC_NOINIT_ATTR static uint32_t g_rtc_magic;
RTC_NOINIT_ATTR static uint32_t g_rtc_fail;
static bool g_safe_mode = false;

static const char *resetReasonString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:   return "unknown";
    case ESP_RST_POWERON:   return "power_on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "other_wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unmapped";
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  esp_reset_reason_t reset_reason = esp_reset_reason();
  Serial.println("\nESP32MC starting...");
  // 模式描述跟着真实开关走，不再硬写一句「AP+STA (temp)」——
  // 改 wifi_config.cpp 的 kStaEnabled 后，这里会自动变成对应的说法。
  Serial.printf("[BUILD] 2026-11-08-k6 (%s, enchantment removed, potion item + /potion cmd fully removed, crafting table/furnace/recipe table implemented (34 craft + 11 smelt recipes, /recipes), view distance 4 chunks with PSRAM section cache, player save every 10s, respawn in place, partial blocks no longer overwrite ground, command tree covers every slash cmd, no drop timeout, drop item id 16-bit, world save fix (2s flush + readback verify, .bak swap, corrupt-log quarantine), keep quit position, cover_here (full blocks overwrite grass in place), AT+USE/AT+POS debug)\n",
                wifiCfgModeString());
  Serial.printf("Reset reason: %d (%s)\n", (int)reset_reason, resetReasonString(reset_reason));
  // 上一次运行崩在哪：RTC 内存里的面包屑 + 崩溃前写进 SPIFFS 的日志尾巴
  extern void mcBrDump();
  extern void mcBrSetBootRR(int rr);
  mcBrSetBootRR((int)reset_reason);   // 开机原因存 RTC，事后 #DIAG# 里能取（WDT/panic/brownout）
  mcBrDump();
  Serial.printf("Free heap on boot: %u bytes\n", ESP.getFreeHeap());

  if (g_rtc_magic != 0x4550324Du) {      // 'EP2M'：第一次上电或 RTC 被清
    g_rtc_magic = 0x4550324Du;
    g_rtc_fail = 0;
  }
  g_rtc_fail++;
  Serial.printf("[BOOT] consecutive crashes: %u\n", (unsigned)g_rtc_fail);
  if (g_rtc_fail >= 3) {
    g_safe_mode = true;
    Serial.println("[SAFEBOOT] 连续崩溃 3 次，已关闭 Minecraft 服务端，只保留热点与设置页");
  }

  ledBegin();

  // 有数据包收发时触发 LED 脉冲
  g_packet_activity_cb = []() { ledPulse(); };

  // 纯热点模式：设备不再连接路由器，自己就是热点。
  // 长按 BOOT 3 秒 = 清空热点密码（回到开放热点）并重启。
  wifiCfgInit(kWebCfgButtonPin, 3000);
  ledSetEnabled(true);

  if (!g_safe_mode) {
    server.begin();
    server_started = true;
    worldStorePrintInfo();          // 开机自检：种子 / 改动条数 / 日志字节数
    // 串口调试：把掉落物入口挂出来（AT+TESTDROP= / AT+DROPS?），
    // 不开客户端也能验证掉落物不会被超时删掉。
    mcDebugHookDrops(+[](uint16_t item, uint8_t count) { server.debugAddDrop(item, count); },
                     +[]() { server.debugPrintDrops(); });
    // 串口调试：模拟对某方块右键（AT+USE=x,y,z），验工作台 / 铁砧 / 容器 / 放置方块
    mcDebugHookUseBlock(+[](int16_t x, int16_t y, int16_t z) { server.debugUseBlock(x, y, z); });
  } else {
    Serial.println("[SAFEBOOT] mc server skipped (safe mode)");
  }

  Serial.printf("Hotspot: %s  (%s)\r\n", wifiCfgApSsid(),
                wifiCfgApIsOpen() ? "open, no password" : "password required");
  Serial.printf("Game:    %s:25565\r\n", wifiCfgApIpString());
  Serial.printf("NetMode: %s\r\n", wifiCfgModeString());
  if (wifiCfgStaEnabled()) {
    Serial.printf("StaNet:  \"%s\" -> %s\r\n", wifiCfgStaSsid(), wifiCfgStaIpString());
  } else {
    Serial.printf("StaNet:  disabled in this build (saved: \"%s\")\r\n", wifiCfgStaSsid());
  }
  Serial.printf("Mode:    %s\r\n", (GAMEMODE == 1) ? "creative" : "survival");
  Serial.printf("Config:  http://%s/\r\n", wifiCfgApIpString());
  Serial.printf("[BOOT] setup done, heap=%u psram=%u, entering loop\r\n", ESP.getFreeHeap(),
                (unsigned)ESP.getPsramSize());
}

void loop() {
  wifiCfgPoll();
  ledTick();

  if (server_started) {
    server.poll();
  }
  worldStoreTick();                        // 攒批写入 Flash（15s / 16 条）

  // 稳定运行 30 秒就把“连续崩溃次数”清零，避免历史上的崩溃影响以后开机
  static bool s_stability_cleared = false;
  if (!s_stability_cleared && millis() >= 30000UL) {
    s_stability_cleared = true;
    if (g_rtc_fail != 0) {
      g_rtc_fail = 0;
      Serial.println("[BOOT] stability ok (30s), crash counter cleared");
    }
  }

  // 心跳：每 5 秒一条，用来区分“卡死”和“只是没网”
  static unsigned long last_hb_ms = 0;
  if (millis() - last_hb_ms >= 5000) {
    last_hb_ms = millis();
    Serial.printf("[hb] up=%lus ap=%s sta=%s(%s) heap=%u srv=%d%s\r\n",
                  millis() / 1000UL, wifiCfgApIpString(),
                  wifiCfgStaConnected() ? "ON" : "off", wifiCfgStaIpString(),
                  ESP.getFreeHeap(), server_started ? 1 : 0,
                  g_safe_mode ? " SAFE" : "");
    // 存档位置核对用：把所有玩家槽的坐标打出来（没在线也会显示从 /pN.dat 读回来的值）
    {
      char line[80];
      int n = snprintf(line, sizeof(line), "[pos]");
      for (int i = 0; i < MAX_PLAYERS; i++) {
        if (player_data[i].client_fd < 0 && player_data[i].uuid[0] == 0) continue;
        n += snprintf(line + n, sizeof(line) - n, " p%d=%d,%u,%d",
                      i, (int)player_data[i].x, (unsigned)player_data[i].y, (int)player_data[i].z);
        if (n > (int)sizeof(line) - 20) break;
      }
      Serial.println(line);
    }
  }
  vTaskDelay(1);
}
