// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../../NOTICE.

#ifndef WIN_PLATFORM_H
#define WIN_PLATFORM_H

// Windows 平台兼容层 - 替代 Arduino.h 和 ESP32 特有 API

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

// ============ Arduino 兼容 ============

static inline uint32_t millis() {
  return (uint32_t)GetTickCount64();
}

static inline void delay(uint32_t ms) {
  Sleep(ms);
}

static inline void yield() {
  // Windows 下让出时间片
  SleepEx(0, TRUE);
}

// ============ ESP32 特有 API 替代 ============

static inline int64_t esp_timer_get_time_win() {
  LARGE_INTEGER freq, counter;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&counter);
  return (int64_t)(counter.QuadPart * 1000000LL / freq.QuadPart);
}

// ============ Serial 模拟 ============

class SerialClass {
public:
  void begin(int baud) { (void)baud; }

  void print(const char* s) { printf("%s", s); }
  void println(const char* s) { printf("%s\n", s); }
  void println() { printf("\n"); }

  template<typename T>
  void print(T val) { printf("%d", (int)val); }
  template<typename T>
  void println(T val) { printf("%d\n", (int)val); }

  void printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
  }
};

extern SerialClass Serial;

// ============ Winsock send/recv 兼容 ============
// lwip/sockets.h 中的 recv/send 在 Windows 上直接用 Winsock 的
// MSG_NOSIGNAL 在 Windows 上不存在
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// fcntl 替代: 用 ioctlsocket 设置非阻塞
static inline int win_set_nonblocking(SOCKET s) {
  u_long mode = 1;
  return ioctlsocket(s, FIONBIO, &mode);
}

// errno 兼容 (仅供参考, win_packet_codec.cpp 直接用 WSAGetLastError)
// 不重定义 EAGAIN/EWOULDBLOCK/EINTR, 避免与系统 errno.h 冲突

static inline int win_get_last_error() {
  return WSAGetLastError();
}

#endif // _WIN32
#endif // WIN_PLATFORM_H
