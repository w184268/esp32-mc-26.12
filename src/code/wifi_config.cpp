// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.
//
// 纯热点（AP）模式。设备不再连接任何路由器：
//   1. 开机即以 "ESP32MC" 为 SSID 起一个热点（默认开放，可设置密码）
//   2. 热点 IP 固定 192.168.4.1，游戏服务端在上面监听 25565
//   3. 手机连上热点后，浏览器打开 http://192.168.4.1/ 就是密码设置页
//   4. 保存密码后自动重启生效；长按 BOOT 3 秒或串口命令可清空密码（恢复开放）

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <string.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "wifi_config.h"
#include "serial_echo.h"  // 串口回显：网页上的串口控制台要看到命令输出
void mcDiagDump();       // mc_server.cpp 里的环形诊断日志（#DIAG# 倒出）
void mcServerInvDump();  // mc_server.cpp：服务端权威背包（含名字/附魔），#INV# 倒出
#include "mc_server.h"   // 只用到指令树 id 的串口设置接口
#include "world_store.h"
#include "procedures.h"  // AT+SETBLOCK 测试口用 makeBlockChange
#include "terrain.h"     // AT+SETBLOCK 写后回读用 getBlockAt

namespace {

const char *kApSsid = "ESP32MC";
const size_t kPassMax = 63;            // WPA2 上限
const size_t kPassMin = 8;             // WPA2 下限（开放热点不用密码）
const unsigned long kApCheckMs = 10000;
const unsigned long kRebootDelayMs = 5000;

// ---- 状态 ----
char g_ap_pass[64] = {0};
bool g_ap_open = true;
char g_ap_ip[16] = "192.168.4.1";
uint8_t g_btn_pin = 0;
unsigned long g_hold_ms = 3000;
bool g_btn_down = false;
bool g_btn_armed = false;      // 开机后必须先看到一次 HIGH，才认按键，防止 GPIO0 常低导致死循环重启
bool g_btn_warned = false;
unsigned long g_btn_down_at = 0;
unsigned long g_reboot_at = 0;
unsigned long g_next_ap_check = 0;
bool g_url_printed = false;
bool g_prefs_ok = false;
Preferences g_prefs;

// ---- 可选 STA：让 ESP32 同时连回家里的路由器，电脑就能一边上网一边测服 ----
// 现在默认关闭：固件只做纯热点（AP），开机不尝试连接任何路由器。
// 想把 AP+STA 打开，把下面改成 true 重新编译即可（原有 NVS 里的
// wssid/wpass 不会被清掉，改了开关重启就能直接连回家里的 Wi-Fi）。
const bool kStaEnabled = true;    // 临时开启：AP + STA 同时在线（配过的家庭 Wi-Fi 会自动连）
char g_sta_ssid[33] = {0};
char g_sta_pass[64] = {0};
bool g_sta_on = false;
bool g_sta_connected = false;
char g_sta_ip[16] = "0.0.0.0";
unsigned long g_sta_next_check = 0;

// ---- 网页上下文（放堆上，不占 dram0.bss）----
struct WebCtx {
  WebServer web;
  WebCtx() : web(80) {}
};

WebCtx *g_wctx = nullptr;
bool g_web_running = false;

// 主循环不再每圈调用 WiFi.softAPIP()（那是库内部的 IP 比较路径，
// 之前每次 panic 的寄存器里都恰好是这个 IP，先把它从热循环里拿掉）。
static bool g_ap_ready = false;

bool wifiCfgApReady(void) { return g_ap_ready; }
bool g_web_routes_bound = false;
TaskHandle_t g_web_task = nullptr;
SemaphoreHandle_t g_web_mux = nullptr;
void webCfgTask(void *arg);

// ---- 网页串口控制台：网页只负责“排队 + 等结果”，真正执行放在 core 1 的主循环里，
//      避免在 core 0 的 HTTP 任务里直接改世界数组/玩家数据。
String g_console_out;                  // 最近一次命令的输出（网页显示用）
char g_webq_cmd[128] = {0};
volatile bool g_webq_req = false;       // core0 置位：有命令待执行
volatile bool g_webq_done = false;      // core1 置位：执行完
char *g_webq_out = nullptr;             // 输出拷贝（堆上分配，静态 1.6KB 会顶爆 dram0）
const size_t kWebqOutSize = 1600;


// ---------------------------------------------------------------- 极简 DNS
// 之前用的 DNSServer 库被怀疑是崩溃源，这里自己实现一个只回 A 记录的
// 极简 DNS：任何域名都回答 192.168.4.1，手机连上热点后打开任意网址
// 都会被引导到设置页（captive portal）。
static WiFiUDP g_dns;
static bool g_dns_on = false;
static uint8_t *g_dns_buf = nullptr;
static const size_t kDnsBufSize = 512;

static void dnsStartInternal() {
  if (!g_dns_buf) {
    g_dns_buf = (uint8_t *)malloc(kDnsBufSize);
    if (!g_dns_buf) { sePrintln("+AP:DNS_FAIL,OOM"); return; }
  }
  if (g_dns_on) return;
  if (!g_dns.begin(53)) { sePrintln("+AP:DNS_FAIL,BIND"); return; }
  g_dns_on = true;
  sePrintln("+AP:DNS_ON,port=53,*->192.168.4.1");
}

static void dnsStopInternal() {
  if (!g_dns_on) return;
  g_dns.stop();
  g_dns_on = false;
}

static void dnsProcessAll() {
  if (!g_dns_on || !g_dns_buf) return;
  int sz = g_dns.parsePacket();
  if (sz <= 0) return;
  if (sz > (int)kDnsBufSize) { g_dns.flush(); return; }
  int n = g_dns.read(g_dns_buf, sz);
  if (n < 12) return;
  if (((g_dns_buf[2] >> 3) & 0x0F) != 0) return;      // 只处理查询（QR=0）
  uint16_t qd = (uint16_t)((g_dns_buf[4] << 8) | g_dns_buf[5]);
  if (qd < 1) return;

  size_t p = 12;
  while (p < (size_t)n) {
    uint8_t l = g_dns_buf[p];
    if (l == 0) { p++; break; }
    if (l > 63) return;                               // 压缩指针，不处理
    p += (size_t)l + 1;
  }
  if (p + 4 > (size_t)n) return;
  uint16_t qtype = (uint16_t)((g_dns_buf[p] << 8) | g_dns_buf[p + 1]);
  size_t q_end = p + 4;
  if (qtype != 1 && qtype != 255) return;             // 只回 A / ANY

  if (q_end + 16 > kDnsBufSize) return;

  g_dns_buf[2] = 0x81;                                // QR=1, RD=1
  g_dns_buf[3] = 0x80;                                // RA=1
  g_dns_buf[6] = 0x00; g_dns_buf[7] = 0x01;           // ANCOUNT=1
  g_dns_buf[8] = 0x00; g_dns_buf[9] = 0x00;
  g_dns_buf[10] = 0x00; g_dns_buf[11] = 0x00;

  size_t o = q_end;
  g_dns_buf[o++] = 0xC0; g_dns_buf[o++] = 0x0C;       // 指向问题里的域名
  g_dns_buf[o++] = 0x00; g_dns_buf[o++] = 0x01;       // TYPE A
  g_dns_buf[o++] = 0x00; g_dns_buf[o++] = 0x01;       // CLASS IN
  g_dns_buf[o++] = 0x00; g_dns_buf[o++] = 0x00;       // TTL = 60
  g_dns_buf[o++] = 0x00; g_dns_buf[o++] = 0x3C;
  g_dns_buf[o++] = 0x00; g_dns_buf[o++] = 0x04;       // RDLENGTH = 4
  g_dns_buf[o++] = 192;  g_dns_buf[o++] = 168;
  g_dns_buf[o++] = 4;    g_dns_buf[o++] = 1;

  g_dns.beginPacket(g_dns.remoteIP(), g_dns.remotePort());
  g_dns.write(g_dns_buf, o);
  g_dns.endPacket();
}

// ---------------------------------------------------------------- 存储

void loadPasswordInternal() {
  g_ap_open = true;
  g_ap_pass[0] = '\0';
  g_prefs_ok = g_prefs.begin("esp32mc", false);
  if (!g_prefs_ok) return;
  String p = g_prefs.getString("appass", "");
  if (p.length() >= kPassMin && p.length() <= kPassMax) {
    strlcpy(g_ap_pass, p.c_str(), sizeof(g_ap_pass));
    g_ap_open = false;
  }
}

void loadStaInternal() {
  g_sta_on = false;
  g_sta_ssid[0] = 0;
  g_sta_pass[0] = 0;
  strlcpy(g_sta_ip, "0.0.0.0", sizeof(g_sta_ip));
  // 纯热点模式下不连路由器，但配置照样读出来：串口 AT+STA? 能看见存的是哪个网络，
  // 以后把 kStaEnabled 打开就能直接用，不用重新配。
  if (!g_prefs_ok) return;
  String s = g_prefs.getString("wssid", "");
  String p = g_prefs.getString("wpass", "");
  if (s.length() > 0 && s.length() <= 32) {
    strlcpy(g_sta_ssid, s.c_str(), sizeof(g_sta_ssid));
    strlcpy(g_sta_pass, p.c_str(), sizeof(g_sta_pass));
    g_sta_on = kStaEnabled;      // 只有开关打开才真的去连
  }
}

void startStaInternal(bool loud) {
  if (!kStaEnabled) return;   // 纯热点模式
  if (!g_sta_on) return;
  WiFi.begin(g_sta_ssid, g_sta_pass);
  if (loud) sePrintf("+STA:CONNECTING,\"%s\"\r\n", g_sta_ssid);
}

bool storePasswordInternal(const char *pass) {
  if (!g_prefs_ok) g_prefs_ok = g_prefs.begin("esp32mc", false);
  if (!g_prefs_ok) return false;
  if (pass == nullptr || pass[0] == '\0') {
    return g_prefs.remove("appass");
  }
  return g_prefs.putString("appass", String(pass));
}

// ---------------------------------------------------------------- 热点

bool startApInternal(bool loud) {
  WiFiMode_t ap_mode = (kStaEnabled && g_sta_on) ? WIFI_AP_STA : WIFI_AP;
  WiFi.mode(ap_mode);
  delay(50);
  const char *pass = g_ap_open ? nullptr : g_ap_pass;
  bool ok = WiFi.softAP(kApSsid, pass, 6, false, 4);

  if (!ok) {
    if (loud) sePrintln("+AP:RETRY");
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(ap_mode);
    delay(200);
    ok = WiFi.softAP(kApSsid, pass, 6, false, 4);
  }
  if (!ok) {
    sePrintln("+AP:FAIL");
    return false;
  }

  // 热点起来后 DHCP 生效需要一点时间，等 IP 真正非 0 再往后续走，
  // 否则 DNSServer 会拿到 0.0.0.0，后续接包容易在空指针上崩。
  unsigned long wait_start = millis();
  while (((uint32_t)WiFi.softAPIP()) == 0 && (millis() - wait_start) < 1500) {
    delay(50);
    yield();
  }
  if (((uint32_t)WiFi.softAPIP()) == 0) {
    sePrintf("+AP:NO_IP,waited=%lums\r\n", (unsigned long)(millis() - wait_start));
  }
  IPAddress ip = WiFi.softAPIP();
  strlcpy(g_ap_ip, ip.toString().c_str(), sizeof(g_ap_ip));
  g_ap_ready = (((uint32_t)ip) != 0);
  sePrintf("+AP:ON,SSID=\"%s\",IP=\"%s\",PASS=%s,CH=6\r\n",
                kApSsid, g_ap_ip, g_ap_open ? "open" : "on");
  return true;
}

// ---------------------------------------------------------------- 网页

String escapeHtml(const String &src) {
  String out;
  out.reserve(src.length() + 8);
  for (size_t i = 0; i < src.length(); i++) {
    char c = src.charAt(i);
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

String buildPage(const char *notice, bool is_error) {
  String h;
  h.reserve(1900);
  h += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>ESP32MC 热点设置</title><style>"
         "body{margin:0;font-family:-apple-system,Segoe UI,Roboto,sans-serif;"
         "background:#12151b;color:#e8ecf1}"
         ".wrap{max-width:520px;margin:0 auto;padding:24px 20px}"
         "h1{font-size:19px;margin:0 0 4px}"
         ".sub{color:#8f96a3;font-size:13px;margin-bottom:18px}"
         ".box{background:#1a1f27;border-radius:12px;padding:16px;margin-bottom:16px}"
         ".row{display:flex;justify-content:space-between;font-size:14px;"
         "padding:6px 0;border-bottom:1px solid #232a33}"
         ".row:last-child{border-bottom:none}"
         ".k{color:#8f96a3}.v{color:#e8ecf1;font-weight:600}"
         "label{display:block;font-size:13px;color:#8f96a3;margin:0 0 6px}"
         "input{width:100%;box-sizing:border-box;padding:12px;border-radius:9px;"
         "border:1px solid #2c343f;background:#0e1116;color:#e8ecf1;font-size:15px}"
         "button{width:100%;padding:12px;border-radius:9px;border:none;"
         "font-size:15px;font-weight:600;cursor:pointer}"
         ".p{background:#2f7d4f;color:#fff}"
         ".s{background:#242b34;color:#b9c2cf}"
         ".msg{padding:12px;border-radius:9px;font-size:14px;margin-bottom:16px}"
         ".out{white-space:pre-wrap;word-break:break-all;background:#0e1116;"
         "border:1px solid #2c343f;border-radius:9px;padding:10px;margin:0;"
         "font-family:ui-monospace,Consolas,monospace;font-size:12px;color:#c6ffd8;"
         "max-height:280px;overflow:auto}"
         "</style></head><body>");
  if (notice && notice[0]) {
    h += F("<div class='msg' style='background:");
    h += is_error ? F("#3a1f22;color:#ff9a9a'") : F("#1f3a29;color:#8fe0aa'");
    h += F(">");
    h += escapeHtml(String(notice));
    h += F("</div>");
  }
  h += F("<div class='wrap'><h1>ESP32MC 热点</h1>"
         "<div class='sub'>连上这个热点后，用浏览器访问本页设置密码。</div>");

  h += F("<div class='box'><div class='row'><span class='k'>热点名称</span><span class='v'>");
  h += escapeHtml(String(kApSsid));
  h += F("</span></div><div class='row'><span class='k'>本机地址</span><span class='v'>");
  h += escapeHtml(String(g_ap_ip));
  h += F("</span></div><div class='row'><span class='k'>当前状态</span><span class='v'>");
  h += g_ap_open ? F("开放（无需密码）") : F("已设密码");
  h += F("</span></div><div class='row'><span class='k'>已连接设备</span><span class='v'>");
  h += String((int)WiFi.softAPgetStationNum());
  h += F(" 台</span></div><div class='row'><span class='k'>游戏地址</span><span class='v'>");
  h += escapeHtml(String(g_ap_ip));
  h += F(":25565</span></div></div>");

  h += F("<form method='POST' action='/save' class='box'>"
         "<label>设置热点密码（8~63 位；留空则不做修改）</label>"
         "<input name='pass' maxlength='63' autocomplete='off' "
         "placeholder='例如 12345678abc'>"
         "<button class='p' type='submit'>保存并重启</button></form>");

  h += F("<form method='POST' action='/open' style='margin-bottom:16px'>"
         "<button class='s' type='submit'>关闭密码（恢复开放热点）</button></form>");

  // ---- 串口控制台：输入框 + 发送按钮，命令直接喂给串口解析器（AP 热点里就能用）----
  h += F("<div class='box'><label>串口控制台（命令等同于在 USB 串口里敲回车）</label>"
         "<form method='POST' action='/serial'>"
         "<input name='cmd' maxlength='95' autocomplete='off' autocapitalize='off' "
         "spellcheck='false' placeholder='#SAVE#  AT+APINFO?  #WORLD_INFO#  AT+DROPS?'>"
         "<button class='p' type='submit' style='margin-top:10px'>发送</button></form>");
  h += F("<div style='color:#8f96a3;font-size:12px;margin:10px 0 6px'>最近一次输出</div>"
         "<pre class='out'>");
  h += escapeHtml(g_console_out.length() ? g_console_out : String("(还没有发送过命令)"));
  h += F("</pre></div>");

  h += F("<div class='box' style='color:#8f96a3;font-size:13px;line-height:1.7'>"
         "· 保存后设备会在 5 秒后自动重启，热点按新密码重新出现<br>"
         "· 改密码会断开当前连接，需要重新连接热点<br>"
         "· 长按 BOOT 3 秒 = 清空密码（恢复开放）<br>"
         "· 串口命令 AT+APOPEN 同样能清空密码<br>"
         "· 控制台常用：#SAVE# 立刻存盘、#WORLD_INFO# 看存档、AT+DROPS? 看地上掉落物"
         "</div></div></body></html>");
  return h;
}

void scheduleReboot(const char *why) {
  g_reboot_at = millis() + kRebootDelayMs;
  sePrintf("+AP:REBOOT_IN,%lums,reason=%s\r\n", kRebootDelayMs, why);
}

void handleRoot() {
  sePrintf("+AP:GET %s,heap=%u\r\n",
                g_wctx->web.uri().c_str(), ESP.getFreeHeap());
  g_wctx->web.send(200, "text/html; charset=utf-8", buildPage(nullptr, false));
}

void handleSave() {
  String pass = g_wctx->web.arg("pass");
  sePrintf("+AP:POST /save,heap=%u\r\n", ESP.getFreeHeap());

  if (pass.length() == 0) {
    g_wctx->web.send(200, "text/html; charset=utf-8",
                     buildPage("密码留空，未做修改。要关闭密码请点下面的按钮。", false));
    return;
  }
  if (pass.length() < kPassMin || pass.length() > kPassMax) {
    String n = "密码长度必须是 8~63 位（当前 ";
    n += String((int)pass.length());
    n += " 位）。";
    g_wctx->web.send(400, "text/html; charset=utf-8", buildPage(n.c_str(), true));
    return;
  }
  if (!storePasswordInternal(pass.c_str())) {
    g_wctx->web.send(500, "text/html; charset=utf-8",
                     buildPage("写入 Flash 失败，密码未改动。", true));
    return;
  }
  strlcpy(g_ap_pass, pass.c_str(), sizeof(g_ap_pass));
  g_ap_open = false;
  g_wctx->web.send(200, "text/html; charset=utf-8",
                   buildPage("密码已保存，5 秒后重启。重启后请用新密码重连热点。", false));
  scheduleReboot("password_set");
}

void handleOpen() {
  storePasswordInternal("");
  g_ap_open = true;
  g_ap_pass[0] = '\0';
  g_wctx->web.send(200, "text/html; charset=utf-8",
                   buildPage("密码已清除，5 秒后重启为开放热点。", false));
  scheduleReboot("password_cleared");
}

void handleSerialCmd() {
  String cmd = g_wctx->web.arg("cmd");
  cmd.trim();
  sePrintf("+WEB:POST /serial,len=%u\r\n", (unsigned)cmd.length());
  if (cmd.length() == 0) {
    g_wctx->web.send(200, "text/html; charset=utf-8",
                     buildPage("命令为空，没有发送。", true));
    return;
  }
  if (cmd.length() > sizeof(g_webq_cmd) - 1) cmd = cmd.substring(0, sizeof(g_webq_cmd) - 1);

  // 交给 core 1 执行，这里最多等 2 秒拿输出
  strlcpy(g_webq_cmd, cmd.c_str(), sizeof(g_webq_cmd));
  g_webq_done = false;
  g_webq_req = true;
  unsigned long t0 = millis();
  while (!g_webq_done && (millis() - t0) < 2000) vTaskDelay(pdMS_TO_TICKS(5));

  String notice = "已发送：";
  notice += cmd;
  if (g_webq_done && g_webq_out) {
    g_webq_out[kWebqOutSize - 1] = '\0';
    g_console_out = String(g_webq_out);
  } else if (g_webq_done) {
    g_console_out = String("+WEB:NO_BUFFER,内存不足没抓到输出");
  } else {
    g_webq_req = false;              // 主循环卡住：撤回请求，别让它稍后突然执行
    g_console_out = String("+WEB:TIMEOUT,主循环 2 秒内没响应，命令已撤回");
    g_wctx->web.send(200, "text/html; charset=utf-8",
                     buildPage("主循环忙，命令已撤回，稍后再试。", true));
    return;
  }
  g_wctx->web.send(200, "text/html; charset=utf-8", buildPage(notice.c_str(), false));
}

void bindWebRoutes() {
  if (g_web_routes_bound || !g_wctx) return;
  g_wctx->web.on("/", HTTP_GET, handleRoot);
  g_wctx->web.on("/save", HTTP_POST, handleSave);
  g_wctx->web.on("/open", HTTP_POST, handleOpen);
  g_wctx->web.on("/serial", HTTP_POST, handleSerialCmd);
  g_wctx->web.onNotFound([]() {
    sePrintf("+AP:302 %s\r\n", g_wctx->web.uri().c_str());
    g_wctx->web.sendHeader("Location", "/", true);
    g_wctx->web.send(302, "text/plain", "");
  });
  g_web_routes_bound = true;
}

// 网页跑在 core 0 独立任务：慢速手机上传不会拖住 core 1 的游戏主循环
void webCfgTask(void *) {
  for (;;) {
    bool locked = false;
    if (g_web_mux) locked = (xSemaphoreTake(g_web_mux, pdMS_TO_TICKS(20)) == pdTRUE);
    if (locked) {
      if (g_wctx && g_web_running) {
        g_wctx->web.handleClient();
      }
      xSemaphoreGive(g_web_mux);
    }
    dnsProcessAll();          // 不占游戏主循环，放在 core 0 的任务里跑
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void startWebInternal() {
  if (!g_ap_ready) {
    sePrintln("+AP:WEB_DEFER,NO_IP");   // 主循环里会重试
    return;
  }
  if (!g_wctx) g_wctx = new WebCtx();
  if (!g_wctx) {
    sePrintln("+AP:FAIL,NO_RAM");
    return;
  }
  bindWebRoutes();
  if (!g_web_running) {
    g_wctx->web.begin();
    dnsStartInternal();           // 自带极简 DNS：任意域名 -> 192.168.4.1
    g_web_running = true;
    if (!g_web_mux) g_web_mux = xSemaphoreCreateMutex();
    if (!g_web_task) {
      xTaskCreatePinnedToCore(webCfgTask, "apcfg", 6144, nullptr, 1, &g_web_task, 0);
    }
    sePrintf("+AP:WEB_ON,http://%s/\r\n", g_ap_ip);
  }
}

void restartWebInternal() {
  if (!g_wctx || !g_ap_ready) return;
  if (g_web_mux && xSemaphoreTake(g_web_mux, pdMS_TO_TICKS(500)) != pdTRUE) return;
  g_wctx->web.stop();
  dnsStopInternal();
  delay(50);
  g_wctx->web.begin();
  dnsStartInternal();
  if (g_web_mux) xSemaphoreGive(g_web_mux);
  sePrintln("+AP:WEB_RESTART");
}

// ---------------------------------------------------------------- 按键

void processButton() {
  int level = digitalRead(g_btn_pin);
  unsigned long now = millis();

  if (level == HIGH) {
    g_btn_armed = true;
    if (g_btn_down) {
      unsigned long held = now - g_btn_down_at;
      g_btn_down = false;
      sePrintf("+BTN:UP,held=%lums\r\n", held);
    }
    return;
  }

  // 开机时 GPIO0 就是低（按键卡住/板子拉低）：不认，否则每次开机 3 秒后自动重启
  if (!g_btn_armed) {
    if (!g_btn_warned) {
      g_btn_warned = true;
      sePrintln("+BTN:LOW_AT_BOOT,IGNORED,release_to_enable");
    }
    return;
  }

  if (!g_btn_down) {
    g_btn_down = true;
    g_btn_down_at = now;
    sePrintln("+BTN:DOWN");
  }
}

void processButtonHold() {
  if (!g_btn_down) return;
  if ((millis() - g_btn_down_at) < g_hold_ms) return;

  g_btn_down = false;                        // 只触发一次
  sePrintf("+BTN:HOLD=%lums,RESET_AP_PASSWORD\r\n", g_hold_ms);
  wifiCfgResetPasswordToOpen();
}

// ---------------------------------------------------------------- 串口

String g_line_buffer;
const size_t kMaxLineLen = 96;

void printOk() { sePrintln("+OK"); }

void commandHelp() {
  sePrintln("+HELP:AT+APINFO?              查看热点状态");
  sePrintln("+HELP:AT+APSET=\"<password>\"   设置热点密码(8~63位)，重启生效");
  sePrintln("+HELP:AT+APOPEN               清空密码(恢复开放)，重启生效");
  sePrintln("+HELP:#WEBCFG#                打印设置页地址");
  sePrintln("+HELP:#WORLD_INFO# / #WORLD_RESET# / #DIAG#(容器日志)");
  sePrintln("+HELP:#SAVE#  (或 AT+SAVE)    立刻存盘：玩家背包坐标 + 方块改动一起写进 Flash");
  sePrintln("+HELP:AT+SETBLOCK=x,y,z,block  改一块方块并立刻落盘(block: 0空气 48泥土 25石头 230附魔台 250铁砧)");
  sePrintln("+HELP:AT+USE=x,y,z            等价于玩家对着该方块右键(验工作台/箱子/熔炉)");
  sePrintln("+HELP:AT+TESTDROP=item,count  在出生点塞一个掉落物");
  sePrintln("+HELP:AT+DROPS?               打印地上掉落物");
  printOk();
}

void commandApInfo() {
  sePrintf("+AP:INFO,SSID=\"%s\",IP=\"%s\",PASS=%s,STA=%u\r\n",
                kApSsid, g_ap_ip, g_ap_open ? "open" : "on",
                (unsigned)WiFi.softAPgetStationNum());
  printOk();
}

void commandApSet(const String &arg) {
  String p = arg;
  p.trim();
  if (p.length() > 0 && p.charAt(0) == '"') {
    int end = p.lastIndexOf('"');
    if (end > 0) p = p.substring(1, end);
  }
  if (p.length() < kPassMin || p.length() > kPassMax) {
    sePrintln("+AP:ERROR,PASS_LEN,8_63_REQUIRED");
    return;
  }
  if (!storePasswordInternal(p.c_str())) {
    sePrintln("+AP:ERROR,NVS_WRITE");
    return;
  }
  strlcpy(g_ap_pass, p.c_str(), sizeof(g_ap_pass));
  g_ap_open = false;
  sePrintln("+AP:PASS_SAVED,REBOOTING");
  scheduleReboot("serial_password_set");
}

void processLine(const String &line) {
  if (line.length() == 0) return;

  if (line == "AT" || line == "AT+HELP") { commandHelp(); return; }
  if (line == "AT+APINFO?" || line == "#APINFO#") { commandApInfo(); return; }
  if (line.startsWith("AT+CID=")) {
    // 强制指定 declare commands 包 id，例如 AT+CID=0x11（AT+CID=0 关闭发送）
    uint32_t v = (uint32_t)strtoul(line.substring(7).c_str(), nullptr, 0);
    mcCommandTreeSetPacketId((uint8_t)v);
    return;
  }
  if (line == "AT+CID?") {
    mcCommandTreePrintState();
    return;
  }
  if (line.startsWith("AT+EDATA=")) {
    // 热改 Set Entity Data 包 id，例如 AT+EDATA=0x63（26.1.2 实测值）
    uint32_t v = (uint32_t)strtoul(line.substring(9).c_str(), nullptr, 0);
    mcEntityDataSetId((uint8_t)v);
    sePrintf("+EDATA:SET,0x%02X\r\n", mcEntityDataGetId());
    return;
  }
  if (line.startsWith("AT+JOIN=")) {
    uint32_t v = (uint32_t)strtoul(line.substring(8).c_str(), nullptr, 0);
    mcJoinSetMask((uint8_t)v);
    return;
  }
  if (line == "AT+JOIN?") {
    sePrintln("[JOIN] mask query: use AT+JOIN=0x.. to set");
    return;
  }
  if (line == "AT+EDATA?") {
    mcEntityDataPrintState();
    return;
  }
  if (line == "AT+DROPS?") {
    mcDebugPrintDrops();
    return;
  }
  if (line.startsWith("AT+USE=")) {
    // AT+USE=<x>,<y>,<z> —— 等价于玩家对着这个方块右键，验附魔台 / 铁砧 / 工作台 / 箱子
    String a = line.substring(7);
    a.trim();
    int c1 = a.indexOf(',');
    int c2 = (c1 >= 0) ? a.indexOf(',', c1 + 1) : -1;
    if (c1 < 0 || c2 < 0) { sePrintln("+USE:ERROR,FORMAT x,y,z"); return; }
    int16_t ux = (int16_t)atoi(a.substring(0, c1).c_str());
    int16_t uy = (int16_t)atoi(a.substring(c1 + 1, c2).c_str());
    int16_t uz = (int16_t)atoi(a.substring(c2 + 1).c_str());
    mcDebugUseBlock(ux, uy, uz);
    return;
  }
  if (line.startsWith("AT+TESTDROP=")) {    // AT+TESTDROP=<item>,<count> —— 在 (5,66,5) 空地上塞一个掉落物，只为调试
    String a = line.substring(12);
    a.trim();
    int comma = a.indexOf(',');
    uint32_t item = (uint32_t)strtoul(comma >= 0 ? a.substring(0, comma).c_str() : a.c_str(), nullptr, 0);
    uint32_t n = (comma >= 0) ? (uint32_t)strtoul(a.substring(comma + 1).c_str(), nullptr, 0) : 1;
    if (item == 0 || item > 65535 || n == 0 || n > 255) { sePrintln("+DROP:ERROR,ARG"); return; }
    mcDebugAddDrop((uint16_t)item, (uint8_t)n);
    sePrintln("+DROP:OK");
    return;
  }
  if (line.startsWith("AT+STA=")) {
    // AT+STA="ssid","password" —— 保存后重启，AP 与 STA 同时在线
    if (!kStaEnabled) { sePrintln("+STA:DISABLED,THIS_BUILD_IS_AP_ONLY"); return; }
    String a = line.substring(7);
    a.trim();
    int q1 = a.indexOf('"');
    int q2 = (q1 >= 0) ? a.indexOf('"', q1 + 1) : -1;
    if (q1 < 0 || q2 < 0) { sePrintln("+STA:ERROR,FORMAT"); return; }
    String ssid = a.substring(q1 + 1, q2);
    int q3 = a.indexOf('"', q2 + 1);
    int q4 = (q3 >= 0) ? a.indexOf('"', q3 + 1) : -1;
    String pw = (q3 >= 0 && q4 > q3) ? a.substring(q3 + 1, q4) : String("");
    if (ssid.length() == 0) { sePrintln("+STA:ERROR,SSID_EMPTY"); return; }
    if (!g_prefs_ok) g_prefs_ok = g_prefs.begin("esp32mc", false);
    if (!g_prefs_ok) { sePrintln("+STA:ERROR,NVS"); return; }
    g_prefs.putString("wssid", ssid);
    g_prefs.putString("wpass", pw);
    strlcpy(g_sta_ssid, ssid.c_str(), sizeof(g_sta_ssid));
    strlcpy(g_sta_pass, pw.c_str(), sizeof(g_sta_pass));
    g_sta_on = true;
    sePrintf("+STA:SAVED,\"%s\",REBOOTING\r\n", g_sta_ssid);
    scheduleReboot("serial_sta_set");
    return;
  }
  if (line == "AT+STA?") {
    // 串口只能读 ASCII，中文 SSID 会显示成 ???，所以额外给一份十六进制，
    // 用 UTF-8 解一下就知道存的是哪个路由器。
    char hex[3 * 33];
    hex[0] = '\0';
    int hn = 0;
    for (int i = 0; g_sta_ssid[i] != '\0' && hn < 96; ++i) {
      hn += snprintf(hex + hn, sizeof(hex) - hn, "%02X", (unsigned char)g_sta_ssid[i]);
    }
    sePrintf("+STA:INFO,ENABLED=%d,ON=%d,LINK=%d,SSID=\"%s\",SSID_HEX=%s,IP=\"%s\",RSSI=%d\r\n",
                  kStaEnabled ? 1 : 0, g_sta_on ? 1 : 0, (WiFi.status() == WL_CONNECTED) ? 1 : 0,
                  g_sta_ssid, hex, g_sta_ip, (int)WiFi.RSSI());
    return;
  }
  if (line == "AT+STACLR") {
    if (!g_prefs_ok) g_prefs_ok = g_prefs.begin("esp32mc", false);
    if (g_prefs_ok) { g_prefs.remove("wssid"); g_prefs.remove("wpass"); }
    g_sta_on = false;
    sePrintln("+STA:CLEARED,REBOOTING");
    scheduleReboot("serial_sta_clear");
    return;
  }
  if (line == "AT+APOPEN" || line == "#APOPEN#") {
    wifiCfgResetPasswordToOpen();
    return;
  }
  if (line.startsWith("AT+APSET=")) {
    commandApSet(line.substring(9));
    return;
  }
  if (line == "#WEBCFG#") {
    sePrintf("+AP:WEB_URL,http://%s/\r\n", g_ap_ip);
    printOk();
    return;
  }
  if (line == "#SAVE#" || line == "AT+SAVE") {
    // 主动存盘：不等 10 秒定时器 / 2 秒落盘批，立刻把玩家档和方块改动写进 Flash。
    // 拔电前想保住当前位置和背包，就发这条。
    worldStoreSavePlayers();
    worldStoreFlush();
    sePrintln("+SAVE:OK,PLAYERS_AND_CHANGES_FLUSHED");
    worldStorePrintInfo();
    printOk();
    return;
  }
  // 容器/附魔排查日志倒出：客户端复现一次之后发这条，把服务端收到的包和回的格全打出来
  if (line == "#DIAG#") {
    mcDiagDump();
    printOk();
    return;
  }
  if (line == "#INV#" || line == "#inv#") {
    // 服务端权威背包：物品/数量/附魔等级/自定义名 + 合成格 + 鼠标
    mcServerInvDump();
    printOk();
    return;
  }
  if (line == "#WORLD_RESET#") {
    if (worldStoreReset()) {
      sePrintln("+WORLD:RESET_OK,REBOOTING");
      delay(200);
      ESP.restart();
    } else {
      sePrintln("+WORLD:RESET_FAIL,NO_SPIFFS");
    }
    return;
  }
  if (line == "#WORLD_INFO#") {
    sePrintf("+WORLD:READY=%d\r\n", worldStoreReady() ? 1 : 0);
    worldStorePrintInfo();
    printOk();
    return;
  }
  // 串口测试口：不改客户端也能改一块方块，用来验证“改动是否真的落到 flash”。
  // 用法：AT+SETBLOCK=<x>,<y>,<z>,<block>   （block=0 是空气，48 是泥土，25 是石头）
  if (line.startsWith("AT+SETBLOCK=")) {
    // 手动切分，不用 sscanf（在 ESP32 上会拖进 10 KB 以上的 scanf 机器）
    String arg = line.substring(12);
    int c1 = arg.indexOf(',');
    int c2 = (c1 >= 0) ? arg.indexOf(',', c1 + 1) : -1;
    int c3 = (c2 >= 0) ? arg.indexOf(',', c2 + 1) : -1;
    if (c3 < 0) {
      sePrintln("+SETBLOCK:ERROR,USE x,y,z,block(0..255)");
      return;
    }
    int sx = arg.substring(0, c1).toInt();
    int sy = arg.substring(c1 + 1, c2).toInt();
    int sz = arg.substring(c2 + 1, c3).toInt();
    int sb = arg.substring(c3 + 1).toInt();
    if (sy < 0 || sy > 255 || sb < 0 || sb > 255) {
      sePrintln("+SETBLOCK:ERROR,USE x,y,z,block(0..255)");
      return;
    }
    if (!worldStoreReady()) {
      sePrintln("+SETBLOCK:ERROR,NO_SPIFFS");
      return;
    }
    if (makeBlockChange((int16_t)sx, (uint8_t)sy, (int16_t)sz, (uint8_t)sb) != 0) {
      sePrintln("+SETBLOCK:ERROR,CHANGE_TABLE_FULL");
      return;
    }
    // 写后立刻回读：能区分「写进去了」和「表里没有这条」，
    // 之前 AT+USE 读到 block=0 就是因为这里没做回读、看不出改动到底进没进表。
    sePrintf("[SETBLOCK] write getBlockAt(%d,%d,%d)=%u\r\n", sx, sy, sz,
                  (unsigned)getBlockAt(sx, sy, sz));
    worldStoreFlush();                 // 立刻落盘，不等 2 秒的批
    worldStorePrintInfo();
    sePrintf("[SETBLOCK] after flush getBlockAt=%u\r\n", (unsigned)getBlockAt(sx, sy, sz));
    sePrintf("+SETBLOCK:OK,%d,%d,%d,%d,changes=%u\r\n",
                  sx, sy, sz, sb, (unsigned)worldStoreChangeCount());
    return;
  }
  sePrintln("+ERR:UNKNOWN_CMD,use AT+HELP");
}

// 网页控制台提交的命令在这里（core 1 主循环）执行，输出抄进 g_webq_out 交给网页
void processWebCommand() {
  if (!g_webq_req) return;
  String cmd(g_webq_cmd);
  cmd.trim();
  seCaptureBegin();
  if (cmd.length() == 0) {
    sePrintln("+ERR:EMPTY_CMD,输入一条命令，例如 #SAVE#");
  } else {
    processLine(cmd);
  }
  size_t len = 0;
  const char *out = seCaptureEnd(&len);
  if (!g_webq_out) {
    // 优先放 PSRAM（这块板有 8MB），失败退回内部堆
    g_webq_out = (char *)heap_caps_malloc(kWebqOutSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_webq_out) g_webq_out = (char *)malloc(kWebqOutSize);
  }
  if (g_webq_out) {
    size_t n = (len < kWebqOutSize - 1) ? len : kWebqOutSize - 1;
    if (n > 0) memcpy(g_webq_out, out, n);
    g_webq_out[n] = '\0';
  }
  g_webq_req = false;
  g_webq_done = true;
}

void processSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = g_line_buffer;
      g_line_buffer = "";
      line.trim();
      processLine(line);
      continue;
    }
    if (g_line_buffer.length() < kMaxLineLen) g_line_buffer += c;
  }
}

// ---------------------------------------------------------------- 热点看护

void processStaWatchdog() {
  if (!g_sta_on) return;
  unsigned long now = millis();
  if ((long)(now - g_sta_next_check) < 0) return;
  g_sta_next_check = now + 5000;

  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    if (!g_sta_connected || ip != String(g_sta_ip)) {
      strlcpy(g_sta_ip, ip.c_str(), sizeof(g_sta_ip));
      g_sta_connected = true;
      sePrintf("+STA:OK,IP=\"%s\",RSSI=%d\r\n", g_sta_ip, (int)WiFi.RSSI());
    }
  } else if (g_sta_connected) {
    g_sta_connected = false;
    sePrintln("+STA:LOST,RETRY");
    WiFi.begin(g_sta_ssid, g_sta_pass);
  }
}

void processApWatchdog() {
  unsigned long now = millis();
  if ((long)(now - g_next_ap_check) < 0) return;
  g_next_ap_check = now + kApCheckMs;

  g_ap_ready = ((uint32_t)WiFi.softAPIP()) != 0;
  if (!g_ap_ready) {
    sePrintln("+AP:LOST,RESTARTING");
    startApInternal(true);
    restartWebInternal();
    g_url_printed = false;
  }
}

}  // namespace

// ---------------------------------------------------------------- 对外接口

void wifiCfgInit(uint8_t io0_pin, unsigned long hold_ms) {
  g_btn_pin = io0_pin;
  g_hold_ms = hold_ms;
  g_line_buffer = "";

  if (g_hold_ms < 1000) g_hold_ms = 3000;
  pinMode(g_btn_pin, INPUT_PULLUP);
  loadPasswordInternal();
  loadStaInternal();

  WiFi.persistent(false);
  WiFi.setSleep(false);
  if (!startApInternal(true)) {
    sePrintln("+AP:FAIL,HOTSPOT_NOT_STARTED");
  }
  startStaInternal(true);
  startWebInternal();
}

void wifiCfgPoll(void) {
  processButton();
  processButtonHold();
  processSerial();
  processWebCommand();          // 网页控制台排进来的命令（在 core 1 执行）
  processApWatchdog();
  processStaWatchdog();

  if (!g_web_running && g_ap_ready) {
    startWebInternal();                    // 开机时 IP 没就绪的补启动
  }

  if (!g_url_printed && g_ap_ready) {
    g_url_printed = true;
    sePrintf("+AP:READY,SSID=\"%s\",IP=\"%s\",GAME=%s:25565,WEB=http://%s/\r\n",
                  kApSsid, g_ap_ip, g_ap_ip, g_ap_ip);
  }

  if (g_reboot_at != 0 && (long)(millis() - g_reboot_at) >= 0) {
    g_reboot_at = 0;
    worldStoreFlush();                 // 重启前把还没落盘的方块改动写掉
    sePrintln("+AP:REBOOT_NOW");
    delay(200);
    ESP.restart();
  }
}

const char *wifiCfgApSsid(void) { return kApSsid; }
const char *wifiCfgApPassword(void) { return g_ap_pass; }
bool wifiCfgApIsOpen(void) { return g_ap_open; }
const char *wifiCfgApIpString(void) { return g_ap_ip; }
const char *wifiCfgStaIpString(void) { return g_sta_ip; }
bool wifiCfgStaConnected(void) { return g_sta_connected; }
bool wifiCfgStaEnabled(void) { return kStaEnabled; }
const char *wifiCfgStaSsid(void) { return g_sta_ssid; }

// 一句话描述当前网络模式，日志里随真实状态变：
//   AP only (hotspot)               纯热点，不连路由器
//   AP+STA (sta linking)            开了 STA，正在连/已连（连上时带 IP）
const char *wifiCfgModeString(void) {
  if (!kStaEnabled) return "AP only (hotspot)";
  return g_sta_connected ? "AP+STA (sta connected)" : "AP+STA (sta linking)";
}

void wifiCfgResetPasswordToOpen(void) {
  storePasswordInternal("");
  g_ap_open = true;
  g_ap_pass[0] = '\0';
  sePrintln("+AP:PASS_CLEARED,REBOOTING");
  scheduleReboot("boot_button");
}
