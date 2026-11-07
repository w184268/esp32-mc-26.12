# esp32-mc-26.1
ESP32MC — 在一块 4MB Flash 的经典 ESP32 上跑 Minecraft Java 服务端
编译、烧录、上电，192.168.x.x:25565 就能进世界。 协议版本 Minecraft Java 26.1.2 / 775，目标板为经典 ESP32（ESP32-D0WD，无 PSRAM），也兼容 ESP32-S3。[![Protocol](https://img.shields.io/badge/protocol-26.1.2%20%2F%20775-green)]() [![MCU](https://img.shields.io/badge/MCU-ESP32--D0WD%20%2F%204MB-blue)]() [![License](https://img.shields.io/badge/license-GPLv3-orange)](LICENSE)

这是什么
这不是把官方服务端塞进单片机——Java 本体连装都装不下。这个项目直接用 C++ 手写了 Minecraft 服务端所需的那一小部分协议：握手、状态查询、登录、Play 状态的区块包与实体包，再加上地形生成、方块交互、背包与合成。
于是你可以：
一块 20 块钱的 ESP32 开发板当一个真正的 MC 服务器
用原版 Java 客户端直连（不加 Mod、不改客户端）
顺着代码把"服务端到底怎么跟客户端说话"从头看到尾
当前视距 1、最多 5 名玩家。它能让你在手机上挖方块、放方块、开箱子、退出再进来背包还在——而不是只能看个登录成功。

功能特性
分类
已实现
协议
Handshake / Status / Login / Play 全流程，Version 775
世界
固定种子程序化地形（含矿洞、树木、水、岩浆等结构）、生物群系、流体
玩法
移动、挖掘、放置、背包、基础合成、熔炉、箱子、部分 Mob
存档
世界种子、方块改动日志、箱子内容、玩家背包/坐标/血量/饥饿，掉电不丢
配网
局域网内常驻网页配网（不用按键、不用热点）、热点自动回落、串口 AT 命令
稳定性
心跳日志、WiFi 掉线看护自动重连、监听 socket 自愈、网页服务独立核心


硬件要求
实测板：ESP32-D0WD rev 1.0，2 核，4MB Flash，无 PSRAM（丝印 ANT-0130 一类的双排 44pin 板）。
项目
要求
芯片
经典 ESP32 或 ESP32-S3（本项目在经典 ESP32 上验证）
Flash
4MB 起（世界存档依赖 SPIFFS，需要 Huge APP 分区）
PSRAM
不需要
供电
USB 供电即可，建议别用劣质线（WiFi 发射峰值会拉低电压）

⚠️ 踩过的硬件坑（重要）
GPIO6~11 接板载 SPI Flash（其中 GPIO9 = SD2）。对这些引脚做 pinMode/digitalWrite 会立刻触发 TG1WDT_SYS_RESET 复位。
状态灯只能用 GPIO2，代码里已有 kLedUsable 保护。
GPIO0（BOOT 键）在很多开发板上按住会进 ROM 串口下载模式，屏幕/串口只有：
load:0x40078000,len:16560
load:0x40080400,len:3500
entry 0x400805b4

此时固件根本没运行。所以本项目不依赖 BOOT 键进配网，改成"局域网常驻配网页"。

快速开始
烧录
用 Arduino IDE 打开 code/code.ino（整个 code/ 目录一起打开），按下表配置：
Arduino IDE 选项
值
Board
ESP32 Dev Module
Flash Size
4MB (32Mb)
Partition Scheme
Huge APP (3MB No OTA / 1MB SPIFFS) ← 必须，否则没有 SPIFFS、存档直接不可用
PSRAM
Disabled
Upload Speed
115200
Port
你的串口号（如 COM6）

编译占用参考：程序空间约 75%，全局变量约 37%。
开机
第一次开机没有保存过 WiFi，会自动开热点：
+BOOTCFG:TRY,SSID="你的WiFi"
+BOOTCFG:NO_NETWORK,ENTERING_WEBCFG
+WEBCFG:ENTER
+WEBCFG:AP,SSID="ESP32MC-CFG",IP="192.168.4.1"

手机连 ESP32MC-CFG（开放网络，无密码），浏览器打开 http://192.168.4.1/，填 SSID 和密码，保存。
保存时服务端会先真实连接一次验证（最多 20 秒），连接成功才写入 Flash 并重启；失败返回 502 且保留原配置，不会把你的网络搞坏。
进游戏
配网成功重启后，串口会打出：
WiFi connected, IP: 192.168.31.49
+WEBCFG:LAN_URL,http://192.168.31.49/
Server listening on port 25565

Minecraft Java 客户端 → 多人游戏 → 添加服务器 → 192.168.31.49:25565 → 进入世界。

配网：三种方式，随便挑
方式一：局域网网页（推荐，日常就用这个）
开机连上路由器后，配网页面一直在线，同网段的手机/电脑直接访问：
http://<设备IP>/

页面显示当前连接的 SSID 与 IP
点「扫描附近 WiFi」列出周边网络
换网时填 SSID + 密码，保存 → 验证 → 5 秒后自动重启生效
不用按任何键，不用开热点，也不会打断正在玩的玩家（网页服务跑在 core 0 的独立任务里）。
方式二：自动回落热点
开机连不上已保存的网络（20 秒超时）→ 自动开 ESP32MC-CFG 热点
运行中掉线连续重试 2 分钟仍失败 → 自动开热点（+WIFI:GIVE_UP,ENTERING_WEBCFG）
热点 180 秒无操作自动关闭，避免长期挂着
方式三：串口 AT 命令
串口监视器（115200）发送：
#WIFI_UART_CFG#        进入串口配网模式
AT+HELP                查看帮助
AT+WSCAN               扫描周边 WiFi
AT+WSET="ssid","pass"  填写凭据（先不落盘）
AT+WSAVE               验证 + 保存 + 重启
AT+WINFO?              查看当前状态
AT+WEXIT               退出配网模式

其他串口命令：
命令
作用
#WEBCFG#
强制进入配网（开热点）
#WORLD_INFO#
查询存档状态
#WORLD_RESET#
清空世界存档并重启


世界存档
地形不存盘——它由固定种子确定性生成，重启后每一块地形都一致。真正落盘的是玩家的痕迹：
文件
内容
/w.seed
世界种子 + RNG 种子（12 字节）
/w.log
方块改动追加日志；type=1 的行额外携带 81 字节箱子内容
/p0.dat … /p4.dat
每个玩家的背包（41 格）、合成格、坐标、朝向、血量、饥饿、UUID

写入策略：
方块改动攒 16 条或 15 秒落盘一次
玩家状态每 30 秒存一次，断线立刻存
日志超过 200KB 自动压缩重写
重进服务器按 UUID 命中同一槽位，背包和坐标自动续上
未持久化：怪物、游戏内时间、掉落物、合成台/熔炉里正在烧的东西。

协议实现要点（想自己改协议的人看这里）
这个项目的区块包曾经卡了很久，问题很值得记录——Minecraft 1.21.5+ 的 level_chunk_with_light 和旧文档差别很大：
高度图不再是 NBT，而是 Prefixed Array of Heightmap：
varint(数量) + 每项 varint(类型) + varint(长度) + uint64 数组
类型 1 = WORLD_SURFACE、4 = MOTION_BLOCKING。
数据数组不允许跨 long 边界打包（1.16 起就如此），并且取消了长度前缀。
entries_per_long = 64 / bits，每个 long 里塞满即换下一个。
光照掩码位序差 1：掩码位数为 段数 + 2，bit 0 是"世界最低段下面那一段"，世界第 k 段对应 bit k + 1。
level_chunk_with_light 的 Play 包 ID 本项目用 0x2D（Wiki 现文档写 46/0x2E），在该版本客户端上 0x2D 实测可用，别乱改。
一个诊断技巧：客户端报 found N bytes extra 时，看这个 N 是否固定。如果每次都是一个恒定值（本项目是 5659 左右），说明不是网络丢包、也不是长度算错，而是字节布局和客户端预期不符——从第一个错位的字段开始查，比对着 TCP 抓包有用的多。

稳定性设计
机制
说明
心跳
每 5 秒 [hb] up=..wifi=..heap=..srv=0/1，与 WiFi 无关，用来区分"卡死"和"只是没网"
WiFi 看护
掉线每 10 秒重试一次，日志 +WIFI:RETRY,n / +WIFI:REGAINED,retries=n / +WIFI:GIVE_UP
监听自愈
掉线时显式 server_.end()，重连后真正重新 begin()（否则端口会永久失效，只能重启板子）
网页隔离
网页服务跑在 core 0 独立 FreeRTOS 任务，慢速浏览器不会拖住 core 1 的游戏主循环
省电关闭
WiFi.setSleep(false)，发包不再等睡眠窗口，延迟抖动明显变小


目录结构
code/                    主线固件（Arduino 打开这里）
├── code.ino             入口：串口、WiFi 事件、LED、主循环、心跳
├── mc_server.cpp/.h     协议状态机、区块包、背包/箱子、玩家状态
├── network_layer.cpp/.h 网络层：WiFi 连接、监听 socket 生命周期
├── wifi_config.cpp/.h   配网：局域网常驻网页、热点回落、串口 AT
├── world_store.cpp/.h   存档：种子、改动日志、箱子、玩家
├── packet_codec.cpp/.h  数据包编解码（VarInt / 字符串 / Payload）
├── terrain.cpp/.h       地形与区块生成
├── procedures.cpp/.h    玩家行为、方块交互、Mob、Tick
├── crafting.cpp/.h      合成与熔炉
├── game_state.cpp/.h    全局游戏状态
├── registries.cpp/.h    协议注册表与静态数据
├── game_types.h         常量与开关（视距、最大玩家、端口都在这）
└── win/                 Windows 本地调试版
EP1/                      教学分阶段代码
Template/                 初始工程模板

常用开关都在 code/game_types.h：
#define VIEW_DISTANCE   1     // 视距（一次进世界 8 个区块）
#define MAX_PLAYERS     5     // 最大玩家数
// 端口 25565


在 Windows 上本地调试
没有板子也能跑协议和逻辑：
cd code\win
build_win.bat

脚本首次运行会自动准备 w64devkit，然后编译并启动本地服务器。

常见问题
Q：客户端报「网络协议错误 / 加载地形错误」怎么办？ 确认客户端版本是 26.1.2 / 775。版本不匹配时第一个区块包就会错位。
Q：25565 连不上，网页也打不开？ 看串口是否有 +MC:SRV_DOWN,WIFI_LOST 之后没有 Server listening on port 25565——那是监听没重建；本版已修。若串口直接蹦 ESP32MC starting...，是复位了，看 Reset reason。
Q：按住 BOOT 想进配网，结果没反应？ 这块板的 BOOT 键会触发 ROM 下载模式，固件压根没跑。用「方式一」局域网网页，或串口 #WEBCFG#。
Q：编译报 .dram0.bss will not fit / region dram0_0_seg overflowed？ 别用 S3 的板型选项；确认 Partition 选的是 Huge APP；本项目已把配网对象全部放堆上，正常不会溢出。
Q：能加视距吗？ 可以改 VIEW_DISTANCE，但内存和带宽会线性涨。经典 ESP32 无 PSRAM，视距 2 时区块包已经很大，视距 1 是实测最稳的值。
Q：多少人能一起玩？ MAX_PLAYERS 5，实际建议 2~3 人，主循环是单线程的。

许可与致谢
原始项目：p2r3/bareiron —— 原始部分版权归其作者与贡献者所有
ESP32MC（Arduino/S3 移植与 C++ 重构）：GYGKHD，2026
本项目在其基础上继续修改：经典 ESP32 适配、协议 775 区块包修正、世界/玩家存档、常驻网页配网、掉线自愈
整个派生项目按 GNU General Public License v3 发布。你可以使用、研究、修改、再发布，但再发布必须继续遵守 GPLv3 并提供源码。
完整许可证：LICENSE
来源与修改声明：NOTICE
本项目与 Mojang / Microsoft 无任何关联，不含任何官方游戏资源文件。Minecraft 是 Mojang Studios 的商标
