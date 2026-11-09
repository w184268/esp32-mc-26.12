# ESP32MC

在 **一块 ESP32 上跑真正的 Minecraft Java 版服务端**。没有 Linux、没有 JVM、没有外部主机——
纯 Arduino 框架的 C++ 固件自己实现了 Minecraft Java 版网络协议，客户端直接连板子的热点就能进服。

- 目标客户端：**Minecraft Java Edition 26.1.2**（协议按该版本客户端 jar 逐一实证）
- 目标硬件：**LilyGO T-Call V1.4 / ESP32-WROVER**（必须带 PSRAM，8 MB）
- 许可：**GPL-3.0**（见 `LICENSE` 与 `NOTICE`）

> 本项目与 Mojang / Microsoft 无关，不包含任何 Mojang 的美术、音效或代码资源。
> 客户端需要你自己合法持有。

---

## 目录

- [现在能玩到什么](#现在能玩到什么)
- [硬件与编译](#硬件与编译)
- [第一次开机 / 怎么连](#第一次开机--怎么连)
- [串口命令](#串口命令)
- [源码结构](#源码结构)
- [协议实现要点](#协议实现要点)
- [已知限制](#已知限制)
- [版本轨迹](#版本轨迹)
- [许可与致谢](#许可与致谢)

---

## 现在能玩到什么

| 分类 | 已实现 |
|---|---|
| 世界 | 程序化地形（生物群系、树、矿、洞穴入口）、区块按需生成与下发、方块破坏/放置、重力方块、流体简单流动 |
| 生存 | 生命/饥饿/摔落与溺水伤害、死亡与重生、经验条与等级、物品掉落与拾取、背包 36 格 + 快捷栏 |
| 方块交互 | 工作台（3×3 合成）、熔炉（烧炼 + 预览）、箱子（27 格）、附魔台（三档附魔 + 青金石）、铁砧（合并 + 改名）、锻造台（原版四格 + 下界合金升级） |
| 战斗 | 近战伤害与击退、生物（鸡/牛/猪/羊/僵尸/骷髅/蜘蛛/苦力怕）、掉落物 |
| 其他 | 聊天、命令、天气、时间、区块与实体同步、存档（世界种子 / 方块改动 / 玩家） |
| 运维 | 开机自建热点 + 网页配置向导（Wi-Fi、世界开关、串口调试映射）、串口控制台 |

几处容易被问到的地方：

- **附魔台**：三档费用与附魔提示完全靠 `container_set_data` 的 10 个数据槽驱动，物品一变就重算重发。
- **铁砧改名**：接住了 serverbound `rename_item`，名字以 `custom_name` 组件随槽位下发。
- **锻造台**：不是"长得像"的四格界面——服务端会先下发 `update_recipes`（三张配方属性表），
  客户端才允许把东西放进模板格 / 底材格 / 附加格；产物由服务端结算。

---

## 硬件与编译

### 硬件要求

- ESP32-WROVER（**必须 8 MB PSRAM**）。普通 ESP32-WROOM 会因 `ESP.getPsramSize() == 0` 直接跑不起来。
- 4 MB Flash 起（`huge_app` 分区，程序占用约 1.13 MB）。
- USB 串口（烧录 + 控制台）。

### 编译（arduino-cli）

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32wrover \
  --board-options PartitionScheme=huge_app \
  --build-path ./build \
  ./src/code
```

### 烧录

```bash
arduino-cli upload -p COM6 \
  --fqbn esp32:esp32:esp32wrover \
  --board-options PartitionScheme=huge_app \
  --build-path ./build \
  ./src/code
```

Windows 上如果 `arduino-cli` 找不到，它一般在
`%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe`。

### 说明

- 板型**必须**是 `esp32wrover`：本固件把区块缓存、菜单缓冲等大对象放在 PSRAM，
  用 `esp32:esp32:esp32`（WROOM）链接会直接 `dram0_0_seg overflow`。
- 静态内存很紧：当前 `Global variables` 约 124 KB / 320 KB，新增常驻缓冲请优先放 PSRAM。

---

## 第一次开机 / 怎么连

1. 烧录后板子自动起热点：**SSID `ESP32MC`**（默认开放，可在配置页设密码），IP `192.168.4.1`。
2. 手机/电脑连上这个热点，浏览器打开 **`http://192.168.4.1`** 进配置向导：
   - 填家里 Wi-Fi 的 SSID / 密码（板子会同时以 STA 连上路由，便于从局域网直连）
   - 世界相关开关（重置世界、掉落物、区块半径等）
   - **串口调试**：页面里有一个命令输入框 + 发送按钮，直接把你输入的内容打到串口控制台
3. 客户端（PCL / 官方启动器）添加服务器：
   - 板子在热点模式：地址 `192.168.4.1`
   - 板子已连上家里路由：地址就是配置页显示的 STA IP（例如 `192.168.31.49`）
   - 版本选 **26.1.2**，离线模式（服务器不校验正版会话）
4. 第一次进服建议先执行一次世界重置（配置页开关或串口命令），避免旧存档与新地形冲突。

---

## 串口命令

串口（115200）同时是控制台。常用命令：

| 命令 | 作用 |
|---|---|
| `#SAVE#` | 立刻把所有玩家与世界改动落盘（SPIFFS） |
| `#WORLD_INFO#` | 打印世界种子、方块改动数量、在线玩家 |
| `AT+DROPS?` | 查询掉落物开关与当前掉落物数量 |
| `AT+STA?` | 查询 STA 状态（`ENABLED / ON / LINK / IP / RSSI`） |
| `AT+APINFO?` | 查询热点信息（SSID、客户端数、IP） |
| `#APOPEN#` | 把热点切回开放模式（清掉热点密码） |
| `!help` | 游戏内帮助（与聊天命令同一套） |

串口输入也会被镜像到游戏内聊天（`serial_echo`），所以调试时可以两边同时看。

---

## 源码结构

```
src/code/
├── code.ino            # 入口：setup/loop、各模块装配
├── mc_server.cpp/.h    # 核心：登录、play 包分发、方块/实体/窗口/命令（最大的一块）
├── packet_codec.cpp/.h # 协议读写封装：VarInt、字符串、定长读写、写超时
├── registries.cpp/.h   # 方块 state / 物品注册表映射（含 256 项方块调色板与 B_to_I）
├── terrain.cpp/.h      # 地形生成（高度图、树、矿、洞穴）
├── crafting.cpp/.h     # 合成表（表驱动）与匹配逻辑
├── procedures.cpp/.h   # 槽位映射、挖掘产物、跨模块小工具
├── world_store.cpp/.h  # SPIFFS 存档：/w.seed、/w.log、/w.state、/pN.dat…
├── wifi_config.cpp/.h  # 热点 + STA + 网页配置向导 + 串口控制台命令
├── serial_echo.cpp/.h  # 串口 ↔ 游戏聊天桥
├── network_layer.cpp/.h# socket 收发封装
├── game_state.cpp/.h   # 全局运行状态
└── game_types.h        # 共享结构体/常量
```

---

## 协议实现要点

这一节记录那些"看代码看不出来、但错了就掉线"的坑，改协议相关代码前建议先读。

### 包 id 必须按 VarInt 写，不是"一个字节"

协议里的 packet id 是 VarInt。id < 128 时看起来就是一个字节，很容易误写成 `writeByte()`：

```cpp
// 错：133 = 0x85，最高位是 1，客户端会继续读下一个字节
codec.writeByte(0x85);
// 对：133 的 VarInt 是 0x85 0x01
codec.writeVarInt(133);
```

写错的典型现象：客户端报 `Received unknown packet id 389`（389 = `0x85 + 0x03*128`，
正好是"单字节 id + 紧随其后的真实字段"被拼成了一个 VarInt）。

### 物品组件补丁（DataComponentPatch）的字段顺序

```
VarInt 新增组件数量
VarInt 删除组件数量      ← 两个数量都在最前面
然后才是 新增条目 / 删除条目
```

把"删除数量 0"写在条目末尾，客户端会把组件 id 当成删除数量继续读，
直接 `VarInt too big` → `Failed to decode packet 'clientbound/minecraft:container_set_slot'`。

### update_recipes（0x85 / id 133）

26.1.2 的这个包只带「配方属性表」，不再带整份配方：

```
VarInt 表数量
  每个表：[Identifier 字符串][VarInt 物品数量][每个物品 VarInt 注册表 id]
VarInt 切石机配方数量（没有就写 0，别漏）
```

锻造台三格能不能放东西，完全由这三张表决定（`smithing_template` / `smithing_base` /
`smithing_addition`）。不发这个包 = 表为空 = 客户端本地拒收任何物品，服务端连日志都没有。

### 容器数据槽

- `container_set_data`：`window_id` 与 `data_id`、`value` 都是 **short**，不是 VarInt。
- `EnchantmentMenu` 有 10 个数据槽：`0=附魔种子`、`1..3=三档费用`、`4..6=附魔提示`、`7..9=等级要求`。
  客户端的按钮亮/灭只认这些值，物品一变动就必须重发。

### 槽位与"产物落点"

服务端不要自己挑背包格塞产物——客户端会按原版语义把产物放到"鼠标手上"，
两边各写一份就会互相覆盖，最后卡死/掉线，还可能因为没落盘丢物品。
本项目统一走"产物落鼠标"（`flagval_16/8`），并在取走时立刻 `worldStoreSavePlayer()`。

---

## 已知限制

- 只面向 **26.1.2** 一个客户端版本；换版本必须重新核对包 id、注册表 id 与组件 id。
- 方块渲染是"只发 state id 的简化实现"，楼梯/半砖/朝向类方块没有完整朝向系统（这些方块在本服被禁用）。
- 附魔只实现了锋利一条（等级由按钮档位决定），没有完整附魔表与铁砧合并附魔。
- 铁砧自定义名只存在内存里：重启或把物品移到别的槽后，名字会退回原名（未做持久化）。
- 世界高度、视距、生物 AI 都是"够玩"级别，不是原版算法。
- 生物群系/结构/音效/粒子未实现。
- 单板玩家上限由 `MAX_PLAYERS` 决定（默认 4），受内存约束，不建议加大。

---

## 版本轨迹

| 版本 | 内容 |
|---|---|
| k12 | 放置飞出去/自埋修复（100 ms 限速 + 位置纠正） |
| k13 | 附魔台、铁砧开窗 |
| k14 | 锻造台方块 + 一次性存档重置 |
| k15 | 附魔台数据槽补全 |
| k16 | 首次下沉 `update_recipes`，锻造台回到 menu 21 |
| k17 | 铁砧改名收包（0x30）、`custom_name` 组件 |
| k18 | 修正 133 的双字节 VarInt（掉线问题） |
| k19 | 修正组件补丁顺序、附魔数据槽重算、配方表改为原版成员清单 |
| k20 | 锻造台每次点击重算结果格、产物改落鼠标、取走即落盘 |

---

## 许可与致谢

- 本仓库代码以 **GPL-3.0** 发布，完整条款见 `LICENSE`，修改与归属说明见 `NOTICE`。
- 协议实现思路与部分基础结构参考了开源项目 **bareiron**（Go 版 Minecraft 服务端）与 **GYGKHD** 的相关工作，
  在此致谢；本项目在其基础上针对 ESP32 的内存与算力约束重写。
- `NOTICE` 中列出了第三方内容的来源与修改范围。分发二进制时请一并保留 `LICENSE` 与 `NOTICE`。
