// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.

#include "packet_codec.h"
#include <string.h>

void (*g_packet_activity_cb)() = nullptr;

#ifdef _WIN32
#include "win_platform.h"
#else
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <errno.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace {
const uint8_t SEGMENT_BITS = 0x7F;
const uint8_t CONTINUE_BIT = 0x80;
}  // namespace

// ---- 读字节追踪（容器包取证用）----
// 缓冲放 PSRAM：768 B 放 .bss 直接把 dram0_0_seg 挤爆了（overflowed by 568 bytes）
#define kPktTraceMax 768
static uint8_t* g_tr = nullptr;
static size_t g_tr_n = 0;
static bool g_tr_on = false;

void pktTraceStart() {
  if (g_tr == nullptr) {
#ifdef _WIN32
    g_tr = (uint8_t*)malloc(kPktTraceMax);
#else
    g_tr = (uint8_t*)heap_caps_malloc(kPktTraceMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g_tr == nullptr) g_tr = (uint8_t*)malloc(kPktTraceMax);
#endif
  }
  g_tr_n = 0;
  g_tr_on = (g_tr != nullptr);
}
void pktTraceStop() { g_tr_on = false; }
size_t pktTraceCopy(uint8_t* out, size_t cap) {
  if (g_tr == nullptr) return 0;
  size_t n = (g_tr_n < cap) ? g_tr_n : cap;
  if (n) memcpy(out, g_tr, n);
  return n;
}

PacketCodec::PacketCodec(int fd) : fd_(fd) {}

bool PacketCodec::readExact(uint8_t* buf, size_t len) {
  // 越界读一律立刻失败：以前解析错位时会真的去 recv 等 5 秒，
  // 一个假的超大循环次数就能把服务端钉死，客户端看到的就是“连接超时”。
  if (read_count_ + len > read_limit_) { read_overrun_ = true; return false; }
  size_t done = 0;
  uint32_t start = millis();
  while (done < len) {
    if (fd_ < 0) return false;
#ifdef _WIN32
    int n = recv((SOCKET)fd_, (char*)(buf + done), (int)(len - done), 0);
    if (n > 0) { done += (size_t)n; start = millis(); continue; }
    if (n == 0) return false;
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
      if (millis() - start > 5000) return false;
      Sleep(1);
      continue;
    }
#else
    int n = recv(fd_, buf + done, len - done, 0);
    if (n > 0) {
      done += (size_t)n;
      start = millis();
      if (g_packet_activity_cb) g_packet_activity_cb();
      continue;
    }
    if (n == 0) return false;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      if (millis() - start > 5000) return false;
      vTaskDelay(1);
      continue;
    }
#endif
    return false;
  }
  read_count_ += done;
  if (g_tr_on && g_tr != nullptr) {
    for (size_t i = 0; i < len && g_tr_n < kPktTraceMax; i++) g_tr[g_tr_n++] = buf[i];
  }
  return true;
}

bool PacketCodec::writeExact(const uint8_t* buf, size_t len) {
  write_timed_out_ = false;
  write_count_ += len;
  size_t done = 0;
  uint32_t start = millis();
  while (done < len) {
    if (fd_ < 0) return false;
    size_t chunk = len - done;
    if (chunk > 1460) chunk = 1460;
#ifdef _WIN32
    int n = send((SOCKET)fd_, (const char*)(buf + done), (int)chunk, 0);
    if (n > 0) { done += (size_t)n; start = millis(); continue; }
    if (n == 0) return false;
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
      if (millis() - start > write_timeout_ms_) { write_timed_out_ = true; return false; }
      Sleep(1);
      continue;
    }
#else
    int n = send(fd_, buf + done, chunk, MSG_NOSIGNAL);
    if (n > 0) {
      done += (size_t)n;
      start = millis();
      if (g_packet_activity_cb) g_packet_activity_cb();
      continue;
    }
    if (n == 0) return false;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENOBUFS) {
      if (millis() - start > write_timeout_ms_) { write_timed_out_ = true; return false; }
      vTaskDelay(2);  // 让 WiFi 栈把发送缓冲腾出来
      continue;
    }
#endif
#ifndef _WIN32
    Serial.printf("[SEND-FAIL] fd=%d errno=%d len=%u done=%u\n", fd_, errno, (unsigned)len, (unsigned)done);
#endif
    return false;
  }
  return true;
}

bool PacketCodec::readByte(uint8_t& value) { return readExact(&value, 1); }
bool PacketCodec::writeByte(uint8_t value) { return writeExact(&value, 1); }

bool PacketCodec::readUint16(uint16_t& value) {
  uint8_t buf[2];
  if (!readExact(buf, 2)) return false;
  value = ((uint16_t)buf[0] << 8) | buf[1];
  return true;
}
bool PacketCodec::writeUint16(uint16_t value) {
  uint8_t buf[2] = {(uint8_t)(value >> 8), (uint8_t)value};
  return writeExact(buf, 2);
}

bool PacketCodec::readUint32(uint32_t& value) {
  uint8_t buf[4];
  if (!readExact(buf, 4)) return false;
  value = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
  return true;
}
bool PacketCodec::writeUint32(uint32_t value) {
  uint8_t buf[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value};
  return writeExact(buf, 4);
}

bool PacketCodec::readUint64(uint64_t& value) {
  uint8_t buf[8];
  if (!readExact(buf, 8)) return false;
  value = ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48) | ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32) |
          ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16) | ((uint64_t)buf[6] << 8) | (uint64_t)buf[7];
  return true;
}
bool PacketCodec::writeUint64(uint64_t value) {
  uint8_t buf[8] = {
    (uint8_t)(value >> 56), (uint8_t)(value >> 48), (uint8_t)(value >> 40), (uint8_t)(value >> 32),
    (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value};
  return writeExact(buf, 8);
}

bool PacketCodec::readFloat(float& value) {
  uint32_t bits; if (!readUint32(bits)) return false;
  memcpy(&value, &bits, 4); return true;
}
bool PacketCodec::writeFloat(float value) {
  uint32_t bits; memcpy(&bits, &value, 4); return writeUint32(bits);
}
bool PacketCodec::readDouble(double& value) {
  uint64_t bits; if (!readUint64(bits)) return false;
  memcpy(&value, &bits, 8); return true;
}
bool PacketCodec::writeDouble(double value) {
  uint64_t bits; memcpy(&bits, &value, 8); return writeUint64(bits);
}

bool PacketCodec::readVarInt(int32_t& value) {
  value = 0; int position = 0;
  while (true) {
    uint8_t byte = 0;
    if (!readExact(&byte, 1)) return false;
    value |= (byte & SEGMENT_BITS) << position;
    if ((byte & CONTINUE_BIT) == 0) return true;
    position += 7;
    if (position >= 32) return false;
  }
}

bool PacketCodec::writeVarInt(uint32_t value) {
  uint8_t out[5]; size_t len = 0;
  do { uint8_t byte = value & SEGMENT_BITS; value >>= 7; if (value) byte |= CONTINUE_BIT; out[len++] = byte; } while (value);
  return writeExact(out, len);
}

int PacketCodec::sizeVarInt(uint32_t value) const {
  int size = 1; while ((value & ~SEGMENT_BITS) != 0) { value >>= 7; size++; } return size;
}

bool PacketCodec::readString(char* out, size_t out_len) {
  int32_t len = 0;
  if (!readVarInt(len) || len < 0 || out_len == 0) return false;
  size_t need = (size_t)len, copy_len = need;
  if (copy_len >= out_len) copy_len = out_len - 1;
  if (copy_len > 0 && !readExact((uint8_t*)out, copy_len)) return false;
  out[copy_len] = '\0';
  if (need > copy_len && !skipBytes(need - copy_len)) return false;
  return true;
}

bool PacketCodec::writeString(const char* str) {
  size_t len = strlen(str);
  return writeVarInt((uint32_t)len) && writeExact((const uint8_t*)str, len);
}

bool PacketCodec::skipString() {
  int32_t len = 0;
  if (!readVarInt(len) || len < 0) return false;
  return skipBytes((size_t)len);
}

bool PacketCodec::skipBytes(size_t len) {
  uint8_t tmp[64];
  while (len > 0) { size_t chunk = len > 64 ? 64 : len; if (!readExact(tmp, chunk)) return false; len -= chunk; }
  return true;
}
