// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.

#include "serial_echo.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

namespace {
const size_t kCapSize = 1536;
char *g_cap = nullptr;          // 堆上分配：静态 1.5KB 会顶爆 dram0（曾经 overflow 2120B）
size_t g_cap_len = 0;
bool g_cap_on = false;

bool capEnsure() {
  if (g_cap) return true;
  g_cap = (char *)malloc(kCapSize);
  if (!g_cap) return false;
  g_cap[0] = '\0';
  return true;
}

void capAppend(const char *s, size_t n) {
  if (!g_cap_on || !g_cap || n == 0) return;
  if (g_cap_len + n >= kCapSize) {
    // 满了就把尾部截掉，而不是挤掉前面的内容（前面的命令回显更有用）
    if (g_cap_len >= kCapSize - 1) return;
    n = kCapSize - 1 - g_cap_len;
  }
  memcpy(g_cap + g_cap_len, s, n);
  g_cap_len += n;
  g_cap[g_cap_len] = '\0';
}

void capString(const char *s) { if (s) capAppend(s, strlen(s)); }
}  // namespace

void sePrintf(const char *fmt, ...) {
  char buf[224];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  size_t len = (size_t)n;
  if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
  Serial.write((const uint8_t *)buf, len);
  capAppend(buf, len);
}

void sePrintln(const String &s) {
  Serial.println(s);
  capString(s.c_str());
  capAppend("\n", 1);
}

void sePrintln(const char *s) {
  if (!s) return;
  Serial.println(s);
  capString(s);
  capAppend("\n", 1);
}

void sePrintln(const __FlashStringHelper *s) {
  Serial.println(s);
  capString((const char *)s);   // __FlashStringHelper 在 ESP32 上就是普通字符串指针
  capAppend("\n", 1);
}

void sePrint(const String &s) {
  Serial.print(s);
  capString(s.c_str());
}

void sePrint(const __FlashStringHelper *s) {
  Serial.print(s);
  capString((const char *)s);
}

void seCaptureBegin() {
  g_cap_len = 0;
  g_cap_on = capEnsure();
  if (g_cap) g_cap[0] = '\0';
}

const char *seCaptureEnd(size_t *len) {
  g_cap_on = false;
  if (len) *len = g_cap_len;
  return g_cap ? g_cap : "";
}
