// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.
//
// 串口回显工具：所有命令应答都走 sePrintf / sePrintln，
// 这些函数照常往 USB 串口写，同时在“网页串口控制台”采集期间
// 把同样的文本抄进一块固定缓冲，让网页能把命令输出原样显示出来。
// 固定缓冲（不开 String、不动态分配），避免每次请求都动堆。

#ifndef SERIAL_ECHO_H
#define SERIAL_ECHO_H

#include <Arduino.h>

// 往串口写（有采集时同时进缓冲）。语义同 Serial.printf。
void sePrintf(const char *fmt, ...);
// 往串口写一行（有采集时同时进缓冲）。支持 F("...") 这种 flash 字符串。
void sePrintln(const String &s);
void sePrintln(const char *s);
void sePrintln(const __FlashStringHelper *s);
// 只写不换行
void sePrint(const String &s);
void sePrint(const __FlashStringHelper *s);

// 开始/结束捕捉。seCaptureEnd 返回缓冲首地址（NUL 结尾），*len 可为 nullptr。
void seCaptureBegin();
const char *seCaptureEnd(size_t *len);

#endif
