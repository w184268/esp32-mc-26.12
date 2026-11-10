// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.

#include "mc_server.h"
#include "world_store.h"
#include "serial_echo.h"
#include <SPIFFS.h>          // 附魔等级单独存 /eN.dat
#include <math.h>            // 鞘翅推进方向要算 sin/cos
#include <stdarg.h>
// 这几个缓冲共 ~3.3KB，DRAM 装不下（实测 dram0_0_seg 溢出 3424B），统一放 PSRAM
static uint8_t (*g_menu31)[81] = nullptr;          // 锻造台 9x3 容器的 27 格（2B 物品 + 1B 数量）
// 附魔组件 id 与锋利 id（客户端 26.1.2 的权威取值，见 probe/gen1261/reports/registries.json）
#define kCompEnchantments 13         // minecraft:enchantments
#define kCompStoredEnchantments 42   // minecraft:stored_enchantments（附魔书用这个）
#define kEnchSharpnessId 33          // minecraft:enchantment/minecraft:sharpness

static char (*g_pending_name)[32] = nullptr;       // 铁砧改名框里正在输入的名字
static char (*g_item_name)[42][32] = nullptr;      // 每格物品的自定义名（RAM，重启清空；下标 0..40 = 快捷栏9+背包27+护甲4+副手1，
                                                   // 下标 41 = 铁砧输入格（窗口 8 槽 0）自己的名字，改名改回去要靠它）
static uint8_t g_craft_ench[MAX_PLAYERS][9];       // 合成/附魔/铁砧/锻造格里物品的附魔等级（RAM）
// 鞘翅滑翔状态。不进 PlayerData：那是定长落盘结构，加字段会让旧 /pN.dat 读不出来。
static uint8_t g_gliding[MAX_PLAYERS];

// 容器/附魔排查用的环形日志：常驻 RAM（PSRAM），客户端复现后发串口 #DIAG# 一次性倒出来。
// 这样"改完还是一样"时能直接看服务端到底收到了什么、回了什么，不用再猜。
#define kDiagSlots 128
#define kDiagChars 84
static char (*g_diag)[kDiagChars] = nullptr;
static uint8_t g_diag_head = 0, g_diag_cnt = 0;

// ---- 崩溃取证：板子自己把证据留下来，不用你贴日志 ----
// RTC 内存不掉电不清，重启后还知道最后处理过哪些包；崩之前的日志尾巴写进 SPIFFS。
#include <esp_system.h>
#define kBrMagic 0x4553324DUL   // 'ES2M'
RTC_NOINIT_ATTR static uint32_t g_br_magic;
RTC_NOINIT_ATTR static uint8_t  g_br_ids[48];
RTC_NOINIT_ATTR static uint8_t  g_br_pos;
RTC_NOINIT_ATTR static uint32_t g_br_cnt;

#define kDiagTail 20
static char (*g_dg_tail)[kDiagChars] = nullptr;
static uint32_t g_dg_last = 0;

RTC_NOINIT_ATTR static int g_br_boot_rr;

void mcBrSetBootRR(int rr) { g_br_boot_rr = rr; }

void mcBrMark(uint8_t packet_id) {
  if (g_br_magic != kBrMagic) { g_br_magic = kBrMagic; g_br_pos = 0; g_br_cnt = 0; }
  g_br_ids[g_br_pos] = packet_id;
  g_br_pos = (uint8_t)((g_br_pos + 1) % sizeof(g_br_ids));
  g_br_cnt++;
}

void mcBrDump() {
  if (g_br_magic != kBrMagic) { Serial.println("+BR:NONE"); return; }
  Serial.printf("+BR:CNT=%lu BOOT_RR=%d RR=%d\r\n",
                (unsigned long)g_br_cnt, (int)g_br_boot_rr, (int)esp_reset_reason());
  uint8_t n = (uint8_t)(g_br_cnt < sizeof(g_br_ids) ? g_br_cnt : sizeof(g_br_ids));
  uint8_t start = (uint8_t)((g_br_pos + sizeof(g_br_ids) - n) % sizeof(g_br_ids));
  Serial.print("+BR:IDS=");
  for (uint8_t k = 0; k < n; k++) {
    uint8_t idx = (uint8_t)((start + k) % sizeof(g_br_ids));
    if (k) Serial.print(' ');
    Serial.printf("%02X", (unsigned)g_br_ids[idx]);
  }
  Serial.println();
}

// 把环形日志最后 kDiagTail 行写进 SPIFFS（节流 800ms），重启后 #DIAG# 还能看到崩溃前发生了什么
static void diagFlushTail_() {
  if (g_diag_cnt == 0) return;
  uint32_t now = millis();
  if ((uint32_t)(now - g_dg_last) < 800) return;
  g_dg_last = now;
  if (g_dg_tail == nullptr) {
    g_dg_tail = (char (*)[kDiagChars])heap_caps_malloc((size_t)kDiagTail * kDiagChars,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g_dg_tail == nullptr) return;
  }
  File f = SPIFFS.open("/dg.txt", "w");
  if (!f) return;
  uint8_t n = (uint8_t)(g_diag_cnt < kDiagTail ? g_diag_cnt : kDiagTail);
  uint8_t start = (uint8_t)((g_diag_head + kDiagSlots - n) % kDiagSlots);
  for (uint8_t k = 0; k < n; k++) {
    uint8_t idx = (uint8_t)((start + k) % kDiagSlots);
    f.print(g_diag[idx]);
    f.print('\n');
  }
  f.close();
}

static void diagInit_() {
  if (g_diag != nullptr) return;
  g_diag = (char (*)[kDiagChars])heap_caps_malloc((size_t)kDiagSlots * kDiagChars,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (g_diag == nullptr) g_diag = (char (*)[kDiagChars])malloc((size_t)kDiagSlots * kDiagChars);
  if (g_diag) memset(g_diag, 0, (size_t)kDiagSlots * kDiagChars);
}

void mcDiag_(const char* fmt, ...) {
  diagInit_();
  if (g_diag == nullptr) return;
  char* dst = g_diag[g_diag_head];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(dst, kDiagChars - 1, fmt, ap);
  va_end(ap);
  dst[kDiagChars - 1] = 0;
  g_diag_head = (uint8_t)((g_diag_head + 1) % kDiagSlots);
  if (g_diag_cnt < kDiagSlots) g_diag_cnt++;
  // 注意：这里**不能**立刻 Serial.print。115200 波特下一行 84 字节要 ~7ms，
  // 一次容器点击会打十几行，主循环被串口拖住 → 客户端 keepalive 超时 → "卡死闪退连接超时"。
  // 只入环形缓冲，需要时用串口 #DIAG# 一次倒出来。
  diagFlushTail_();
}

void mcDiagDump() {
  diagInit_();
  if (g_diag == nullptr) { Serial.println("+DIAG:NO_BUFFER"); return; }
  mcBrDump();   // 顺带报上一次崩溃的面包屑（RR = esp_reset_reason）
  Serial.printf("+DIAG:COUNT=%u\r\n", (unsigned)g_diag_cnt);
  uint8_t start = (g_diag_cnt >= kDiagSlots) ? g_diag_head : 0;
  for (uint8_t k = 0; k < g_diag_cnt; k++) {
    uint8_t idx = (uint8_t)((start + k) % kDiagSlots);
    Serial.println(g_diag[idx]);
    if ((k & 7) == 7) delay(2);   // 让 WiFi 栈有机会跑，避免一次倒 128 行把连接拖断
  }
  Serial.println("+DIAG:END");
  {
    File f = SPIFFS.open("/dg.txt", "r");
    if (f) {
      Serial.println("+DG:BEGIN");
      while (f.available()) Serial.write(f.read());
      f.close();
      Serial.println("+DG:END");
    }
  }
}

// 客户端回传的组件补丁我们不一定读得懂（日志里 [CHG] ok=0 就是这种）。
// 所以附魔台/铁砧格子里的东西被拿走时，先把它带着的附魔/名字记成“待携带”，
// 等它真的落进背包某一格（item id 对得上）再贴上去 —— 不再依赖客户端把组件送回来。
struct CarryItem {
  uint16_t item;
  uint8_t  lv;
  uint8_t  id;
  uint8_t  valid;
  char     name[32];
};
static CarryItem g_carry[MAX_PLAYERS];

// ============================================================
// 物品级的“名字 / 附魔”记忆
// ------------------------------------------------------------
// 服务端只按格子记名字和附魔，一上鼠标（鼠标物品没有组件原文）就断链：
// “附魔后放回铁砧就没了”“改名后换个格子又变回去”都是这个。
// 这里按物品 id 记一份最近的名字/附魔，物品落进任何一格都能补回来。
struct ItemMeta { uint16_t item; uint8_t lv; uint8_t id; char nm[32]; };
// 这几张表放 PSRAM（.bss 放不下：dram0_0_seg 只差几百字节就溢出）
static ItemMeta (*g_meta)[4] = nullptr;     // [MAX_PLAYERS][4]
static uint8_t  (*g_meta_rr) = nullptr;     // [MAX_PLAYERS]

static void metaRemember_(int pi, uint16_t item, uint8_t lv, const char* nm) {
  if (g_meta == nullptr || g_meta_rr == nullptr) return;
  if (pi < 0 || pi >= MAX_PLAYERS || item == 0) return;
  if (lv == 0 && (nm == nullptr || nm[0] == 0)) return;
  for (int k = 0; k < 4; k++) {
    if (g_meta[pi][k].item == item) {
      if (lv > g_meta[pi][k].lv) g_meta[pi][k].lv = lv;
      if (nm != nullptr && nm[0] != 0) strlcpy(g_meta[pi][k].nm, nm, sizeof(g_meta[pi][k].nm));
      return;
    }
  }
  for (int k = 0; k < 4; k++) {
    if (g_meta[pi][k].item == 0) {
      g_meta[pi][k].item = item;
      g_meta[pi][k].lv = lv;
      g_meta[pi][k].id = (uint8_t)kEnchSharpnessId;
      g_meta[pi][k].nm[0] = 0;
      if (nm != nullptr) strlcpy(g_meta[pi][k].nm, nm, sizeof(g_meta[pi][k].nm));
      return;
    }
  }
  int k = (int)(g_meta_rr[pi]++ & 3);
  g_meta[pi][k].item = item;
  g_meta[pi][k].lv = lv;
  g_meta[pi][k].id = (uint8_t)kEnchSharpnessId;
  g_meta[pi][k].nm[0] = 0;
  if (nm != nullptr) strlcpy(g_meta[pi][k].nm, nm, sizeof(g_meta[pi][k].nm));
}

static bool metaRecall_(int pi, uint16_t item, uint8_t* lv, const char** nm) {
  if (g_meta == nullptr) return false;
  if (pi < 0 || pi >= MAX_PLAYERS) return false;
  for (int k = 0; k < 4; k++) {
    if (g_meta[pi][k].item == item) {
      if (lv) *lv = g_meta[pi][k].lv;
      if (nm) *nm = (g_meta[pi][k].nm[0] != 0) ? g_meta[pi][k].nm : nullptr;
      return true;
    }
  }
  return false;
}

// 窗口输入格（附魔台/铁砧 0、1 号格）发包去重：内容没变就不发，
// 否则每点一次都重发一遍，客户端物品被替换 → tooltip/附魔提示一直在闪。
static uint16_t (*g_ws_item)[2] = nullptr;
static uint8_t  (*g_ws_cnt)[2] = nullptr;
static uint8_t  (*g_ws_lv)[2] = nullptr;
static char     (*g_ws_nm)[2][32] = nullptr;
static uint8_t  (*g_ws_win) = nullptr;

static void wsReset_(uint8_t c) {
  if (g_ws_item == nullptr || c >= MAX_PLAYERS) return;
  memset(g_ws_item[c], 0, sizeof(g_ws_item[c]));
  memset(g_ws_cnt[c], 0, sizeof(g_ws_cnt[c]));
  memset(g_ws_lv[c], 0, sizeof(g_ws_lv[c]));
  g_ws_nm[c][0][0] = 0; g_ws_nm[c][1][0] = 0;
}
static bool wsDiff_(uint8_t c, int k, uint16_t item, uint8_t cnt, uint8_t lv, const char* nm) {
  if (g_ws_item == nullptr || g_ws_nm == nullptr) return true;
  if (c >= MAX_PLAYERS || k < 0 || k > 1) return true;
  if (g_ws_item[c][k] != item || g_ws_cnt[c][k] != cnt || g_ws_lv[c][k] != lv) return true;
  bool had = (g_ws_nm[c][k][0] != 0), has = (nm != nullptr && nm[0] != 0);
  if (had != has) return true;
  if (has && strcmp(g_ws_nm[c][k], nm) != 0) return true;
  return false;
}
static void wsSave_(uint8_t c, int k, uint16_t item, uint8_t cnt, uint8_t lv, const char* nm) {
  if (g_ws_item == nullptr || g_ws_nm == nullptr) return;
  if (c >= MAX_PLAYERS || k < 0 || k > 1) return;
  g_ws_item[c][k] = item; g_ws_cnt[c][k] = cnt; g_ws_lv[c][k] = lv;
  g_ws_nm[c][k][0] = 0;
  if (nm != nullptr && nm[0] != 0) strlcpy(g_ws_nm[c][k], nm, sizeof(g_ws_nm[c][k]));
}
static uint8_t wsBegin_(uint8_t c, int window_id) {
  if (g_ws_item == nullptr || g_ws_win == nullptr || c >= MAX_PLAYERS) return 0;
  if (g_ws_win[c] != (uint8_t)window_id) { wsReset_(c); g_ws_win[c] = (uint8_t)window_id; }
  return c;
}



// 把刚才这一包读到的东西 hexdump 进诊断日志（配合 PacketCodec 的读字节追踪）
static void diagTraceDump_() {
  uint8_t buf[96];
  size_t n = pktTraceCopy(buf, sizeof(buf));
  mcDiag_("[TRACE] bytes=%u", (unsigned)n);
  for (size_t off = 0; off < n; off += 12) {
    char line[kDiagChars];
    int k = snprintf(line, kDiagChars, "[T%02u]", (unsigned)off);
    if (k < 0) k = 0;
    for (size_t i = off; i < n && i < off + 12; i++) {
      if ((size_t)k + 4 >= kDiagChars) break;
      k += snprintf(line + k, (size_t)kDiagChars - (size_t)k, " %02X", (unsigned)buf[i]);
    }
    mcDiag_("%s", line);
  }
}

static void menuBuffersInit() {
  if (g_item_name != nullptr) return;
  // 一块 PSRAM 切给所有容器/物品元数据表。
  // 注意：这里的偏移必须与实际类型一致（g_item_name 是 [42][32]，以前写成 41 会重叠）
  size_t per_player = (size_t)(42 * 32 + 32 + 81) + sizeof(ItemMeta) * 4 + 1
                      + 2 * (sizeof(uint16_t) + 2) + 2 * 32 + 1;
  size_t need = (size_t)MAX_PLAYERS * per_player;
  void *mp = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (mp == nullptr) { Serial.println("[MENU] PSRAM alloc failed"); return; }
  memset(mp, 0, need);
  uint8_t *base = (uint8_t *)mp;
  g_item_name = (char (*)[42][32])base;
  base += (size_t)MAX_PLAYERS * 42 * 32;
  g_pending_name = (char (*)[32])base;
  base += (size_t)MAX_PLAYERS * 32;
  g_menu31 = (uint8_t (*)[81])base;
  base += (size_t)MAX_PLAYERS * 81;
  g_meta    = (ItemMeta (*)[4])base;
  base += (size_t)MAX_PLAYERS * sizeof(ItemMeta) * 4;
  g_meta_rr = (uint8_t *)base;
  base += (size_t)MAX_PLAYERS;
  g_ws_item = (uint16_t (*)[2])base;
  base += (size_t)MAX_PLAYERS * 2 * sizeof(uint16_t);
  g_ws_cnt  = (uint8_t (*)[2])base;
  base += (size_t)MAX_PLAYERS * 2;
  g_ws_lv   = (uint8_t (*)[2])base;
  base += (size_t)MAX_PLAYERS * 2;
  g_ws_nm   = (char (*)[2][32])base;
  base += (size_t)MAX_PLAYERS * 2 * 32;
  g_ws_win  = (uint8_t *)base;
}

// Set Entity Data 包 id：26.1.2 真实协议表实测为 0x63（99）
// （0x5C=92 是 set_border_warning_distance，0x64=100 是 set_entity_link，
//   之前误把 0x5C 当 entity data，导致客户端 "found N bytes extra" 掉线）
// 串口 AT+EDATA=<hex> 可热改
static uint8_t g_entity_data_id = 0x63;

// 进世界可选包开关（默认 0 = 最小集合，只发定位/能力/区块/指令树）
// bit0 默认出生点 0x61 | bit1 等待区块 game event 13 | bit2 天气同步
// bit3 玩家列表 0x46 | bit4 刷怪 | bit5 其他玩家 add_entity | bit6 TAB 页脚
static uint8_t g_join_mask = 0;
void mcJoinSetMask(uint8_t m) { g_join_mask = m; Serial.printf("[JOIN] mask=0x%02X\r\n", (unsigned)m); }

// 最近发出的包（id + 声明长度），掉线时打印最后 12 条，用来定位“xx bytes extra”
static uint8_t  s_sent_id[16];
static uint16_t s_sent_len[16];
static uint8_t  s_sent_head = 0;

static void pktLog_(uint8_t id, uint32_t len) {
  s_sent_id[s_sent_head] = id;
  s_sent_len[s_sent_head] = (uint16_t)(len > 0xFFFF ? 0xFFFF : len);
  s_sent_head = (uint8_t)((s_sent_head + 1) & 15);
}

static void pktDump_() {
  Serial.println("[PKT-TRAIL] last 12:");
  for (int k = 12; k >= 1; k--) {
    uint8_t i = (uint8_t)((s_sent_head - k) & 15);
    if (s_sent_id[i] == 0xFF && s_sent_len[i] == 0) continue;
    Serial.printf("[PKT-TRAIL] id=0x%02X len=%u\r\n", (unsigned)s_sent_id[i], (unsigned)s_sent_len[i]);
  }
}



// Set Entity Data 包 id 自校准（已用 26.1.2 真实脚包表确认，默认不再探测）
static const uint8_t kEDataCandidates[] = {0x63};  // 0x5C=set_border_warning_distance, 0x64=set_entity_link
static const uint8_t kEDataCandidateCount = (uint8_t)(sizeof(kEDataCandidates) / sizeof(kEDataCandidates[0]));
static uint8_t  s_edata_cand_idx = 0;
static uint8_t  s_edata_forced_id = 0xFF;   // 0 = 关闭探测
static uint8_t  s_edata_slot = 0xFF;
static unsigned long s_edata_ms = 0;
static uint16_t s_edata_acks = 0;
static bool s_edata_pending = false;
static bool s_edata_confirmed = true;   // 已确认 id，进世界不再发探包

void mcEntityDataSetId(uint8_t id) {
  s_edata_forced_id = id;
  s_edata_confirmed = false;
  s_edata_pending = false;
  if (id == 0) Serial.println("[EDATA] probe disabled");
  else { g_entity_data_id = id; Serial.printf("[EDATA] forced id=0x%02X\r\n", (unsigned)id); }
}
uint8_t mcEntityDataGetId() { return g_entity_data_id; }

void mcEntityDataPrintState() {
  Serial.printf("[EDATA] cand=%u/%u id=0x%02X confirmed=%d forced=0x%02X acks=%u\r\n",
                (unsigned)(s_edata_cand_idx + 1), (unsigned)kEDataCandidateCount,
                (unsigned)g_entity_data_id, s_edata_confirmed ? 1 : 0,
                (unsigned)s_edata_forced_id, (unsigned)s_edata_acks);
}

// ---- 串口调试：不开客户端也能验掉落物（寿命、坐标）----
static void (*s_dbg_add_drop)(uint16_t, uint8_t) = nullptr;
static void (*s_dbg_print_drops)() = nullptr;

void mcDebugHookDrops(void (*add_fn)(uint16_t, uint8_t), void (*print_fn)()) {
  s_dbg_add_drop = add_fn;
  s_dbg_print_drops = print_fn;
}
void mcDebugAddDrop(uint16_t item, uint8_t count) {
  if (!s_dbg_add_drop) { Serial.println("[DBG] drop hook 未注册"); return; }
  s_dbg_add_drop(item, count);
}
void mcDebugPrintDrops() {
  if (!s_dbg_print_drops) { Serial.println("[DBG] drop hook 未注册"); return; }
  s_dbg_print_drops();
}

// ---- 串口调试：模拟"对着 (x,y,z) 右键"，用来验工作台 / 铁砧 / 容器界面 / 方块放置 ----
static void (*s_dbg_use_block)(int16_t, int16_t, int16_t) = nullptr;
void mcDebugHookUseBlock(void (*use_fn)(int16_t, int16_t, int16_t)) {
  s_dbg_use_block = use_fn;
}
void mcDebugUseBlock(int16_t x, int16_t y, int16_t z) {
  if (!s_dbg_use_block) { Serial.println("[DBG] use hook 未注册"); return; }
  s_dbg_use_block(x, y, z);
}

// 最近一次死亡位置：死亡后重生时避开这里，否则复活点正好压在刚爆出来的物品上，
// 第一帧就全捡回去，看起来像"/kill 没爆掉落物"。
static int16_t s_die_x = 0;
static int16_t s_die_z = 0;
static uint32_t s_die_ms = 0;




#include "game_state.h"
#include "registries.h"
#include "terrain.h"
#include "procedures.h"
#include "crafting.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// 每个连接最近一次“成功放下方块”的时间，用来给放置动作限速：
// 一次右键客户端可能连发好几个 use_item_on 包，旧代码照单全收 → 一下放好几块。
static uint32_t s_last_place_ms[MAX_PLAYERS] = {0};
static uint32_t s_stuck_since_ms[MAX_PLAYERS] = {0};   // 连续卡在实心方块里的起始时间
// 组件读取的前置声明（0x38 创造模式槽早于定义处就要用它）
static bool readOneComponent_(PacketCodec& c, int32_t id, char* name_out, size_t name_len,
                              uint8_t* ench_out, uint8_t* ench_id_out = nullptr);

// ============================================================
// 指令树（Clientbound Commands / declare commands）
// 这个包的 id 在 1.21.x 各版本里会挪位，所以做成自校准：
// 每次有玩家进来先试一个候选 id；如果客户端 6 秒内掉线就当猜错，
// 下次连接自动换下一个候选；收到 4 个包还活着就认为猜对并记住。
// ============================================================
static const uint8_t kTreeCandidates[] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
static const uint8_t kTreeCandidateCount = (uint8_t)(sizeof(kTreeCandidates) / sizeof(kTreeCandidates[0]));

static uint8_t s_tree_cand_idx = 0;       // 当前在试第几个候选
static uint8_t s_tree_cur_id = 0x10;      // 本次实际使用的 id
static bool    s_tree_confirmed = false;  // 本 boot 已经确认
static uint8_t s_tree_forced_id = 0xFF;   // 串口强制指定（0 = 关闭）
static bool    s_tree_pending = false;    // 刚发出去，还在观察
static uint8_t s_tree_slot = 255;
static unsigned long s_tree_ms = 0;
static uint16_t s_tree_acks = 0;


#ifdef _WIN32
#include "win_platform.h"
#include "win_network_layer.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <esp_timer.h>
#endif

namespace {
const char* VERSION_NAME = "26.1.2";
const int PROTOCOL_VERSION = 775;
const int ACTIVE_VIEW_DISTANCE = VIEW_DISTANCE;

#ifdef _WIN32
int64_t get_time_us() { return esp_timer_get_time_win(); }
#else
int64_t get_time_us() { return (int64_t)esp_timer_get_time(); }
#endif
}  // namespace

// ============================================================
// 构造 / 初始化
// ============================================================

static uint8_t clientSlotToServerSlot_(uint16_t cslot);

MinecraftServer::MinecraftServer(uint16_t port) : network_(port), last_tick_time_us_(0) {
  for (uint8_t i = 0; i < kMaxClients; ++i) {
    clients_[i].fd = -1;
    clients_[i].used = false;
    clients_[i].state = STATE_NONE;
    clients_[i].player_index = -1;
    clients_[i].chunk_queue_idx = -1;
    clients_[i].edge_queue_count = 0;
    clients_[i].edge_queue_idx = 0;
    clients_[i].chunk_interval_ms = 80;
    clients_[i].chunk_send_start_ms = 0;
    clients_[i].chunk_slow_count = 0;
    clients_[i].chest_buf = nullptr;
    clients_[i].chest_open = false;
    clients_[i].chest_x = 0;
    clients_[i].chest_y = 0;
    clients_[i].chest_z = 0;
  }
}

bool MinecraftServer::begin() {
  // 药水记录：255 = 该格没药水

  // 掉落物池放堆：24 × sizeof(DropItem) ≈ 700B，放 .bss 会把 DRAM 顶爆
  if (!drops_) {
    drops_ = (DropItem*)calloc(kMaxDrops, sizeof(DropItem));
    if (!drops_) Serial.println("[DROP] 掉落物池分配失败，掉落物功能关闭");
  }

  // 一次性重置存档：世界改动 / 玩家位置与背包 / 附魔 / 掉落物映射，全清一遍，跑完写标记不再重复
  SPIFFS.begin(true);
  if (!SPIFFS.exists("/reset.done")) {
    SPIFFS.remove("/w.log");        SPIFFS.remove("/w.log.bak");   SPIFFS.remove("/w.log.tmp");
    SPIFFS.remove("/w.seed");       SPIFFS.remove("/w.seed.tmp");  SPIFFS.remove("/w.state");
    for (int ri = 0; ri < 32; ri++) {
      char fp[16];
      snprintf(fp, sizeof(fp), "/p%d.dat", ri); SPIFFS.remove(fp);
      snprintf(fp, sizeof(fp), "/q%d.dat", ri); SPIFFS.remove(fp);
      snprintf(fp, sizeof(fp), "/e%d.dat", ri); SPIFFS.remove(fp);
    }
    File rf = SPIFFS.open("/reset.done", FILE_WRITE);
    if (rf) { rf.print("1"); rf.close(); }
    Serial.println("[RESET] world + players + items cleared");
  }  menuBuffersInit();
  worldStoreBegin();                       // 挂载 SPIFFS + 读回种子
  if (!worldStoreSeedLoaded()) {
    world_seed = (uint32_t)splitmix64(world_seed);
    rng_seed = (uint32_t)splitmix64(rng_seed);
    worldStoreSaveSeed();                  // 首次开机：固定下来，以后世界不变
  }
  Serial.printf("World seed: %08X, RNG seed: %08X\n", world_seed, rng_seed);

  for (int i = 0; i < MAX_BLOCK_CHANGES; i++) block_changes[i].block = 0xFF;
  worldStoreReplayChanges();               // 回放玩家改过的方块
  worldStoreLoadPlayers();                 // 载入背包/坐标/血量
  for (int i = 0; i < MAX_PLAYERS; i++) player_data[i].client_fd = -1;

  // 注册槽位同步回调
  g_sync_slot_cb = [](int fd, int slot, uint8_t count, uint16_t item) {
    // fd 是 slot_idx，通过映射表取真实 socket fd
    if (fd < 0 || fd >= MAX_PLAYERS) return;
    int real_fd = g_slot_fd_map[fd];
    if (real_fd < 0) return;
    PacketCodec pc(real_fd);
    uint16_t client_slot = (uint16_t)serverSlotToClientSlot(0, (uint8_t)slot);
    uint32_t pkt_len = 1 + pc.sizeVarInt(0) + 1 + 2 + pc.sizeVarInt(count) +
                       (count > 0 ? pc.sizeVarInt(item) + 2 : 0);
    pc.writeVarInt(pkt_len); pc.writeByte(0x14);
    pc.writeVarInt(0); pc.writeVarInt(0); pc.writeUint16(client_slot);
    pc.writeVarInt(count);
    if (count > 0) { pc.writeVarInt(item); pc.writeVarInt(0); pc.writeVarInt(0); }
  };

  last_tick_time_us_ = get_time_us();
  return network_.begin();
}

// ============================================================
// 主循环
// ============================================================

void MinecraftServer::poll() {
  network_.poll();
  if (!network_.connected()) return;

  acceptClient_();

  int64_t now = get_time_us();
  if (now - last_tick_time_us_ > TIME_BETWEEN_TICKS) {
    handleServerTick_();
    last_tick_time_us_ = now;
  }

  static int64_t last_mem_print_us_ = 0;
#ifndef _WIN32
  if (now - last_mem_print_us_ > 1000000) {
    Serial.printf("[heap] free: %u internal: %u\n", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    last_mem_print_us_ = now;
  }
#endif

  for (uint8_t i = 0; i < kMaxClients; ++i) {
    if (!clients_[i].used) continue;
    serviceClient_(i);
  }

  // ============================================================
  // 自适应区块发送调度
  // 原则:
  //   1. play 包已在上面 serviceClient_ 里优先处理完毕
  //   2. 每个客户端有独立的 chunk_interval_ms (10~500ms)
  //      - 发块耗时 < interval/2  → 加速 (interval *= 0.85, 最低 10ms)
  //      - 发块耗时 > interval    → 减速 (interval *= 1.5,  最高 500ms)
  //      - 连续 3 次超时          → 额外退避到 300ms
  //   3. 轮转: 每帧最多给 1 个客户端发 1 个区块, 保证 play 包响应不被阻塞
  // ============================================================
  static uint8_t chunk_send_turn = 0;

  auto sendOneChunk = [&](uint8_t i) -> bool {
    ClientSlot& cs = clients_[i];
    if (!cs.used || cs.fd < 0) return false;

    uint32_t now_ms = (uint32_t)(now / 1000);

    // 还没到发送时间 → 跳过
    if ((int32_t)(now_ms - cs.chunk_next_send_ms) < 0) return false;

    bool sent = false;
    cs.chunk_send_start_ms = now_ms;

    if (cs.chunk_queue_idx >= 0) {
      processDeferredChunks_(i);
      sent = true;
    } else if (cs.edge_queue_idx < cs.edge_queue_count) {
      PacketCodec pc(cs.fd);
      int8_t ei = cs.edge_queue_idx;
      sendChunkDataAndUpdateLight_(pc, cs.edge_chunks_x[ei], cs.edge_chunks_z[ei]);
      cs.edge_queue_idx++;
      sent = true;
    }

    if (sent) {
      uint32_t elapsed = (uint32_t)(millis() - cs.chunk_send_start_ms);

      // 自适应调整间隔
      if (elapsed < cs.chunk_interval_ms / 2) {
        // 发送很快 → 加速
        cs.chunk_interval_ms = (uint16_t)(cs.chunk_interval_ms * 85 / 100);
        if (cs.chunk_interval_ms < 10) cs.chunk_interval_ms = 10;
        if (cs.chunk_slow_count > 0) cs.chunk_slow_count--;
      } else if (elapsed > cs.chunk_interval_ms) {
        // 发送慢 → 退避
        cs.chunk_slow_count++;
        if (cs.chunk_slow_count >= 3) {
          cs.chunk_interval_ms = 300;
          cs.chunk_slow_count = 0;
        } else {
          cs.chunk_interval_ms = (uint16_t)(cs.chunk_interval_ms * 150 / 100);
          if (cs.chunk_interval_ms > 500) cs.chunk_interval_ms = 500;
        }
      }

      cs.chunk_next_send_ms = millis() + cs.chunk_interval_ms;
    }
    return sent;
  };

#ifdef _WIN32
  for (uint8_t attempt = 0; attempt < kMaxClients; attempt++) {
    uint8_t i = (chunk_send_turn + attempt) % kMaxClients;
    if (sendOneChunk(i)) {
      chunk_send_turn = (i + 1) % kMaxClients;
      break;
    }
  }
#else
  // 内存分级保护:
  //   > 90KB: 正常发块
  //   60~90KB: 降速发块 (强制 interval >= 200ms)
  //   < 60KB: 完全停止发块, 让 lwIP 释放 pbuf
  uint32_t free_mem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (free_mem > 90000) {
    for (uint8_t attempt = 0; attempt < kMaxClients; attempt++) {
      uint8_t i = (chunk_send_turn + attempt) % kMaxClients;
      if (sendOneChunk(i)) {
        chunk_send_turn = (i + 1) % kMaxClients;
        break;
      }
    }
  } else if (free_mem > 60000) {
    // 内存偏低: 强制所有客户端降速
    for (uint8_t i = 0; i < kMaxClients; i++) {
      if (clients_[i].chunk_interval_ms < 200)
        clients_[i].chunk_interval_ms = 200;
    }
    for (uint8_t attempt = 0; attempt < kMaxClients; attempt++) {
      uint8_t i = (chunk_send_turn + attempt) % kMaxClients;
      if (sendOneChunk(i)) {
        chunk_send_turn = (i + 1) % kMaxClients;
        break;
      }
    }
  } else {
    // 内存危险: 完全停止发块, 让 lwIP 栈处理 ACK 释放 pbuf
    vTaskDelay(20);
  }
#endif
}

// ============================================================
// 连接管理
// ============================================================

#ifndef _WIN32
// 保持 WiFiClient 引用防止析构关闭 fd
static WiFiClient kept_clients[MAX_PLAYERS];
#endif

bool MinecraftServer::acceptClient_() {
#ifdef _WIN32
  SOCKET new_sock = network_.acceptClient();
  if (new_sock == INVALID_SOCKET) return false;
  int new_fd = (int)new_sock;
#else
  WiFiClient client = network_.accept();
  if (!client) return false;

  int new_fd = client.fd();
  if (new_fd < 0) { client.stop(); return false; }

  int flags = fcntl(new_fd, F_GETFL, 0);
  if (flags >= 0) fcntl(new_fd, F_SETFL, flags | O_NONBLOCK);
#endif

  for (uint8_t i = 0; i < kMaxClients; ++i) {
    if (clients_[i].used) continue;
    clients_[i].fd = new_fd;
    clients_[i].state = STATE_NONE;
    clients_[i].used = true;
    clients_[i].config_received_info = false;
    clients_[i].config_received_packs = false;
    clients_[i].player_index = -1;
    clients_[i].chunk_queue_idx = -1;
    clients_[i].edge_queue_count = 0;
    clients_[i].edge_queue_idx = 0;
    clients_[i].chunk_interval_ms = 80;
    clients_[i].chunk_send_start_ms = 0;
    clients_[i].chunk_slow_count = 0;
    memset(clients_[i].uuid, 0, 16);
    memset(clients_[i].name, 0, 16);
    g_slot_fd_map[i] = new_fd;
    client_count++;
#ifndef _WIN32
    kept_clients[i] = client;  // 保持引用, 防止 fd 被关闭
#endif
    return true;
  }
#ifdef _WIN32
  closesocket(new_sock);
#else
  client.stop();
#endif
  return false;
}

void MinecraftServer::serviceClient_(uint8_t slot_index) {
  ClientSlot& slot = clients_[slot_index];
  if (slot.fd < 0) { closeClient_(slot_index, 1); return; }

  uint8_t peek_buf[2];
#ifdef _WIN32
  int peek_n = recv((SOCKET)slot.fd, (char*)peek_buf, 2, MSG_PEEK);
  if (peek_n == 0) { closeClient_(slot_index, 1); return; }
  if (peek_n < 0) {
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) return;
    closeClient_(slot_index, 1);
    return;
  }
#else
  int peek_n = recv(slot.fd, peek_buf, 2, MSG_PEEK);
  if (peek_n == 0) { closeClient_(slot_index, 1); return; }
  if (peek_n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    closeClient_(slot_index, 1);
    return;
  }
#endif

  PacketCodec codec(slot.fd);
  int32_t packet_len = 0, packet_id = 0;
  if (!codec.readVarInt(packet_len) || !codec.readVarInt(packet_id)) {
    closeClient_(slot_index, 2);
    return;
  }

  int32_t payload_len = packet_len - codec.sizeVarInt((uint32_t)packet_id);
  if (payload_len < 0) payload_len = 0;
  // 这个包的读取上限：解析一旦错位，越界读直接失败，而不是卡在 recv 里等超时
  codec.setReadLimit(codec.readCount() + (size_t)payload_len + 1);
  bool ok = false;

  switch (slot.state) {
    case STATE_NONE: ok = handleHandshake_(slot, codec, packet_id); break;
    case STATE_STATUS: ok = handleStatus_(slot, codec, packet_id, payload_len); break;
    case STATE_LOGIN: ok = handleLogin_(slot, codec, packet_id, payload_len); break;
    case STATE_CONFIGURATION: ok = handleConfiguration_(slot, codec, packet_id, payload_len); break;
    case STATE_PLAY: ok = handlePlay_(slot, codec, packet_id, payload_len); break;
    default: ok = false; break;
  }

  if (!ok) closeClient_(slot_index, 4);
}

void MinecraftServer::closeClient_(uint8_t slot_index, int cause) {
  ClientSlot& slot = clients_[slot_index];
  if (!slot.used) return;

  if (slot.player_index >= 0) handlePlayerDisconnect_(slot_index);

#ifdef _WIN32
  if (slot.fd >= 0) closesocket((SOCKET)slot.fd);
#else
  // 释放 WiFiClient 引用 (这会关闭底层 fd)
  kept_clients[slot_index].stop();
  kept_clients[slot_index] = WiFiClient();
#endif
  slot.fd = -1;
  slot.state = STATE_NONE;
  slot.used = false;
  slot.player_index = -1;
  g_slot_fd_map[slot_index] = -1;
  if (client_count > 0) client_count--;

  Serial.printf("Client %d disconnected, cause: %d\n", slot_index, cause);
}

// ============================================================
// Handshake
// ============================================================

bool MinecraftServer::handleHandshake_(ClientSlot& slot, PacketCodec& codec, int32_t packet_id) {
  if (packet_id != 0x00) return false;
  int32_t protocol_version = 0;
  char server_address[256];
  uint16_t server_port = 0;
  int32_t next_state = 0;
  if (!codec.readVarInt(protocol_version)) return false;
  if (!codec.readString(server_address, sizeof(server_address))) return false;
  if (!codec.readUint16(server_port)) return false;
  if (!codec.readVarInt(next_state)) return false;
  slot.state = (uint8_t)next_state;
  return true;
}

// ============================================================
// Status
// ============================================================

bool MinecraftServer::handleStatus_(ClientSlot& slot, PacketCodec& codec, int32_t packet_id, int32_t packet_len) {
  if (packet_id == 0x00) return sendStatusResponse_(codec);
  if (packet_id == 0x01) {
    uint8_t payload[8];
    if (!codec.readExact(payload, 8)) return false;
    bool ok = codec.writeVarInt(9) && codec.writeVarInt(0x01) && codec.writeExact(payload, 8);
    closeClient_((uint8_t)(&slot - clients_), 8);
    return ok;
  }
  return codec.skipBytes((size_t)packet_len);
}

bool MinecraftServer::sendStatusResponse_(PacketCodec& codec) {
  char json[512];
  int len = snprintf(json, sizeof(json),
    "{\"version\":{\"name\":\"%s\",\"protocol\":%d},"
    "\"players\":{\"max\":%d,\"online\":%d},"
    "\"description\":{\"text\":\"ESP32MC Server\"}}",
    VERSION_NAME, PROTOCOL_VERSION, MAX_PLAYERS, onlineCount_());
  uint32_t json_len = (uint32_t)len;
  uint32_t pkt_len = 1 + codec.sizeVarInt(json_len) + json_len;
  return codec.writeVarInt(pkt_len) && codec.writeVarInt(0x00) &&
         codec.writeVarInt(json_len) && codec.writeExact((const uint8_t*)json, json_len);
}

// ============================================================
// Login
// ============================================================

bool MinecraftServer::handleLogin_(ClientSlot& slot, PacketCodec& codec, int32_t packet_id, int32_t packet_len) {
  if (packet_id == 0x00) {
    if (!codec.readString(slot.name, sizeof(slot.name))) return false;
    if (!codec.readExact(slot.uuid, 16)) return false;
    uint8_t slot_idx = (uint8_t)(&slot - clients_);
    if (reservePlayerData(slot_idx, slot.uuid, slot.name)) return false;
    PlayerData* p;
    if (getPlayerData(slot_idx, &p) == 0) slot.player_index = (int)(p - player_data);
    return sendLoginSuccess_(codec, slot.uuid, slot.name);
  }
  if (packet_id == 0x03) {
    slot.state = STATE_CONFIGURATION;
    if (!sendBrand_(codec)) return false;
    if (!sendEnabledFeatures_(codec)) return false;
    return sendKnownPacks_(codec);
  }
  return codec.skipBytes((size_t)packet_len);
}

bool MinecraftServer::sendLoginSuccess_(PacketCodec& codec, const uint8_t uuid[16], const char* name) {
  uint32_t name_len = (uint32_t)strlen(name);
  uint32_t pkt_len = 1 + 16 + codec.sizeVarInt(name_len) + name_len + 1;
  return codec.writeVarInt(pkt_len) && codec.writeVarInt(0x02) &&
         codec.writeExact(uuid, 16) && codec.writeVarInt(name_len) &&
         codec.writeExact((const uint8_t*)name, name_len) && codec.writeVarInt(0);
}

// ============================================================
// Configuration
// ============================================================

bool MinecraftServer::handleConfiguration_(ClientSlot& slot, PacketCodec& codec, int32_t packet_id, int32_t packet_len) {
  if (packet_id == 0x00) { slot.config_received_info = true; return consumeClientInformation_(codec); }
  if (packet_id == 0x02) return consumePluginMessage_(codec, packet_len);
  if (packet_id == 0x07) {
    if (!consumeKnownPacks_(codec)) return false;
    slot.config_received_packs = true;
    if (!sendRegistries_(codec)) return false;
    return sendFinishConfiguration_(codec);
  }
  if (packet_id == 0x03) {
    uint8_t slot_idx = (uint8_t)(&slot - clients_);
    slot.state = STATE_PLAY;
    if (!sendLoginPlay_(codec, slot_idx)) return false;
    // 指令树：让客户端的 / 补全能认出 gamemode / tp / help
    // 注意：指令树是“每条连接”一份的，客户端每次进服都要重新收，所以确认过 id 之后
    // 仍然必须每次发（早先只在未确认时发，导致第二次进服 / 补全整个消失）。
    uint8_t tree_id = (s_tree_forced_id != 0xFF) ? s_tree_forced_id
                     : (s_tree_confirmed ? s_tree_cur_id : kTreeCandidates[s_tree_cand_idx]);
    s_tree_cur_id = tree_id;
    if (tree_id != 0) {
      if (!sendCommandTree_(codec, tree_id)) return false;
      if (!s_tree_confirmed) {
        s_tree_pending = true;
        s_tree_slot = slot_idx;
        s_tree_ms = millis();
        s_tree_acks = 0;
        Serial.printf("[CMD] declare_commands try id=0x%02X (cand %u/%u)\r\n",
                      (unsigned)tree_id, (unsigned)(s_tree_cand_idx + 1), (unsigned)kTreeCandidateCount);
      } else {
        Serial.printf("[CMD] declare_commands resend id=0x%02X (per-connection tree)\r\n",
                      (unsigned)tree_id);
      }
    }
    spawnPlayer_(slot_idx);
    Serial.printf("[JOIN] slot=%u minimal join mask=0x%02X\r\n", (unsigned)slot_idx, (unsigned)g_join_mask);
    // Set Entity Data id 自校准探包
    if (!s_edata_confirmed && s_edata_forced_id != 0) {
      g_entity_data_id = (s_edata_forced_id != 0xFF) ? s_edata_forced_id : kEDataCandidates[s_edata_cand_idx];
      if (!sendEntityDataProbe_(codec, slot_idx)) return false;
      s_edata_pending = true;
      s_edata_slot = slot_idx;
      s_edata_ms = millis();
      s_edata_acks = 0;
      Serial.printf("[EDATA] try id=0x%02X (cand %u/%u)\r\n", (unsigned)g_entity_data_id,
                    (unsigned)(s_edata_cand_idx + 1), (unsigned)kEDataCandidateCount);
    }
    return true;
  }
  return codec.skipBytes((size_t)packet_len);
}

// ============================================================
// Play 状态收包
// ============================================================

bool MinecraftServer::handlePlay_(ClientSlot& slot, PacketCodec& codec, int32_t packet_id, int32_t packet_len) {
  uint8_t slot_idx = (uint8_t)(&slot - clients_);
  PlayerData* player = nullptr;
  if (slot.player_index >= 0) player = &player_data[slot.player_index];

  if (s_edata_pending && slot_idx == s_edata_slot) {
    if (s_edata_acks < 0xFFFF) s_edata_acks++;
    if (s_edata_acks >= 4) {
      s_edata_pending = false;
      s_edata_confirmed = true;
      Serial.printf("[EDATA] id=0x%02X OK\r\n", (unsigned)g_entity_data_id);
    }
  }

  if (s_tree_pending && slot_idx == s_tree_slot) {
    if (s_tree_acks < 0xFFFF) s_tree_acks++;
    if (s_tree_acks >= 4) {
      s_tree_pending = false;
      s_tree_confirmed = true;
      Serial.printf("[CMD] declare_commands id=0x%02X OK\r\n", (unsigned)s_tree_cur_id);
    }
  }

  mcBrMark((uint8_t)packet_id);
  switch (packet_id) {
    case 0x1C: // Keep Alive
    case 0x0B: // Chunk batch
    case 0x0D: // Client tick
      return codec.skipBytes((size_t)packet_len);

    case 0x1E: // Set Player Position
    case 0x1F: // Set Player Position And Rotation
    case 0x20: // Set Player Rotation
    case 0x21: { // Set Player Movement Flags
      if (!player) return codec.skipBytes((size_t)packet_len);
      double x = 0, y = 0, z = 0;
      float yaw = 0, pitch = 0;

      if (packet_id == 0x1E || packet_id == 0x1F) {
        double dx, dy, dz;
        if (!codec.readDouble(dx) || !codec.readDouble(dy) || !codec.readDouble(dz)) return false;
        x = dx; y = dy; z = dz;
      }
      if (packet_id == 0x1F || packet_id == 0x20) {
        float fy, fp;
        if (!codec.readFloat(fy) || !codec.readFloat(fp)) return false;
        yaw = fy; pitch = fp;
      }
      uint8_t flags_byte;
      if (!codec.readByte(flags_byte)) return false;
      uint8_t on_ground = flags_byte & 0x01;

      // 摔落伤害（鞘翅滑翔中不结算；一落地就结束滑翔）
      if (on_ground && packet_id != 0x20) {
        int gpi = slotIndexForPlayer_(player);
        if (gpi >= 0 && gpi < MAX_PLAYERS && g_gliding[gpi]) {
          g_gliding[gpi] = 0;
          broadcastPlayerMetadata_(player);
          Serial.printf("[ELYTRA] glide end (landed y=%u)\r\n", (unsigned)player->y);
        } else {
          int16_t damage = player->grounded_y - player->y - 3;
          if (damage > 0 && (player->game_mode == 0 || player->game_mode == 2))
            hurtEntity_(slot_idx, -1, D_fall, (uint8_t)damage);
        }
        player->grounded_y = player->y;
      }

      if (packet_id == 0x21) return true;

      // 更新朝向
      if (packet_id != 0x1E) {
        player->yaw = ((int16_t)(yaw + 540) % 360 - 180) * 127 / 180;
        player->pitch = (int8_t)(pitch / 90.0f * 127.0f);
      }

      if (packet_id == 0x20) {
        for (uint8_t i = 0; i < kMaxClients; i++) {
          if (!clients_[i].used || clients_[i].state != STATE_PLAY || i == slot_idx) continue;
          if (clients_[i].player_index < 0) continue;
          PacketCodec oc(clients_[i].fd);
          sendUpdateEntityRotation_(oc, slot_idx, player->yaw, player->pitch);
          sendSetHeadRotation_(oc, slot_idx, player->yaw);
        }
        return true;
      }

      // 更新坐标
      int16_t cx = (int16_t)x, cy = (int16_t)y, cz = (int16_t)z;
      int16_t prev_chunk_x = div_floor(player->x, 16);
      int16_t prev_chunk_z = div_floor(player->z, 16);
      player->x = cx; player->y = (uint8_t)cy; player->z = cz;
      { int ps = slotIndexForPlayer_(player); if (ps >= 0) fixPlayerEmbedded_((uint8_t)ps, player); }
      // 位置变动诊断：每 30 个位移包打印一次，用来确认客户端真的在走（不是卡死）
      static uint16_t pos_log_n = 0;
      if ((++pos_log_n % 30) == 0)
        Serial.printf("[POS] %d,%u,%d yaw=%d pitch=%d ground=%u\r\n",
                      (int)player->x, (unsigned)player->y, (int)player->z,
                      (int)player->yaw, (int)player->pitch, (unsigned)on_ground);

      // 饥饿消耗
      if (player->saturation == 0) {
        if (player->hunger > 0) player->hunger--;
        player->saturation = 200;
        PacketCodec hpc(slot.fd);
        sendSetHealth_(hpc, player->health, player->hunger, player->saturation);
      } else if (player->flags & 0x08) {
        player->saturation -= 1;
      }

      // 广播位置
      for (uint8_t i = 0; i < kMaxClients; i++) {
        if (!clients_[i].used || clients_[i].state != STATE_PLAY || i == slot_idx) continue;
        if (clients_[i].player_index < 0) continue;
        PacketCodec oc(clients_[i].fd);
        sendTeleportEntity_(oc, slot_idx, x, y, z, yaw, pitch);
        sendSetHeadRotation_(oc, slot_idx, player->yaw);
      }

      // 跨区块时发新边缘区块
      int16_t new_chunk_x = div_floor(cx, 16), new_chunk_z = div_floor(cz, 16);
      if (new_chunk_x != prev_chunk_x || new_chunk_z != prev_chunk_z) {
        PacketCodec pc(slot.fd);
        sendStartWaitingForChunks_(pc);
        sendSetCenterChunk_(pc, new_chunk_x, new_chunk_z);
        int16_t dx = new_chunk_x - prev_chunk_x;
        int16_t dz = new_chunk_z - prev_chunk_z;

        // 把未发完的区块往前压缩（丢弃已发的）
        int remaining = slot.edge_queue_count - slot.edge_queue_idx;
        if (remaining > 0 && slot.edge_queue_idx > 0) {
          for (int j = 0; j < remaining; j++) {
            slot.edge_chunks_x[j] = slot.edge_chunks_x[slot.edge_queue_idx + j];
            slot.edge_chunks_z[j] = slot.edge_chunks_z[slot.edge_queue_idx + j];
          }
        } else if (remaining <= 0) {
          remaining = 0;
        }
        slot.edge_queue_count = (int8_t)remaining;
        slot.edge_queue_idx = 0;

        // 追加新边缘区块（去重）
        auto enqueue = [&](int16_t ex, int16_t ez) {
          if (slot.edge_queue_count >= 50) return;
          // 去重
          for (int j = 0; j < slot.edge_queue_count; j++)
            if (slot.edge_chunks_x[j] == ex && slot.edge_chunks_z[j] == ez) return;
          slot.edge_chunks_x[slot.edge_queue_count] = ex;
          slot.edge_chunks_z[slot.edge_queue_count] = ez;
          slot.edge_queue_count++;
        };

        if (dx != 0) {
          int16_t edge_x = new_chunk_x + (dx > 0 ? ACTIVE_VIEW_DISTANCE : -ACTIVE_VIEW_DISTANCE);
          for (int j = -ACTIVE_VIEW_DISTANCE; j <= ACTIVE_VIEW_DISTANCE; j++)
            enqueue(edge_x, new_chunk_z + j);
        }
        if (dz != 0) {
          int16_t edge_z = new_chunk_z + (dz > 0 ? ACTIVE_VIEW_DISTANCE : -ACTIVE_VIEW_DISTANCE);
          for (int j = -ACTIVE_VIEW_DISTANCE; j <= ACTIVE_VIEW_DISTANCE; j++)
            enqueue(new_chunk_x + j, edge_z);
        }
        trySpawnMobNearPlayer_(player);
      }
      return true;
    }

    case 0x29: { // Player Action (mining)
      if (!player) return codec.skipBytes((size_t)packet_len);
      uint8_t action; if (!codec.readByte(action)) return false;
      uint64_t pos_raw; if (!codec.readUint64(pos_raw)) return false;
      int32_t bx = (int32_t)(pos_raw >> 38);
      int32_t by = (int32_t)((pos_raw << 52) >> 52);
      int32_t bz = (int32_t)((pos_raw << 26) >> 38);
      uint8_t face; if (!codec.readByte(face)) return false;
      int32_t sequence; if (!codec.readVarInt(sequence)) return false;
      PacketCodec pc(slot.fd);
      sendAcknowledgeBlockChange_(pc, sequence);
      handlePlayerAction_(player, action, (int16_t)bx, (int16_t)by, (int16_t)bz);
      return true;
    }

    case 0x42: { // Use Item On
      if (!player) return codec.skipBytes((size_t)packet_len);
      uint8_t hand; if (!codec.readByte(hand)) return false;
      uint64_t pos_raw; if (!codec.readUint64(pos_raw)) return false;
      int32_t bx = (int32_t)(pos_raw >> 38);
      int32_t by = (int32_t)((pos_raw << 52) >> 52);
      int32_t bz = (int32_t)((pos_raw << 26) >> 38);
      uint8_t face; if (!codec.readByte(face)) return false;
      codec.skipBytes(12 + 2); // cursor + inside_block + world_border
      int32_t sequence; if (!codec.readVarInt(sequence)) return false;
      PacketCodec pc(slot.fd);
      sendAcknowledgeBlockChange_(pc, sequence);
      handlePlayerUseItem_(player, (int16_t)bx, (int16_t)by, (int16_t)bz, face);
      return true;
    }

    case 0x35: { // Set Held Item
      if (!player) return codec.skipBytes((size_t)packet_len);
      uint16_t held_slot; if (!codec.readUint16(held_slot)) return false;
      if (held_slot < 9) player->hotbar = (uint8_t)held_slot;
      return true;
    }

    case 0x09: { // Chat
      if (!player) return codec.skipBytes((size_t)packet_len);
      char msg[225]; if (!codec.readString(msg, sizeof(msg))) return false;
      codec.skipBytes(8 + 8); // timestamp + salt
      uint8_t has_sig; codec.readByte(has_sig);
      if (has_sig) codec.skipBytes(256);
      int32_t msg_count; codec.readVarInt(msg_count);
      codec.skipBytes(4); // ack bitmap

      if (msg[0] == '/' || msg[0] == '!') {
        if (runCommand_(slot_idx, player, msg + 1)) return true;
        if (msg[0] == '/') {
          PacketCodec uc(slot.fd);
          sendSystemChat_(uc, "Unknown command. Try /gamemode or /tp", 37);
          return true;
        }
      }

      if (msg[0] == '!') {
        PacketCodec pc(slot.fd);
        if (strncmp(msg, "!help", 5) == 0) {
          sendSystemChat_(pc, "Commands: /gamemode <creative|survival> [player]", 47);
          sendSystemChat_(pc, "          /tp <x> <y> <z>   or   /tp <player>", 43);
          sendSystemChat_(pc, "          /kill   or   !kill <player>", 32);
          sendSystemChat_(pc, "          /time set day|noon|night|midnight   /time add <t>", 56);
          sendSystemChat_(pc, "          /weather clear|rain|thunder", 33);
          sendSystemChat_(pc, "          !help, !msg <player> <message>, !summon <mob> [n]", 55);
          sendSystemChat_(pc, "Mobs: chicken cow pig sheep zombie", 34);
        } else if (strncmp(msg, "!summon ", 8) == 0) {
          char* args = msg + 8;
          while (*args == ' ') args++;
          // 解析 mob 名和可选数量
          char mob_name[32] = {};
          int count = 1;
          char* space = strchr(args, ' ');
          if (space) {
            int nlen = (int)(space - args);
            if (nlen > 31) nlen = 31;
            memcpy(mob_name, args, nlen);
            count = atoi(space + 1);
            if (count < 1) count = 1;
            if (count > 10) count = 10;
          } else {
            strncpy(mob_name, args, 31);
          }
          uint8_t type = 0; uint8_t hp = 10;
          if      (strcmp(mob_name, "chicken") == 0) { type = 26;  hp = 4;  }
          else if (strcmp(mob_name, "cow")     == 0) { type = 30;  hp = 10; }
          else if (strcmp(mob_name, "pig")     == 0) { type = 100; hp = 10; }
          else if (strcmp(mob_name, "sheep")   == 0) { type = 111; hp = 8;  }
          else if (strcmp(mob_name, "zombie")  == 0) { type = 150; hp = 20; }
          else if (strcmp(mob_name, "skeleton")== 0) { type = 115; hp = 20; }
          else if (strcmp(mob_name, "spider")  == 0) { type = 124; hp = 16; }
          else if (strcmp(mob_name, "creeper") == 0) { type = 32;  hp = 20; }
          // 支持直接输入数字 ID 测试
          else if (mob_name[0] >= '0' && mob_name[0] <= '9') { type = (uint8_t)atoi(mob_name); hp = 20; }
          if (type == 0) {
            sendSystemChat_(pc, "Unknown mob. Try: chicken cow pig sheep zombie skeleton spider creeper", 67);
          } else {
            int spawned = 0;
            for (int n = 0; n < count; n++) {
              // 在玩家附近随机偏移生成
              uint32_t r = fast_rand();
              int16_t sx = player->x + (int16_t)((r & 7) - 3);
              int16_t sz = player->z + (int16_t)(((r >> 4) & 7) - 3);
              uint8_t sy = player->y;
              // 找合适的 y
              for (int t = 0; t < 10; t++) {
                if (!isPassableBlock(getBlockAt(sx, sy - 1, sz)) &&
                     isPassableBlock(getBlockAt(sx, sy,     sz)) &&
                     isPassableBlock(getBlockAt(sx, sy + 1, sz))) break;
                sy++;
              }
              spawnMob(type, sx, sy, sz, hp);
              broadcastMobSpawn_(type, sx, sy, sz);
              spawned++;
            }
            char out[64];
            int ol = snprintf(out, sizeof(out), "Spawned %d %s", spawned, mob_name);
            sendSystemChat_(pc, out, (uint16_t)ol);
          }
        } else if (strncmp(msg, "!edata ", 7) == 0) {
          uint32_t v = (uint32_t)strtoul(msg + 7, nullptr, 0);
          mcEntityDataSetId((uint8_t)v);
          char out[48];
          int ol = snprintf(out, sizeof(out), "edata=0x%02X", g_entity_data_id);
          sendSystemChat_(pc, out, (uint16_t)ol);
        } else if (strncmp(msg, "!testitem", 9) == 0) {
          int16_t dx = 3, dz = 0;
          if (player->yaw >= 32 && player->yaw < 96) { dx = 3; dz = 0; }
          // 注意：这里必须用物品 id（I_stone=1），不能用方块 id（B_stone=25）。
          // 之前传 B_stone 会把 minecraft:chiseled_tuff_bricks 塞进 item stack。
          addDrop_((int16_t)(player->x + dx), player->y, (int16_t)(player->z + dz), I_stone, 1);
          char out[64];
          int ol = snprintf(out, sizeof(out), "testitem edata=0x%02X", g_entity_data_id);
          sendSystemChat_(pc, out, (uint16_t)ol);
        } else if (strncmp(msg, "!msg ", 5) == 0) {
          char* target_start = msg + 5;
          while (*target_start == ' ') target_start++;
          char* space = strchr(target_start, ' ');
          if (!space) { sendSystemChat_(pc, "Usage: !msg <player> <message>", 30); return true; }
          *space = '\0';
          char* text = space + 1;
          PlayerData* target = nullptr;
          for (int i = 0; i < MAX_PLAYERS; i++) {
            if (player_data[i].client_fd == -1) continue;
            if (strcmp(player_data[i].name, target_start) == 0) { target = &player_data[i]; break; }
          }
          if (!target) { sendSystemChat_(pc, "Player not found", 16); return true; }
          int ti = slotIndexForPlayer_(target);
          if (ti >= 0) {
            char whisper[256];
            int wl = snprintf(whisper, sizeof(whisper), "\xC2\xA77\xC2\xA7o%s whispers to you: %s", player->name, text);
            PacketCodec tc(clients_[ti].fd);
            sendSystemChat_(tc, whisper, (uint16_t)wl);
          }
          char reply[256];
          int rl = snprintf(reply, sizeof(reply), "\xC2\xA77\xC2\xA7oYou whisper to %s: %s", target_start, text);
          sendSystemChat_(pc, reply, (uint16_t)rl);
        } else {
          sendSystemChat_(pc, "Unknown command. Try !help", 26);
        }
      } else {
        char buf[256];
        int blen = snprintf(buf, sizeof(buf), "<%s> %s", player->name, msg);
        for (uint8_t i = 0; i < kMaxClients; i++) {
          if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
          PacketCodec oc(clients_[i].fd);
          sendSystemChat_(oc, buf, (uint16_t)blen);
        }
      }
      return true;
    }

    case 0x06: // Chat Command（无签名，客户端按 / 时最常发这个）
    case 0x07: { // Chat Command Signed
      if (!player) return codec.skipBytes((size_t)packet_len);
      char cmd[200];
      if (!codec.readString(cmd, sizeof(cmd))) return true;
      if (cmd[0] >= 'a' && cmd[0] <= 'z' && cmd[1] != '\0') {
        if (!runCommand_(slot_idx, player, cmd)) {
          PacketCodec uc(slot.fd);
          sendSystemChat_(uc, "Unknown command. Try /gamemode or /tp", 37);
        }
      }
      return true;
    }

    case 0x2C: { // Player Loaded
      if (!player) return codec.skipBytes((size_t)packet_len);
      handlePlayerJoin_(slot_idx);
      return true;
    }

    case 0x3F: { // Swing Arm
      if (!player) return codec.skipBytes((size_t)packet_len);
      int32_t hand; codec.readVarInt(hand);
      uint8_t animation = (hand == 0) ? 0 : 2;
      for (uint8_t i = 0; i < kMaxClients; i++) {
        if (!clients_[i].used || clients_[i].state != STATE_PLAY || i == slot_idx) continue;
        PacketCodec oc(clients_[i].fd);
        sendEntityAnimation_(oc, slot_idx, animation);
      }
      return true;
    }

    case 0x01: { // Attack
      if (!player) return codec.skipBytes((size_t)packet_len);
      int32_t entity_id; if (!codec.readVarInt(entity_id)) return false;
      hurtEntity_(entity_id, slot_idx, D_generic, 1);
      return true;
    }

    case 0x1A: { // Interact
      if (!player) return codec.skipBytes((size_t)packet_len);
      int32_t entity_id; if (!codec.readVarInt(entity_id)) return false;
      uint8_t type; if (!codec.readByte(type)) return false;
      if (type == 2) codec.skipBytes(12);
      if (type != 1) codec.skipBytes(1);
      codec.skipBytes(1);
      if (type == 1) hurtEntity_(entity_id, slot_idx, D_generic, 1);
      return true;
    }

    case 0x11: { // Container Button Click（附魔台三档按钮）
      int32_t wid = 0, btn = 0;
      codec.readVarInt(wid); codec.readVarInt(btn);
      ClientSlot& s2 = clients_[slot_idx];
      if (s2.player_index >= 0 && wid == 13) {
        PlayerData* pp = &player_data[s2.player_index];
        uint8_t lvl = (uint8_t)(btn + 1);
        if (lvl > 3) lvl = 3;
        mcDiag_("[ENCHBTN] slot=%u btn=%d item=%u cnt=%u lapis=%u/%u gm=%u",
                (unsigned)slot_idx, (int)btn, (unsigned)pp->craft_items[0],
                (unsigned)pp->craft_count[0], (unsigned)pp->craft_items[1],
                (unsigned)pp->craft_count[1], (unsigned)pp->game_mode);
        // 原版 EnchantmentMenu.clickMenuButton 的判定（javap 实证）：
        //   生存：槽 1 青金石数量 >= 档位(btn+1)，且等级够；扣青金石、扣等级
        //   创造（hasInfiniteMaterials）：两样都不需要
        // 之前这里写成“没有青金石也照样附”，所以出现了“不放青金石也能附魔”。
        // 用户明确要求：没有青金石就不能附（创造模式也一样），否则就是"白嫖附魔"。
        bool has_lapis = (pp->craft_items[1] == 901 && pp->craft_count[1] >= lvl);
        if (pp->craft_items[0] != 0 && pp->craft_count[0] > 0 && has_lapis) {
          if (pp->craft_items[1] == 901) {   // 901 = lapis_lazuli
            pp->craft_count[1] = (uint8_t)(pp->craft_count[1] - lvl);
            if (pp->craft_count[1] == 0) pp->craft_items[1] = 0;
          }
          // 原版：附魔台里放“书”会变成“附魔书”（transmuteCopy(ENCHANTED_BOOK)）
          if (pp->craft_items[0] == 1030) pp->craft_items[0] = I_enchanted_book;   // book -> enchanted_book
          // 原版行为：物品留在附魔台的输入格里、直接带上附魔，不会跳到背包。
          g_craft_ench[slot_idx][0] = lvl;
          PacketCodec ep(s2.fd);
          sendSetContainerSlotEnch_(ep, 13, 0, pp->craft_count[0], pp->craft_items[0], lvl);
          sendSetContainerSlot_(ep, 13, 1, pp->craft_count[1], pp->craft_items[1]);
          // 附魔后台面重算：费用(0..2) 归零，费用槽真的在 data 0/1/2
          sendSetContainerData_(ep, 13, 0, 0);
          sendSetContainerData_(ep, 13, 1, 0);
          sendSetContainerData_(ep, 13, 2, 0);
          sendSetExperience_(ep, 0.0f, (int32_t)(30 - lvl), (int32_t)(30 - lvl));
          if (s2.player_index >= 0) {
            metaRemember_(s2.player_index, pp->craft_items[0], lvl, nullptr);
            g_item_name[s2.player_index][41][0] = 0;
          }
          sendSystemChat_(ep, "Enchanted!", 10);
        } else if (pp->craft_items[0] != 0 && pp->craft_count[0] > 0) {
          PacketCodec ep2(s2.fd);
          sendSystemChat_(ep2, "Need lapis in slot 1", 22);
          mcDiag_("[ENCHBTN] rejected: need lapis x%u (slot1=%u cnt=%u)",
                  (unsigned)lvl, (unsigned)pp->craft_items[1], (unsigned)pp->craft_count[1]);
        }
      }
      return true;
    }
    case 0x30: { // Rename Item（铁砧改名框，26.1.2 serverbound id 48）
      int32_t nlen = 0; codec.readVarInt(nlen);
      ClientSlot& s3 = clients_[slot_idx];
      char nm[32]; uint16_t k = 0; nm[0] = 0;
      for (int32_t i = 0; i < nlen; i++) {
        uint8_t ch = 0; codec.readByte(ch);
        if (k < 31) nm[k++] = (char)ch;
      }
      nm[k] = 0;
      mcDiag_("[RENAME] pi=%d len=%ld text=%s", (int)s3.player_index, (long)nlen, nm);
      if (s3.player_index >= 0) {
        strlcpy(g_pending_name[s3.player_index], nm, sizeof(g_pending_name[0]));
        {
          PlayerData* p3 = &player_data[s3.player_index];
          uint8_t cur_lv = (slot_idx < kMaxClients) ? g_craft_ench[slot_idx][0] : 0;
          if (p3->craft_items[0] != 0 && (cur_lv > 0 || nm[0] != 0)) {
            metaRemember_(s3.player_index, p3->craft_items[0], cur_lv, (nm[0] != 0) ? nm : nullptr);
          }
        }
        // 不再自己往结果槽推一份“带名字的物品”：客户端的铁砧界面本来就会按改名框
        // 在本地算出结果，服务端再推一次会和客户端预测打架，之前就是这么掉线/卡死的。
        // 这里只记住名字，等玩家点结果时用。
      }
      return true;
    }
    case 0x12: { // Click Container
      pktTraceStart();
      bool cok = handleClickContainer_(slot_idx, codec, packet_len);
      diagTraceDump_();
      pktTraceStop();
      return cok;
    }

    case 0x13: { // Close Container
      if (!player) return codec.skipBytes((size_t)packet_len);
      int32_t window_id; codec.readVarInt(window_id);
      CarryItem cy_save = g_carry[slot_idx];    // 关窗时手上/格子里的那件东西，附魔和名字要跟着走
      // 窗口格发包缓存作废：下次开同一个界面必须无条件重发一次 0/1 格
      if (slot_idx < MAX_PLAYERS) { wsReset_((uint8_t)slot_idx); g_ws_win[slot_idx] = 0xFF; }
      mcDiag_("[CLOSEWIN] win=%d c0=%u/%u c1=%u/%u lv0=%u lv1=%u",
              (int)window_id, (unsigned)player->craft_items[0], (unsigned)player->craft_count[0],
              (unsigned)player->craft_items[1], (unsigned)player->craft_count[1],
              (unsigned)g_craft_ench[slot_idx][0], (unsigned)g_craft_ench[slot_idx][1]);
      if (window_id == 2 && slot.chest_open) {          // 关箱时把内容写进存档
        worldStoreSaveChest(slot.chest_x, slot.chest_y, slot.chest_z);
        slot.chest_open = false;
        slot.chest_buf = nullptr;
      }
      int pi_close = clients_[slot_idx].player_index;
      if (pi_close < 0) pi_close = 0;
      for (uint8_t i = 0; i < 9; i++) {
        // 熔炉的产物槽 i==2 里放的是“预览”（还没真正烧过），不能当成品塞回背包
        if (window_id != 2 && !(window_id == 14 && i == 2)) {
          uint16_t it = player->craft_items[i];
          uint8_t cn = player->craft_count[i];
          if (it != 0 && cn != 0) {
            // 附魔台/铁砧里还留在格子上的东西，退出界面时要连附魔/名字一起带回背包：
            // 以前只按 item id 塞进去，附魔只存在 g_craft_ench 里、背包那份是白的，
            // →“附魔台附完一关界面就变回普通剑”。
            uint8_t lv = g_craft_ench[slot_idx][i];
            uint8_t ench_id = (uint8_t)kEnchSharpnessId;
            const char* nm = (window_id == 8 && i == 0 && g_pending_name[pi_close][0])
                             ? g_pending_name[pi_close] : nullptr;
            if (cy_save.valid && cy_save.item == it && cy_save.lv > lv) {
              lv = cy_save.lv; ench_id = cy_save.id;
            }
            if (cy_save.valid && cy_save.item == it && cy_save.name[0] != 0) nm = cy_save.name;
            uint8_t got = 255;
            mcDiag_("[CLOSE] win=%d craft[%u] item=%u cnt=%u lv=%u nm=%s",
                    (int)window_id, (unsigned)i, (unsigned)it, (unsigned)cn,
                    (unsigned)lv, (nm != nullptr) ? nm : "-");
            if (givePlayerItemAt(player, it, cn, &got, lv > 0 || nm != nullptr) == 0 && got < 41) {
              if (lv > 0) applyEnchant_(slot_idx, got, ench_id, lv);
              if (nm != nullptr) strlcpy(g_item_name[pi_close][got], nm, sizeof(g_item_name[0][0]));
            }
            g_craft_ench[slot_idx][i] = 0;
          }
        }
        player->craft_items[i] = 0;
        player->craft_count[i] = 0;
        player->flags &= ~0x80;
      }
      // 鼠标上那件（刚从附魔台/铁砧拿走、还没落到背包就关窗）：附魔/名字必须跟着入包，
      // 否则“附完魔一退出界面就变普通剑”还是会发生。
      if (player->flagval_16 != 0 && player->flagval_8 != 0) {
        bool need = (cy_save.valid && cy_save.item == player->flagval_16);
        uint8_t gotm = 255;
        if (givePlayerItemAt(player, player->flagval_16, player->flagval_8, &gotm, need) == 0 && gotm < 41) {
          if (need && cy_save.lv > 0) applyEnchant_(slot_idx, gotm, cy_save.id, cy_save.lv);
          if (need && cy_save.name[0] != 0)
            strlcpy(g_item_name[pi_close][gotm], cy_save.name, sizeof(g_item_name[0][0]));
          mcDiag_("[CLOSE] cursor item=%u cnt=%u -> slot=%u lv=%u nm=%s",
                  (unsigned)player->flagval_16, (unsigned)player->flagval_8, (unsigned)gotm,
                  (unsigned)(need ? cy_save.lv : 0), (need ? cy_save.name : "-"));
        }
      } else {
        givePlayerItem(player, player->flagval_16, player->flagval_8);
      }
      player->flagval_16 = 0;
      player->flagval_8 = 0;
      if (window_id == 8) g_pending_name[pi_close][0] = 0;
      g_carry[slot_idx].valid = 0;
      PacketCodec pc(slot.fd);
      // 同步合成格为空
      sendSetContainerSlot_(pc, 0, 0, 0, 0);  // 输出槽
      for (uint8_t i = 1; i <= 4; i++)
        sendSetContainerSlot_(pc, 0, i, 0, 0);  // 合成格 1-4
      // 同步完整背包（带名字/附魔，否则改过名的东西一关窗就被“洗白/没收”）
      for (uint8_t i = 0; i < 41; i++)
        sendSetContainerSlotEnch_(pc, 0, serverSlotToClientSlot(0, i), player->inventory_count[i],
                                   player->inventory_items[i], enchantGet_(slot_idx, i),
                                   g_item_name[pi_close][i][0] ? g_item_name[pi_close][i] : nullptr,
                                   enchantIdGet_(slot_idx, i));
      return true;
    }

    case 0x38: { // Set Creative Mode Slot（创造模式改背包）
      if (!player) return codec.skipBytes((size_t)packet_len);
      size_t cstart = codec.readCount();
      uint16_t cslot = 0; if (!codec.readUint16(cslot)) return false;
      int32_t ccount = 0; if (!codec.readVarInt(ccount)) return false;
      int32_t citem = 0;
      char cnm[20]; cnm[0] = 0;
      uint8_t cench = 0, cench_id = kEnchSharpnessId;
      if (ccount > 0) {
        if (!codec.readVarInt(citem)) return false;
        int32_t comp_add = 0, comp_rem = 0;
        if (codec.readVarInt(comp_add) && comp_add >= 0 && comp_add <= 32) {
          bool ok = true;
          for (int32_t c = 0; c < comp_add; c++) {
            int32_t cid = 0;
            if (!codec.readVarInt(cid) ||
                !readOneComponent_(codec, cid, cnm, sizeof(cnm), &cench, &cench_id)) { ok = false; break; }
          }
          if (ok && codec.readVarInt(comp_rem) && comp_rem >= 0 && comp_rem <= 32) {
            for (int32_t c = 0; c < comp_rem; c++) { int32_t cid = 0; if (!codec.readVarInt(cid)) { ok = false; break; } }
          }
        }
      }
      // 组件解析出问题也别丢物品：只把流对齐到本包末尾
      size_t cused = codec.readCount() - cstart;
      if (!codec.readOverrun() && cused < (size_t)packet_len) codec.skipBytes((size_t)packet_len - cused);
      uint8_t idx = clientSlotToServerSlot_(cslot);
      if (idx == 255) return true;
      if (ccount < 0) ccount = 0;
      if (ccount > 64) ccount = 64;
      // 药水已整体下线：创造模式里点到的药水瓶子一律不收，免得拿在手上点一下只回“无法使用”
      if (citem == I_potion || citem == I_splash_potion || citem == I_lingering_potion) {
        citem = 0; ccount = 0;
      }
      if (idx >= 36 && idx <= 40 && ccount > 1) ccount = 1;   // 护甲/副手一格一件
      // 护甲槽只认对应部位（头盔 39 / 胸甲 38 / 护腿 37 / 靴 36）。
      // 鞘翅本版禁用，getArmorItemSlot_ 对它返回 255，这里会被拒收，
      // 并把服务端这一格的旧内容推回客户端，避免"客户端放进去了、服务端不认"。
      if (idx >= 36 && idx <= 39 && citem != 0 && getArmorItemSlot_((uint16_t)citem) != idx) {
        PacketCodec prc(player->client_fd);
        sendSetContainerSlot_(prc, 0, serverSlotToClientSlot(0, idx), player->inventory_count[idx],
                              player->inventory_items[idx]);
        Serial.printf("[CSLOT] reject item=%d for armor slot %u\r\n", (int)citem, (unsigned)idx);
        return true;
      }
      player->inventory_items[idx] = (uint16_t)citem;
      player->inventory_count[idx] = (uint8_t)ccount;
      mcDiag_("[CSLOT] cslot=%u idx=%u item=%d cnt=%d ench=%u/%u name=%s",
              (unsigned)cslot, (unsigned)idx, (int)citem, (int)ccount,
              (unsigned)cench, (unsigned)cench_id, cnm);
      // 创造模式拖进来的东西也把名字/附魔记下来，重新同步时不会再被“洗白”
      if (idx < 41 && clients_[slot_idx].player_index >= 0) {
        int pi38 = clients_[slot_idx].player_index;
        strlcpy(g_item_name[pi38][idx], cnm, sizeof(g_item_name[0][0]));
        uint8_t prev_lv = (uint8_t)enchantGet_(slot_idx, idx);
        // 创造模式拿到的是“空附魔书”（原版这个物品本来就不带附魔，等于废纸）：
        // 给它补一条锋利 I，铁砧里才用得上；书以外的物品不会被凭空附魔。
        if (cench == 0 && citem == I_enchanted_book && prev_lv == 0) { cench = 1; cench_id = kEnchSharpnessId; }
        if (cench > 0) applyEnchant_(slot_idx, idx, cench_id, cench);
        else if (prev_lv != 0) applyEnchant_(slot_idx, idx, kEnchSharpnessId, 0);
        // 关键：必须把“服务端权威的这一格”回给客户端。
        // 原版服务端处理 set_creative_mode_slot 后就是广播这一格的；不回包的话
        // 客户端只保留它自己那份（比如空附魔书 / 没附魔的剑），服务端记的东西永远看不见。
        PacketCodec ec(clients_[slot_idx].fd);
        sendSetContainerSlotEnch_(ec, 0, serverSlotToClientSlot(0, idx),
                                  player->inventory_count[idx], player->inventory_items[idx],
                                  enchantGet_(slot_idx, idx),
                                  g_item_name[pi38][idx][0] ? g_item_name[pi38][idx] : nullptr,
                                  enchantIdGet_(slot_idx, idx));
      }
      return true;
    }

    case 0x0C: { // Client Status (respawn)
      if (!player) return codec.skipBytes((size_t)packet_len);
      uint8_t action_id; if (!codec.readByte(action_id)) return false;
      if (action_id == 0) {
        // 两种重生要分开：
        // 1) 死亡后点“重生”（health 已 0，/kill 或被打死）→ 回世界出生点，不是死哪在哪复活；
        // 2) 非死亡的重生（客户端换维度之类）→ 就地复活。
        // 断线重连走的是 spawnPlayer_ 的“keep”分支，用的是每 10 秒存盘的位置，不受这里影响。
        bool was_dead = (player->health == 0);
        int16_t rx = (int16_t)player->x;
        int16_t rz = (int16_t)player->z;
        uint8_t ry = player->y;
        uint8_t ryaw = player->yaw, rpitch = player->pitch;
        // 顺序很关键：先重置（模式回到默认生存、清空背包），
        // 再用“重置后的模式”发重生包，否则客户端会留在创造模式里。
        resetPlayerData(player);
        if (was_dead) {
          // resetPlayerData 已经把 x/z 置回 (8,8)、y=80、flags|=0x02，交给 spawnPlayer_ 挑落脚点
          Serial.println("[SPAWN] death respawn -> world spawn");
        } else {
          player->flags &= ~0x02;                 // 不要触发“重新选出生点”
          player->x = rx; player->z = rz;
          uint8_t safe_y = getSafeSpawnY(rx, rz);  // 站立高度 = 当地地表 + 1
          player->y = (safe_y >= 65 && safe_y <= 79) ? safe_y : ry;
          player->yaw = ryaw; player->pitch = rpitch;
          Serial.printf("[SPAWN] respawn in place at %d,%u,%d\r\n", (int)rx, (unsigned)player->y, (int)rz);
        }
        player->grounded_y = 0;
        PacketCodec pc(slot.fd);
        sendRespawn_(pc, player->game_mode);
        spawnPlayer_(slot_idx);
      }
      return true;
    }

    case 0x2A: { // Player Command (sprint/sneak)
      if (!player) return codec.skipBytes((size_t)packet_len);
      int32_t eid; codec.readVarInt(eid);
      uint8_t action; codec.readByte(action);
      int32_t jump_boost; codec.readVarInt(jump_boost);
      if (action == 0) player->flags |= 0x04;
      else if (action == 1) player->flags &= ~0x04;
      else if (action == 3) player->flags |= 0x08;
      else if (action == 4) player->flags &= ~0x08;
      else if (action == 6) {
        // 鞘翅滑翔本版已禁用（不置滑翔态、不改玩家状态），只回一条提示。
        PacketCodec opc_e(player->client_fd);
        const char* emsg = "鞘翅已在本版本中禁用";
        sendSystemChat_(opc_e, emsg, (uint16_t)strlen(emsg));
        return true;
      }
      broadcastPlayerMetadata_(player);
      return true;
    }

    case 0x2B: { // Player Input
      if (!player) return codec.skipBytes((size_t)packet_len);
      uint8_t flags_in; codec.readByte(flags_in);
      if (flags_in & 0x20) player->flags |= 0x04;
      else player->flags &= ~0x04;
      broadcastPlayerMetadata_(player);
      return true;
    }

    case 0x43: { // Use Item (eating)
      if (!player) return codec.skipBytes((size_t)packet_len);
      uint8_t hand; codec.readByte(hand);
      int32_t sequence; codec.readVarInt(sequence);
      codec.skipBytes(8);
      // 鞘翅本版禁用：右键不穿、不推进，只回提示
      if (player->inventory_items[player->hotbar] == I_elytra &&
          player->inventory_count[player->hotbar] > 0) {
        PacketCodec opc_x(player->client_fd);
        const char* xmsg = "鞘翅已在本版本中禁用";
        sendSystemChat_(opc_x, xmsg, (uint16_t)strlen(xmsg));
        return true;
      }

      // 右键护甲 = 穿上。原版把这件事做在 useItem（0x43）里，不是 useItemOn（0x42）；
      // 之前只有 use_item_on 的 face==255 分支在穿甲，可客户端右键空中根本不发那个包，
      // 所以生存模式右键护甲什么都不发生（穿甲分支成了一段死代码）。
      {
        uint16_t held_armor = player->inventory_items[player->hotbar];
        uint8_t armor_slot2 = getArmorItemSlot_(held_armor);
        if (armor_slot2 != 255 && player->inventory_count[player->hotbar] > 0) {
          uint16_t prev_item2 = player->inventory_items[armor_slot2];
          uint8_t prev_count2 = player->inventory_count[armor_slot2];
          player->inventory_items[armor_slot2] = held_armor;
          player->inventory_count[armor_slot2] = 1;
          player->inventory_items[player->hotbar] = prev_item2;
          player->inventory_count[player->hotbar] = prev_count2;
          int gpi_a = slotIndexForPlayer_(player);
          PacketCodec opc_a(player->client_fd);
          sendSetContainerSlotEnch_(opc_a, 0, serverSlotToClientSlot(0, armor_slot2), 1,
                                    player->inventory_items[armor_slot2],
                                    (gpi_a >= 0) ? enchantGet_((uint8_t)gpi_a, armor_slot2) : 0,
                                    (gpi_a >= 0 && g_item_name[gpi_a][armor_slot2][0])
                                        ? g_item_name[gpi_a][armor_slot2] : nullptr,
                                    (gpi_a >= 0) ? enchantIdGet_((uint8_t)gpi_a, armor_slot2)
                                                 : (uint8_t)kEnchSharpnessId);
          sendHeldSlot_(opc_a, player, player->inventory_count[player->hotbar],
                        player->inventory_items[player->hotbar]);
          Serial.printf("[ARMOR] equip item=%u -> slot=%u prev=%u\r\n",
                        (unsigned)held_armor, (unsigned)armor_slot2, (unsigned)prev_item2);
          return true;
        }
      }

      if (canPlayerEat_(player)) {
        player->flagval_16 = 0;
        player->flags |= 0x10;
      }
      return true;
    }

    default:
      return codec.skipBytes((size_t)packet_len);
  }
}

// ============================================================
// 游戏逻辑
// ============================================================

// 出生的“站位”是否够开阔：脚位/头位的 3x3 都是空气（否则一睁眼就是一面土壁）
static bool spawnSiteOpen_(int16_t x, uint8_t y, int16_t z) {
  if (y > 78) return false;
  for (int8_t dz = -1; dz <= 1; dz++) {
    for (int8_t dx = -1; dx <= 1; dx++) {
      int16_t tx = (int16_t)(x + dx), tz = (int16_t)(z + dz);
      if (getBlockAt(tx, y, tz) != B_air) return false;
      if ((uint8_t)(y + 1) <= 79 && getBlockAt(tx, (uint8_t)(y + 1), tz) != B_air) return false;
    }
  }
  return true;
}

void MinecraftServer::spawnPlayer_(uint8_t slot_idx) {
  ClientSlot& slot = clients_[slot_idx];
  if (slot.player_index < 0) return;
  PlayerData* player = &player_data[slot.player_index];
  PacketCodec codec(slot.fd);

  // 上一局被 /kill 过：存档里的 health 是 0，直接沿用会让客户端一进来就是"你死了"界面。
  // 死亡状态下进服/点重生都按"已重生"处理：满血满饥饿，并强制重新选出生点。
  if (player->health == 0) {
    player->health = 20;
    player->hunger = 20;
    player->saturation = 2500;
    player->flags |= 0x02;
    Serial.printf("[SPAWN] revive(%s) health->20, force respawn\r\n", player->name);
  }

  // 是否需要重新选出生点：只有两种情况——新玩家（flags & 0x02，resetPlayerData 置的），
  // 或者上次是死亡状态退出的（health==0，上面已 revive 并置 0x02）。
  // 其余一律“原地进服”：存档里是什么坐标就用什么坐标，玩家退出前站在哪就从哪进，
  // 不做卡方块/地表高度之类的判定去改位置（那些判定会把人挪到别处）。
  bool need_spawn = (player->flags & 0x02) ? true : false;
  int16_t px = (int16_t)player->x, pz = (int16_t)player->z;
  uint8_t py = player->y;
  const char* spawn_why = need_spawn ? "flags0x02" : "keep";
  if (need_spawn) {
    // 在 (8,8) 周围挑“可站立 + 头顶 3 格净空 + 地面在 y>=64”的位置，优先高处。
    // 关键约束：只下发 y64~79 这 6 个 section，地表低于 64 的列客户端看着是空的，
    // 出生在那里就会“整屏方块/悬空”，所以这里必须挑地表 >= 64 的列。
    int16_t bx = 8, bz = 8;
    uint8_t best_y = 0;
    for (int16_t r = 1; r <= 8 && best_y == 0; r++) {
      for (int16_t dz = -r; dz <= r; dz++) {
        for (int16_t dx = -r; dx <= r; dx++) {
          if (dx > -r && dx < r && dz > -r && dz < r) continue;  // 只看外圈
          int16_t tx = 8 + dx, tz = 8 + dz;
          // 刚死过：出生点别挑在尸体附近，否则复活瞬间就把掉落在脚下的物品捡回去了
          if ((uint32_t)(millis() - s_die_ms) < 60000) {
            int16_t ax = (tx > s_die_x) ? (int16_t)(tx - s_die_x) : (int16_t)(s_die_x - tx);
            int16_t az = (tz > s_die_z) ? (int16_t)(tz - s_die_z) : (int16_t)(s_die_z - tz);
            if (ax <= 3 && az <= 3) continue;
          }
          uint8_t ty = getSafeSpawnY(tx, tz);          // 站立高度 = 地表 + 1
          if (ty < 65 || ty > 79) continue;            // 地表必须落在已下发的 section 内
          bool clear = true;
          for (uint8_t k = 1; k <= 3; k++) {
            uint8_t ay = (uint8_t)(ty + k);
            if (ay > 79) break;
            if (getBlockAt(tx, ay, tz) != B_air) { clear = false; break; }
          }
          if (!clear) continue;
          // 平整度评分：周围 1 格内高差越小越好（避免出生在 1 格宽的深坑里）
          uint8_t dmax = 0;
          for (int8_t nz = -1; nz <= 1; nz++) {
            for (int8_t nx = -1; nx <= 1; nx++) {
              uint8_t nh = getSafeSpawnY(tx + nx, tz + nz);
              uint8_t d = (nh > ty) ? (uint8_t)(nh - ty) : (uint8_t)(ty - nh);
              if (d > dmax) dmax = d;
            }
          }
          if (dmax > 1) continue;   // 周围高差超过 1 格 → 太坑/太陡，跳过
          // 站位周围 3x3（脚位+头位）必须都是空气，否则一睁眼就贴着一面土壁
          if (!spawnSiteOpen_(tx, ty, tz)) continue;
          if (ty >= best_y) { best_y = ty; bx = tx; bz = tz; }
        }
      }
    }
    if (best_y == 0) {
      best_y = getSafeSpawnY(8, 8);
      bx = 8; bz = 8;
    }
    player->x = bx;
    player->z = bz;
    player->y = best_y;
    player->flags &= ~0x02;
    Serial.printf("[SPAWN] pick(%d,%u,%d) safe8=%u why=%s src=(%d,%u,%d) flags=0x%02X\r\n",
                  (int)player->x, (unsigned)player->y, (int)player->z,
                  (unsigned)getSafeSpawnY(8, 8), spawn_why, (int)px, (unsigned)py, (int)pz,
                  (unsigned)player->flags);
  } else {
    Serial.printf("[SPAWN] keep(%d,%u,%d) here=%u safe=%u\r\n", (int)player->x, (unsigned)player->y,
                  (int)player->z,
                  (unsigned)((player->y <= 79) ? getBlockAt((int16_t)player->x, player->y, (int16_t)player->z) : 255),
                  (unsigned)getSafeSpawnY((int16_t)player->x, (int16_t)player->z));
  }
  float spawn_x = (float)player->x + 0.5f;
  float spawn_y = (float)player->y;
  float spawn_z = (float)player->z + 0.5f;

  sendSynchronizePlayerPosition_(codec, spawn_x, spawn_y, spawn_z, 0, 0);
  if (g_join_mask & 0x01) sendSetDefaultSpawnPosition_(codec, 8, 80, 8, 0, 0);
  if (g_join_mask & 0x02) sendStartWaitingForChunks_(codec);
  sendPlayerAbilities_(codec, (player->game_mode == 1) ? 0x0D : 0x00);

  int16_t cx = div_floor(player->x, 16), cz = div_floor(player->z, 16);
  sendSetCenterChunk_(codec, cx, cz);
  sendChunkDataAndUpdateLight_(codec, cx, cz);

  // 启动延迟发块队列
  slot.chunk_queue_idx = 0;
  slot.chunk_center_x = cx;
  slot.chunk_center_z = cz;
  slot.chunk_next_send_ms = millis() + slot.chunk_interval_ms;
}

void MinecraftServer::handlePlayerJoin_(uint8_t slot_idx) {
  ClientSlot& slot = clients_[slot_idx];
  if (slot.player_index < 0) return;
  PlayerData* player = &player_data[slot.player_index];
  player->flags &= ~0x20;

  // 药水功能已整体下线：背包里残留的药水一律清掉（否则拿在手上会“无法使用”）
  for (uint8_t i = 0; i < 41; i++) {
    if (player->inventory_items[i] == I_potion || player->inventory_items[i] == I_splash_potion ||
        player->inventory_items[i] == I_lingering_potion) {
      player->inventory_items[i] = 0;
      player->inventory_count[i] = 0;
    }
  }

  // 工具/武器/护甲/鞘翅从这一版起按原版不可堆叠（getItemStackSize 返回 1）。
  // 旧存档里这些物品可能是 64 一叠：如果只改回包不改存档，客户端显示 1、服务端记 64，
  // 一操作数量就对不上（看起来像物品被吞掉或复制）。进服时按新规则把数量压回 1。
  for (uint8_t i = 0; i < 41; i++) {
    uint16_t it = player->inventory_items[i];
    if (it == 0) continue;
    if (getItemStackSize(it) == 1 && player->inventory_count[i] > 1)
      player->inventory_count[i] = 1;
  }

  // 玩家加入时在周围生成几批 mob
  if (g_join_mask & 0x10) {
    for (int attempt = 0; attempt < 8; attempt++) trySpawnMobNearPlayer_(player);
  }

  char buf[64];
  int blen = snprintf(buf, sizeof(buf), "%s joined the game", player->name);
  for (uint8_t i = 0; i < kMaxClients; i++) {
    if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
    PacketCodec oc(clients_[i].fd);
    sendSystemChat_(oc, buf, (uint16_t)blen);
    if (g_join_mask & 0x08) sendPlayerInfoUpdateAddPlayer_(oc, *player);
    if ((g_join_mask & 0x20) && i != slot_idx) {
      sendSpawnEntity_(oc, slot_idx, player->uuid, 155,
        player->x + 0.5, player->y, player->z + 0.5, player->yaw, player->pitch);
    }
  }

  // 把已有玩家同步给新玩家
  PacketCodec pc(slot.fd);
  for (uint8_t i = 0; i < kMaxClients; i++) {
    if (i == slot_idx || !clients_[i].used || clients_[i].state != STATE_PLAY) continue;
    if (clients_[i].player_index < 0) continue;
    PlayerData& other = player_data[clients_[i].player_index];
    if (g_join_mask & 0x08) sendPlayerInfoUpdateAddPlayer_(pc, other);
    if (g_join_mask & 0x20) sendSpawnEntity_(pc, i, other.uuid, 155, other.x + 0.5, other.y, other.z + 0.5, other.yaw, other.pitch);
  }

  // 把已存在的 mob 同步给新玩家
  for (int i = 0; i < MAX_MOBS; i++) {
    if (mob_data[i].type == 0) continue;
    if ((mob_data[i].data & 31) == 0) continue;  // 已死亡
    uint8_t uuid[16];
    uint32_t r = fast_rand();
    memcpy(uuid, &r, 4);
    memcpy(uuid + 4, &i, 4);
    memset(uuid + 8, 0, 8);
    sendSpawnEntity_(pc, -2 - i, uuid, mob_data[i].type,
      mob_data[i].x + 0.5, mob_data[i].y, mob_data[i].z + 0.5, 0, 0);
  }

  // 已解锁的合成：把服务端这套合成表当配方书一次性推给客户端
  sendRecipeBookAdd_(pc);
}

void MinecraftServer::handlePlayerDisconnect_(uint8_t slot_idx) {
  pktDump_();
  if (s_tree_pending && slot_idx == s_tree_slot &&
      (int32_t)(millis() - s_tree_ms) < 6000) {
    s_tree_pending = false;
    if (s_tree_forced_id == 0xFF) {
      s_tree_cand_idx = (uint8_t)((s_tree_cand_idx + 1) % kTreeCandidateCount);
      Serial.printf("[CMD] declare_commands id=0x%02X dropped, next try 0x%02X\r\n",
                    (unsigned)s_tree_cur_id, (unsigned)kTreeCandidates[s_tree_cand_idx]);
    }
  }

  if (s_edata_pending && slot_idx == s_edata_slot &&
      (int32_t)(millis() - s_edata_ms) < 6000) {
    s_edata_pending = false;
    if (s_edata_forced_id == 0xFF) {
      s_edata_cand_idx = (uint8_t)((s_edata_cand_idx + 1) % kEDataCandidateCount);
      Serial.printf("[EDATA] id=0x%02X dropped, next try 0x%02X\r\n",
                    (unsigned)g_entity_data_id, (unsigned)kEDataCandidates[s_edata_cand_idx]);
    }
  }

  if (gm_last_slot_ == slot_idx && (int32_t)(millis() - gm_last_change_ms_) < 10000) {
    Serial.printf("[CMD] slot=%u dropped %lums after gamemode switch\r\n",
                  (unsigned)slot_idx, (unsigned long)(millis() - gm_last_change_ms_));
  }
  ClientSlot& slot = clients_[slot_idx];
  if (slot.player_index < 0) return;
  PlayerData* player = &player_data[slot.player_index];
  if (slot.chest_open) {
    worldStoreSaveChest(slot.chest_x, slot.chest_y, slot.chest_z);
    slot.chest_open = false;
    slot.chest_buf = nullptr;
  }
  worldStoreSavePlayer(slot.player_index);
  worldStoreFlush();                       // 断线即落盘：方块改动 + 玩家档都写掉
  player->client_fd = -1;

  char buf[64];
  int blen = snprintf(buf, sizeof(buf), "%s left the game", player->name);
  for (uint8_t i = 0; i < kMaxClients; i++) {
    if (!clients_[i].used || clients_[i].state != STATE_PLAY || i == slot_idx) continue;
    PacketCodec oc(clients_[i].fd);
    sendSystemChat_(oc, buf, (uint16_t)blen);
    sendRemoveEntity_(oc, slot_idx);
  }
}

void MinecraftServer::handleServerTick_() {
  world_time = (world_time + TIME_BETWEEN_TICKS / 50000) % 24000;
  server_ticks++;

  for (uint8_t i = 0; i < kMaxClients; i++) {
    if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
    if (clients_[i].player_index < 0) continue;
    PlayerData* player = &player_data[clients_[i].player_index];
    if (player->client_fd == -1) continue;
    PacketCodec pc(clients_[i].fd);

    // 攻击冷却
    if (player->flags & 0x01) {
      if (player->flagval_8 >= (uint8_t)(0.6f * TICKS_PER_SECOND)) {
        player->flags &= ~0x01; player->flagval_8 = 0;
      } else player->flagval_8++;
    }

    // 进食计时
    if (player->flags & 0x10) {
      if (player->flagval_16 >= (uint16_t)(1.6f * TICKS_PER_SECOND)) {
        doPlayerEat_(player);
        player->flags &= ~0x10;
        player->flagval_16 = 0;
      } else player->flagval_16++;
    }

    // 状态效果整段已移除（客户端无法通过我们的物品/药水通道还原效果，留着只会出现“无效果药水”）

#ifndef BROADCAST_ALL_MOVEMENT
    player->flags &= ~0x40;
#endif

    // 每秒事件
    if (server_ticks % (uint32_t)TICKS_PER_SECOND != 0) continue;
    sendKeepAlive_(pc);
    sendUpdateTime_(pc, world_time);
    tickDrops_();
    // 天气自动轮换（每 10~20 分钟随机一次）
    if ((uint32_t)(millis() - weather_change_ms_) > 900000UL) {
      uint8_t w = (uint8_t)(fast_rand() % 3);
      setWeather_(w >= 2 ? 2 : (w == 1 ? 1 : 0), false);
    }

    // TAB 列表页脚: 显示堆内存
    if (g_join_mask & 0x40) {
      char footer_text[80];
      int footer_text_len;
#ifdef _WIN32
      MEMORYSTATUSEX ms = {};
      ms.dwLength = sizeof(ms);
      GlobalMemoryStatusEx(&ms);
      uint32_t avail_kb = (uint32_t)(ms.ullAvailPhys / 1024);
      footer_text_len = snprintf(footer_text, sizeof(footer_text),
        "\xC2\xA7""b\xC2\xA7""lFree: \xC2\xA7""a%u KB", avail_kb);
#else
      uint32_t free_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
      // §b = aqua bold, §a = green
      footer_text_len = snprintf(footer_text, sizeof(footer_text),
        "\xC2\xA7""b\xC2\xA7""lHeap: \xC2\xA7""a%u KB free", free_kb);
#endif
      // tab_list (0x7A): header(NBT Text) + footer(NBT Text)
      uint32_t tab_pkt_len = 1
        + 1 + 2 + 0
        + 1 + 2 + footer_text_len;
      pc.writeVarInt(tab_pkt_len);
      pc.writeByte(0x7A);
      pc.writeByte(8); pc.writeUint16(0);
      pc.writeByte(8); pc.writeUint16((uint16_t)footer_text_len);
      pc.writeExact((const uint8_t*)footer_text, (uint32_t)footer_text_len);
    }

    // 岩浆伤害
    uint8_t block = getBlockAt(player->x, player->y, player->z);
    if (block >= B_lava && block < B_lava + 4) hurtEntity_(i, -1, D_lava, 8);

    // 溺水伤害 (flagval_16 高字节存已在水中的秒数, 最多 15 秒憋死)
    uint8_t head_block = getBlockAt(player->x, player->y + 1, player->z);
    bool in_water = (head_block >= B_water && head_block <= B_water_7);
    uint8_t air_ticks = (player->flagval_16 >> 8) & 0xFF;
    if (in_water) {
      if (air_ticks < 15) {
        air_ticks++;
        player->flagval_16 = (player->flagval_16 & 0x00FF) | ((uint16_t)air_ticks << 8);
      }
      if (air_ticks >= 15) hurtEntity_(i, -1, D_drown, 2);
      // 同步氧气值给客户端 (满300, 每秒-20)
      int16_t air = (int16_t)(300 - air_ticks * 20);
      if (air < 0) air = 0;
      // Set Entity Data: index 1 (air supply), type 1 (VarInt)
      uint32_t air_pkt_len = pc.sizeVarInt(g_entity_data_id) + pc.sizeVarInt(i) + 1 + 1 + pc.sizeVarInt((uint32_t)air) + 1;
      pc.writeVarInt(air_pkt_len); pc.writeByte(g_entity_data_id);
      pc.writeVarInt(i); // entity id = slot index (player)
      pc.writeByte(1);   // metadata index: air supply
      pc.writeVarInt(1); // type: VarInt
      pc.writeVarInt((uint32_t)air);
      pc.writeByte(0xFF); // end
    } else if (air_ticks > 0) {
      player->flagval_16 = player->flagval_16 & 0x00FF;  // 离水清零
      // 恢复满氧气
      uint32_t air_pkt_len = pc.sizeVarInt(g_entity_data_id) + pc.sizeVarInt(i) + 1 + 1 + pc.sizeVarInt(300) + 1;
      pc.writeVarInt(air_pkt_len); pc.writeByte(g_entity_data_id);
      pc.writeVarInt(i);
      pc.writeByte(1);
      pc.writeVarInt(1);
      pc.writeVarInt(300);
      pc.writeByte(0xFF);
    }

#ifdef ENABLE_CACTUS_DAMAGE
    if (block == B_cactus ||
        getBlockAt(player->x + 1, player->y, player->z) == B_cactus ||
        getBlockAt(player->x - 1, player->y, player->z) == B_cactus ||
        getBlockAt(player->x, player->y, player->z + 1) == B_cactus ||
        getBlockAt(player->x, player->y, player->z - 1) == B_cactus)
      hurtEntity_(i, -1, D_cactus, 4);
#endif

    // 自然回血
    if (player->health < 20 && player->health > 0 && player->hunger >= 18) {
      if (player->saturation >= 600) { player->saturation -= 600; player->health++; }
      else { player->hunger--; player->health++; }
      sendSetHealth_(pc, player->health, player->hunger, player->saturation);
    }
  }

  if (server_ticks % (uint32_t)TICKS_PER_SECOND == 0) tickMobs_();
  if (rng_seed == 0) rng_seed = world_seed;
}

void MinecraftServer::dropHeldItem_(PlayerData* player, uint8_t drop_n) {
  if (!player) return;
  // 注意：物品 id 是 16 位（下界合金剑 942），这里原来写成 uint8_t，
  // 942 被截成 174，地上的掉落物就变成了别的物品（表现为“我的剑变成木头”）。
  uint16_t held = player->inventory_items[player->hotbar];
  uint8_t have = player->inventory_count[player->hotbar];
  if (!held || !have) {
    Serial.printf("[DROP] skip slot=%u (empty)\r\n", (unsigned)player->hotbar);
    return;
  }
  if (drop_n == 0) drop_n = 1;
  if (drop_n > have) drop_n = have;
  // 丢在身前（优先 2 格，拾取判定是 1 格内，丢 1 格会被自己立刻捡回）。
  // 落点必须是客户端真正收到的空气格：世界只下发 y64~79，如果把物品放在实心的
  // 山体/树冠里、或者放在 >=80 的地表上，客户端那格没有数据/被方块遮住，看起来
  // 就是“丢出去的物品卡在方块里、完全看不见也捡不到”。所以这里逐格校验：
  //   站立高度在 [65,79] 且贴近玩家脚位（-1~+2）、该格是空气、该格下方是实心地面；
  //   不满足就换下一格，身前全不通就试身后，最后才退回自己脚边（有 1.2 秒拾取延迟兜着）。
  float yawdeg = (float)player->yaw * 180.0f / 127.0f;
  float rad = yawdeg * 3.14159265f / 180.0f;
  float fx = -sinf(rad), fz = cosf(rad);   // MC：yaw=0 朝 +z，yaw=90 朝 -x
  int8_t fdx = 0, fdz = 0;
  if (fx > 0.707f) fdx = 1; else if (fx < -0.707f) fdx = -1;
  else if (fz > 0.707f) fdz = 1; else fdz = -1;
  static const uint8_t kSteps[] = {2, 1, 3};   // 候选距离：2 格（刚出拾取范围）优先，1 格次之，3 格最后
  static const uint8_t kPen[]   = {0, 4, 7};
  int16_t tx = player->x, tz = player->z;
  uint8_t ty = player->y;
  int best_pen = 255;
  const char* how = "feet";
  for (uint8_t dir = 0; dir < 2; dir++) {
    int8_t sx = (dir == 0) ? fdx : (int8_t)(-fdx);
    int8_t sz = (dir == 0) ? fdz : (int8_t)(-fdz);
    for (uint8_t i = 0; i < 3; i++) {
      uint8_t step = kSteps[i];
      int16_t cx = (int16_t)(player->x + sx * (int16_t)step);
      int16_t cz = (int16_t)(player->z + sz * (int16_t)step);
      uint8_t s = getSafeSpawnY(cx, cz);                    // 站立高度 = 地表 + 1
      if (s < 65 || s > 79) continue;                      // 不在已下发 section 内
      // 落点高度限制在玩家脚边这一层附近（-1 ~ +2）：否则朝坡上丢会把物品放到
      // 头顶的坡顶、朝沟里丢会掉到看不见的沟底，都表现为“物品卡进方块里”。
      int dh = (int)s - (int)player->y;
      if (dh < -1 || dh > 2) continue;
      if (getBlockAt(cx, s, cz) != B_air) continue;         // 落点被树/墙/雪占着
      if (getBlockAt(cx, (uint8_t)(s - 1), cz) == B_air) continue;  // 脚下不是地面
      int pen = kPen[i] + (dir ? 2 : 0) + (dh < 0 ? (-dh) * 2 : dh);
      if (pen < best_pen) {
        best_pen = pen; tx = cx; tz = cz; ty = s;
        char tag = (step == 1) ? '1' : (step == 2 ? '2' : '3');
        how = (dir == 0) ? (tag == '1' ? "fwd1" : (tag == '2' ? "fwd2" : "fwd3"))
                         : (tag == '1' ? "back1" : (tag == '2' ? "back2" : "back3"));
      }
    }
  }
  addDrop_(tx, ty, tz, held, drop_n);
  Serial.printf("[DROP] slot=%u item=%u n=%u placed=%s from=(%d,%u,%d) yaw=%d\r\n",
                (unsigned)player->hotbar, (unsigned)held, (unsigned)drop_n, how,
                (int)player->x, (unsigned)player->y, (int)player->z, (int)player->yaw);
  player->inventory_count[player->hotbar] = (uint8_t)(have - drop_n);
  if (player->inventory_count[player->hotbar] == 0) player->inventory_items[player->hotbar] = 0;
  int pi = slotIndexForPlayer_(player);
  if (pi >= 0) {
    PacketCodec pc(clients_[pi].fd);
    sendHeldSlot_(pc, player, player->inventory_count[player->hotbar], player->inventory_items[player->hotbar]);
  }
}

void MinecraftServer::handlePlayerAction_(PlayerData* player, int action, int16_t x, int16_t y, int16_t z) {
  // 丢物品（Q / Ctrl+Q）：3 = 整叠，4 = 一个，丢成地上的掉落物
  if (action == 3 || action == 4) {
    uint8_t have = player->inventory_count[player->hotbar];
    dropHeldItem_(player, (action == 3) ? (have ? have : 1) : 1);
    return;
  }

  // 停止吃东西
  if (action == 5) { player->flagval_16 = 0; player->flags &= ~0x10; return; }

  if (action != 0 && action != 2) return;
  if (action == 0 && player->game_mode == 1) {
    makeBlockChange(x, (uint8_t)y, z, 0);
    for (uint8_t i = 0; i < kMaxClients; i++) {
      if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
      PacketCodec oc(clients_[i].fd);
      sendBlockUpdate_(oc, x, y, z, B_air);
    }
    return;
  }

  uint8_t block = getBlockAt(x, y, z);
  if (action == 0 && !isInstantlyMined(player, block)) return;
  if (makeBlockChange(x, (uint8_t)y, z, 0)) return;

  // 挖掉箱子：里面的东西一起掉出来（箱子本体在下面按 getMiningResult 掉）
#ifdef ALLOW_CHESTS
  if (block == B_chest) dropChestContents_(x, (uint8_t)y, z);
#endif

  uint16_t held_item = player->inventory_items[player->hotbar];
  uint16_t item = getMiningResult(held_item, block);
  bumpToolDurability(player);
  if (item) addDrop_(x, (uint8_t)y, z, item, 1);   // 掉在地上，靠近 1 格自动捡

  for (uint8_t i = 0; i < kMaxClients; i++) {
    if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
    PacketCodec oc(clients_[i].fd);
    sendBlockUpdate_(oc, x, y, z, B_air);
  }

  int pi = slotIndexForPlayer_(player);
  if (pi >= 0 && item) {
    PacketCodec pc(clients_[pi].fd);
    for (uint8_t i = 0; i < 41; i++) {
      if (player->inventory_items[i] == item) {
        sendSetContainerSlotEnch_(pc, 0, serverSlotToClientSlot(0, i), player->inventory_count[i],
                                   player->inventory_items[i], enchantGet_((uint8_t)pi, i),
                                   g_item_name[pi][i][0] ? g_item_name[pi][i] : nullptr,
                                   enchantIdGet_((uint8_t)pi, i));
        break;
      }
    }
  }

#ifdef DO_FLUID_FLOW
  uint8_t block_above = getBlockAt(x, y + 1, z);
  checkFluidUpdate(x, (uint8_t)(y + 1), z, block_above);
  checkFluidUpdate(x - 1, (uint8_t)y, z, getBlockAt(x - 1, y, z));
  checkFluidUpdate(x + 1, (uint8_t)y, z, getBlockAt(x + 1, y, z));
  checkFluidUpdate(x, (uint8_t)y, z - 1, getBlockAt(x, y, z - 1));
  checkFluidUpdate(x, (uint8_t)y, z + 1, getBlockAt(x, y, z + 1));
#else
  uint8_t block_above = getBlockAt(x, y + 1, z);
#endif

  // 处理上方柱状方块
  uint8_t y_offset = 1;
  while (isColumnBlock(block_above)) {
    makeBlockChange(x, (uint8_t)(y + y_offset), z, 0);
    uint16_t col_item = getMiningResult(0, block_above);
    if (col_item) givePlayerItem(player, col_item, 1);
    for (uint8_t i = 0; i < kMaxClients; i++) {
      if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
      PacketCodec oc(clients_[i].fd);
      sendBlockUpdate_(oc, x, y + y_offset, z, B_air);
    }
    y_offset++;
    block_above = getBlockAt(x, y + y_offset, z);
  }
}

void MinecraftServer::handlePlayerUseItem_(PlayerData* player, int16_t x, int16_t y, int16_t z, uint8_t face) {
  uint8_t target = (face == 255) ? 0 : getBlockAt(x, y, z);
  uint8_t *count = &player->inventory_count[player->hotbar];
  uint16_t *item = &player->inventory_items[player->hotbar];
  int pi = slotIndexForPlayer_(player);
  if (pi < 0) return;
  PacketCodec pc(clients_[pi].fd);

  // 非潜行时先处理容器交互
  if (!(player->flags & 0x04) && face != 255) {
    if (target == B_crafting_table) {
      sendOpenScreen_(pc, 12, "Crafting", 8);
      // 打开时把 3×3 格子里的东西和当前能合成出的产物发给客户端，
      // 否则客户端界面是全空的，看起来像“工作台没反应”。
      for (int i = 0; i < 9; i++)
        sendSetContainerSlot_(pc, 12, (uint16_t)(i + 1), player->craft_count[i], player->craft_items[i]);
      uint8_t out_count; uint16_t out_item;
      getCraftingOutput(player, 3, 3, &out_count, &out_item);
      sendSetContainerSlot_(pc, 12, 0, out_count, out_item);
      player->flags |= 0x80;
      return;
    } else if (target == B_enchanting_table) {
      // 本版停用附魔台（附魔相关界面在 26.1.2 上还有未解决的问题，先整块关掉）
      const char* msg = "附魔台已在本版本中禁用";
      sendSystemChat_(pc, msg, (uint16_t)strlen(msg));
      return;
    } else if (target == B_smithing_table) {
      // 26.1.2 锻造台四格是否可放由客户端配方属性表决定，本服不发 update_recipes，
      // 属性表为空 → 客户端本地拒绝任何物品。这里改用 9x3 通用容器（id31/菜单2）：
      // 全格都能放，0=模板 1=底材 2=附加 3=结果，服务端自己结算。
      // 真原版：先下发配方属性表（客户端才会允许往三格里放东西），再开 menu 21 四格界面
      sendUpdateRecipes_(pc);
      sendOpenScreen_(pc, 21, "Smithing", 8);
      for (int i = 0; i < 4; i++)
        sendSetContainerSlot_(pc, 21, (uint16_t)i, player->craft_count[i], player->craft_items[i]);
      sendSetContainerData_(pc, 21, 0, 0);          // hasRecipeError = 0
      {   // 格子规定：告诉玩家每格放什么，避免乱放又“没反应”
        const char* rule = "锻造台：0=锻造模板  1=钻石/下界合金工具盔甲  2=下界合金锭  3=结果";
        sendSystemChat_(pc, rule, (uint16_t)strlen(rule));
      }
      // 结果预览：必须有模板；底材（+同类附加数量）
      player->craft_items[3] = 0; player->craft_count[3] = 0;
      if (player->craft_items[0] != 0 && player->craft_count[0] > 0 &&
          player->craft_items[1] != 0 && player->craft_count[1] > 0) {
        uint8_t oc = player->craft_count[1];
        if (player->craft_items[2] == player->craft_items[1]) {
          uint16_t tot = (uint16_t)player->craft_count[1] + player->craft_count[2];
          oc = (uint8_t)(tot > 64 ? 64 : tot);
        }
        player->craft_items[3] = player->craft_items[1];
        player->craft_count[3] = oc;
        sendSetContainerSlot_(pc, 21, 3, oc, player->craft_items[1]);
      } else {
        sendSetContainerSlot_(pc, 21, 3, 0, 0);
      }
      player->flags |= 0x80;
      return;    } else if (target == B_anvil) {
      // 本版停用铁砧（改名/合并同样受 HashedStack 无组件原文的限制）
      const char* msg = "铁砧已在本版本中禁用";
      sendSystemChat_(pc, msg, (uint16_t)strlen(msg));
      return;
    } else if (target == B_furnace) {
      sendOpenScreen_(pc, 14, "Furnace", 7);
      // 熔炉三槽：0 原料 1 燃料 2 产物（产物先按现有原料显示预览）
      for (int i = 0; i < 3; i++)
        sendSetContainerSlot_(pc, 14, (uint16_t)i, player->craft_count[i], player->craft_items[i]);
      uint8_t pv_count; uint16_t pv_item;
      furnacePreview(player, &pv_count, &pv_item);
      if (pv_count > 0) {
        player->craft_items[2] = pv_item;
        player->craft_count[2] = pv_count;
        sendSetContainerSlot_(pc, 14, 2, pv_count, pv_item);
      }
      player->flags |= 0x80;
      return;
    } else if (target == B_composter && *count > 0) {
      uint32_t compost_chance = isCompostItem(*item);
      if (compost_chance != 0) {
        if ((*count -= 1) == 0) *item = 0;
        sendHeldSlot_(pc, player, *count, *item);
        if (fast_rand() < compost_chance) givePlayerItem(player, I_bone_meal, 1);
        return;
      }
    }
#ifdef ALLOW_CHESTS
    else if (target == B_chest) {
      ChestStore *cs = findChestStore(x, (uint8_t)y, z);
      if (cs == nullptr) cs = ensureChestStore(x, (uint8_t)y, z);
      if (cs == nullptr) return;                    // 箱子内容表满了
      uint8_t *storage_ptr = cs->items;
      int csl = slotIndexForPlayer_(player);
      if (csl >= 0) {
        clients_[csl].chest_buf = storage_ptr;
        clients_[csl].chest_x = x;
        clients_[csl].chest_y = (uint8_t)y;
        clients_[csl].chest_z = z;
        clients_[csl].chest_open = true;
      }
      player->flags |= 0x80;
      sendOpenScreen_(pc, 2, "Chest", 5);
      for (int i = 0; i < 27; i++) {
        uint16_t ci; uint8_t cc;
        memcpy(&ci, storage_ptr + i * 3, 2);
        memcpy(&cc, storage_ptr + i * 3 + 2, 1);
        if (cc == 0 || ci == 0) { ci = 0; cc = 0; }   // 空槽只发一个 0，别发残留字节
        sendSetContainerSlot_(pc, 2, i, cc, ci);
      }
      return;
    }
#endif
  }

  // 创造模式手持物品是无限的，不检查数量
  if (player->game_mode != 1 && *count == 0) return;

  // 骨粉催树
  if (*item == I_bone_meal && face != 255) {
    uint8_t target_below = getBlockAt(x, y - 1, z);
    if (target == B_oak_sapling) {
      if ((*count -= 1) == 0) *item = 0;
      sendHeldSlot_(pc, player, *count, *item);
      if ((target_below == B_dirt || target_below == B_grass_block || target_below == B_snowy_grass_block || target_below == B_mud) &&
          (fast_rand() & 3) == 0) {
        placeTreeStructure(x, (uint8_t)y, z);
        broadcastBlockChangesInArea_(x - 3, z - 3, x + 3, z + 3);
      }
      return;
    }
  }

  // 吃东西
  if (canPlayerEat_(player)) {
    player->flagval_16 = 0;
    player->flags |= 0x10;
    return;
  }

  // 穿护甲
  uint8_t armor_slot = getArmorItemSlot_(*item);
  if (armor_slot != 255) {
    if (face != 255) return;
    uint16_t prev_item = player->inventory_items[armor_slot];
    uint8_t prev_count = player->inventory_count[armor_slot];
    player->inventory_items[armor_slot] = *item;
    player->inventory_count[armor_slot] = 1;
    player->inventory_items[player->hotbar] = prev_item;
    player->inventory_count[player->hotbar] = prev_count;
    // 以前这里写的是 window_id = -2：那会被当成 4294967294 这个 VarInt 发出去，
    // 客户端根本对不上任何打开的窗口，于是"穿上护甲"在客户端完全没有反应（也不会显示）。
    // 护甲槽属于玩家背包窗口 0（客户端槽号 5~8 / 45）。
    sendSetContainerSlot_(pc, 0, serverSlotToClientSlot(0, armor_slot), 1, player->inventory_items[armor_slot]);
    sendHeldSlot_(pc, player, prev_count, prev_item);
    return;
  }

  // 放方块
  if (face == 255) return;
  uint8_t block = I_to_B(*item);
  if (block == 0) return;

  // 状态写死的方块（楼梯/梯子/门/栅栏/床/铁轨…）整套下线：
  // 服务端只发 block id、不发 state，这些方块放下去外形/朝向一定是错的。
  // 与其半残废，不如直接不让放，并明确回一条聊天提示。
  if (isUnsupportedBlock(block)) {
    sendSystemChat_(pc, "无法放置此方块", 21);   // 7 个汉字 = 21 字节 UTF-8
    return;
  }

  // 放置规则回到原版语义：一律按点击的“面”偏移一格（点上面就放到上方，点侧面就放到旁边）。
  // 之前那套“对着草方块/地表植物直接把方块盖在被点的格子里”的 cover_here 逻辑已整体删除：
  // 它会把地面的草、泥土、花木甚至别的不该动的方块顶掉，问题远比它解决的麻烦多。
  // 梯子要先记住“被点的那块”，它就是梯子的支撑面。
  int64_t sup_x = x, sup_y = y, sup_z = z;
  uint8_t ladder_facing = 255;
  if (block == B_ladder) {
    // 原版：梯子只能贴在方块的水平侧面上；facing = 点击面的反方向（背朝支撑方块）
    // 梯子 facing 的取值顺序（客户端 jar 导出的 blocks.json 实证，不是猜）：
    //   minecraft:ladder  facing = [north, south, west, east] → 0=north 1=south 2=west 3=east
    //   5719+facing*2 = waterlogged:true，+1 = false
    // 原版规则：facing 指向“离墙那一侧”，支撑方块在 facing 的反方向；
    // 点墙的哪个面，梯子就朝那个面（朝玩家），所以 facing == 点击面方向。
    // 以前这里写成“点北面→朝南”，北/南、西/东整对调，模型贴到了外侧空气那一面，
    // 于是墙和梯子之间空出一格能走进去，攀爬面也反了。
    switch (face) {
      case 2: ladder_facing = 0; break;   // 点北面(-z) -> 梯子朝北
      case 3: ladder_facing = 1; break;   // 点南面(+z) -> 朝南
      case 4: ladder_facing = 2; break;   // 点西面(-x) -> 朝西
      case 5: ladder_facing = 3; break;   // 点东面(+x) -> 朝东
      default:
        sendSystemChat_(pc, "梯子只能贴墙放置", 27);
        return;
    }
    uint8_t support = getBlockAt(sup_x, (uint8_t)sup_y, sup_z);
    if (support == B_air || isPassableBlock(support)) {
      sendSystemChat_(pc, "梯子只能贴墙放置", 27);
      return;
    }
  }

  switch (face) {
    case 0: y -= 1; break; case 1: y += 1; break;
    case 2: z -= 1; break; case 3: z += 1; break;
    case 4: x -= 1; break; case 5: x += 1; break;
    default: break;
  }

  // 目标格如果正好是玩家自己脚下/头顶这一格，就别往身体里塞方块：
  // 旧代码只是 return，客户端已经预测了一个“幽灵方块”，本地碰撞会把人挤出去/顶上天。
  // 这里只把这一格的实际方块回发一遍撤掉预测；绝对不要再发位置同步——
  // 位置同步包会把 yaw/pitch 一起拉回 0，快速搭塔时看起来就是“视角被拉高/复位”。
  if (!isPassableBlock(block)) {
    bool in_feet = (x == player->x && z == player->z && y == player->y);
    bool in_head = (x == player->x && z == player->z && y == (uint8_t)(player->y + 1));
    if (in_feet || in_head) {
      sendBlockUpdate_(pc, x, y, z, getBlockAt(x, y, z));
      return;
    }
  }

  // 目标格本身必须可替换（空气、草、水、雪层这类），否则放不下
  if (isReplaceableBlock(getBlockAt(x, y, z)) && (!isColumnBlock(block) || getBlockAt(x, y - 1, z) != B_air)) {
    // 放置限速：100ms 内只认第一次，客户端同一瞬间发的重复包会被丢掉，不会再“一下放好几块”。
    int pslot = slotIndexForPlayer_(player);
    uint32_t now_ms = millis();
    if (pslot >= 0 && pslot < MAX_PLAYERS) {
      if ((uint32_t)(now_ms - s_last_place_ms[pslot]) < 50) return;
      s_last_place_ms[pslot] = now_ms;
    }
    if (makeBlockChange(x, (uint8_t)y, z, block)) return;

    // 带状态的方块：算好这一格该用的原版 state id（走独立旁路表记录，不塞进 uint8 的 block 字段）
    uint16_t state = 0;
    if (isStairsBlock(block)) {
      // 朝向 = 玩家水平朝向；点上面 = 正放（bottom），点侧面/底面 = 倒放（top）
      int a = ((int)player->yaw * 180) / 127;      // yaw 存的是 ±127 的弧度近似值
      if (a < 0) a += 360;
      uint8_t facing;                              // 0=north 1=south 2=west 3=east
      if (a >= 315 || a < 45) facing = 1;          // 面朝南 (+z)
      else if (a < 135) facing = 2;                // 面朝西 (-x)
      else if (a < 225) facing = 0;                // 面朝北 (-z)
      else facing = 3;                             // 面朝东 (+x)
      state = stairsStateId(block, facing, (face == 1) ? 0 : 1, 0);
    } else if (block == B_ladder) {
      state = ladderStateId(ladder_facing, 0);
    } else if (block == B_furnace) {
      // 熔炉 facing 取值顺序同梯子：north=0 south=1 west=2 east=3，5327+facing*2+(lit?0:1)
      // 原版熔炉正面朝向玩家：facing = 玩家朝向的反向（南北、西东互换）
      int a2 = ((int)player->yaw * 180) / 127;
      if (a2 < 0) a2 += 360;
      uint8_t look;
      if (a2 >= 315 || a2 < 45) look = 1;          // 玩家朝南 (+z)
      else if (a2 < 135) look = 2;                 // 朝西 (-x)
      else if (a2 < 225) look = 0;                 // 朝北 (-z)
      else look = 3;                               // 朝东 (+x)
      state = furnaceStateId((uint8_t)(look ^ 1), 0);   // 0<->1(north/south)、2<->3(west/east)
    }
    if (state) {
      blockStateExtraSet((int16_t)x, (uint8_t)y, (int16_t)z, state);
      worldStoreNoteState((int16_t)x, (uint8_t)y, (int16_t)z, state);
    }

    if (player->game_mode != 1) {   // 创造模式放置方块不扣数量
      *count -= 1;
      if (*count == 0) *item = 0;
    }

#ifdef DO_FLUID_FLOW
    checkFluidUpdate(x, y + 1, z, getBlockAt(x, y + 1, z));
    checkFluidUpdate(x - 1, (uint8_t)y, z, getBlockAt(x - 1, y, z));
    checkFluidUpdate(x + 1, (uint8_t)y, z, getBlockAt(x + 1, y, z));
    checkFluidUpdate(x, (uint8_t)y, z - 1, getBlockAt(x, y, z - 1));
    checkFluidUpdate(x, (uint8_t)y, z + 1, getBlockAt(x, y, z + 1));
#endif

    for (uint8_t i = 0; i < kMaxClients; i++) {
      if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
      PacketCodec oc(clients_[i].fd);
      if (state) sendBlockUpdateState_(oc, x, y, z, state);
      else sendBlockUpdate_(oc, x, y, z, block);
    }
    // 放完只把这一格回发一遍（撤掉客户端的预测），绝不回发位置同步：
    // 位置同步会把视角一起拉回，搭高时看起来就是“人被顶飞/视角被复位”。
    { }
  }

  sendHeldSlot_(pc, player, *count, *item);
}

void MinecraftServer::hurtEntity_(int entity_id, int attacker_slot, uint8_t damage_type, uint8_t damage) {
  // --- Mob 伤害处理 ---
  if (entity_id <= -2) {
    int mob_idx = -2 - entity_id;
    if (mob_idx < 0 || mob_idx >= MAX_MOBS) return;
    if (mob_data[mob_idx].type == 0) return;

    // 攻击冷却 & 武器加成
    if (attacker_slot >= 0 && attacker_slot < kMaxClients && clients_[attacker_slot].player_index >= 0) {
      PlayerData* attacker = &player_data[clients_[attacker_slot].player_index];
      if (attacker->flags & 0x01) return;
      uint16_t held = attacker->inventory_items[attacker->hotbar];
      // 剑/斧按材质给加成。原来只写到钻石剑，漏了下界合金剑，而且斧完全不参与。
      uint8_t kind = getToolKind(held);
      uint8_t tier = getToolTier(held);
      if (kind == 1) {          // 剑
        damage *= (tier == 1) ? 4 : (tier == 2) ? 5 : (tier == 3) ? 6 : (tier == 5) ? 7 : 8;
      } else if (kind == 4) {   // 斧
        damage *= (tier == 1) ? 3 : (tier == 2) ? 4 : (tier == 3) ? 5 : (tier == 5) ? 6 : 7;
      }
      attacker->flags |= 0x01; attacker->flagval_8 = 0;
    }

    uint8_t hp = mob_data[mob_idx].data & 31;
    if (hp <= damage) {
      mob_data[mob_idx].data &= ~31;  // hp = 0, 标记死亡
      mob_data[mob_idx].y = 0;        // 重置死亡计时器

      // 掉落物直接给击杀者
      PlayerData* killer = nullptr;
      if (attacker_slot >= 0 && attacker_slot < kMaxClients && clients_[attacker_slot].player_index >= 0)
        killer = &player_data[clients_[attacker_slot].player_index];
      if (killer) {
        uint32_t r = fast_rand();
        switch (mob_data[mob_idx].type) {
          case 26:  // Chicken
            givePlayerItem(killer, I_chicken, 1);
            break;
          case 30:  // Cow
            givePlayerItem(killer, I_beef, 1 + (r % 3));
            break;
          case 100:  // Pig
            givePlayerItem(killer, I_porkchop, 1 + (r % 3));
            break;
          case 111:  // Sheep
            givePlayerItem(killer, I_mutton, 1 + (r & 1));
            break;
          case 150:  // Zombie
            givePlayerItem(killer, I_rotten_flesh, r % 3);
            break;
          case 115:  // Skeleton
            givePlayerItem(killer, I_bone, 1 + (r & 1));
            if ((r >> 2) & 1) givePlayerItem(killer, I_arrow, 1 + (r & 1));
            break;
          case 124:  // Spider
            if ((r >> 2) & 1) givePlayerItem(killer, I_string, 1 + (r & 1));
            break;
          default:
            break;
        }
      }
    } else {
      mob_data[mob_idx].data = (mob_data[mob_idx].data & ~31) | (hp - damage);
    }

    for (uint8_t i = 0; i < kMaxClients; i++) {
      if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
      PacketCodec oc(clients_[i].fd);
      sendDamageEvent_(oc, entity_id, damage_type);
      if ((mob_data[mob_idx].data & 31) == 0) sendEntityEvent_(oc, entity_id, 3);
    }
    return;
  }

  // --- 玩家伤害处理 ---
  if (entity_id < 0 || entity_id >= kMaxClients) return;
  if (!clients_[entity_id].used || clients_[entity_id].player_index < 0) return;
  PlayerData* player = &player_data[clients_[entity_id].player_index];
  if (player->health == 0) return;

  // 状态效果已删除：这里只剩附魔带来的固定加伤（锋利，见 handleCommand_ 的 /enchant）

  if (attacker_slot >= 0 && attacker_slot < kMaxClients && clients_[attacker_slot].player_index >= 0) {
    PlayerData* attacker = &player_data[clients_[attacker_slot].player_index];
    if (attacker->flags & 0x01) return;
    uint16_t held = attacker->inventory_items[attacker->hotbar];
    if (held == I_wooden_sword || held == I_golden_sword) damage *= 4;
    else if (held == I_stone_sword) damage *= 5;
    else if (held == I_iron_sword) damage *= 6;
    else if (held == I_diamond_sword) damage *= 7;
    else if (held == I_netherite_sword) damage *= 8;
    // 附魔锋利：每级额外加伤（只对手持武器生效，等级来自附魔台/铁砧，书拿在手上不加伤）
    bool is_weapon = (held >= 937 && held <= 946) || (held >= I_wooden_sword && held <= I_golden_axe);
    uint8_t lvl = is_weapon ? (uint8_t)enchantGet_((uint8_t)attacker_slot, attacker->hotbar) : 0;
    if (lvl > 0) {
      uint16_t total = (uint16_t)damage + (uint16_t)(lvl * 2);
      damage = (uint8_t)(total > 255 ? 255 : total);
    }
    attacker->flags |= 0x01; attacker->flagval_8 = 0;
  }

  bool was_alive = (player->health > 0);
  if (player->health <= damage) player->health = 0;
  else player->health -= damage;
  if (was_alive && player->health == 0) {
    dropAllInventory_(player);                                   // 死亡爆背包 → 变成地上的掉落物
    worldStoreSavePlayer(clients_[entity_id].player_index);       // 空背包也存一次，防止重进捡回旧物品
    worldStoreFlush();
  }

  PacketCodec pc(clients_[entity_id].fd);
  sendSetHealth_(pc, player->health, player->hunger, player->saturation);

  for (uint8_t i = 0; i < kMaxClients; i++) {
    if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
    PacketCodec oc(clients_[i].fd);
    sendDamageEvent_(oc, entity_id, damage_type);
    if (player->health == 0) sendEntityEvent_(oc, entity_id, 3);
  }
}

void MinecraftServer::broadcastPlayerMetadata_(PlayerData* player) {
  uint8_t sneaking = (player->flags & 0x04) != 0;
  uint8_t sprinting = (player->flags & 0x08) != 0;
  uint8_t entity_bit_mask = 0;
  if (sneaking) entity_bit_mask |= 0x02;
  if (sprinting) entity_bit_mask |= 0x08;
  {
    int gpi = slotIndexForPlayer_(player);
    if (gpi >= 0 && gpi < MAX_PLAYERS && g_gliding[gpi]) entity_bit_mask |= 0x80;   // fall flying
  }

  int pi = slotIndexForPlayer_(player);
  if (pi < 0) return;

  for (uint8_t i = 0; i < kMaxClients; i++) {
    if (!clients_[i].used || clients_[i].state != STATE_PLAY || (int)i == pi) continue;
    PacketCodec oc(clients_[i].fd);
    int eid_size = oc.sizeVarInt((uint32_t)pi);
    uint32_t pkt_len = oc.sizeVarInt(g_entity_data_id) + eid_size + 1 + 1 + 1 + 1;
    oc.writeVarInt(pkt_len);
    oc.writeVarInt(g_entity_data_id);
    oc.writeVarInt((uint32_t)pi);
    oc.writeByte(0);
    oc.writeVarInt(0);
    oc.writeByte(entity_bit_mask);
    oc.writeByte(0xFF);
  }
}

// ------------------------------------------------------------------ 物品组件补丁解析
// container_click 的 changes 里，每格物品都会带一份组件补丁。以前每个组件只 skip 1 字节，
// 但文本组件（custom_name）和附魔组件都不止 1 字节：位置一错就把后面的“删除表”
// 当成下一格，接着越界读 → 服务端卡在 recv 里等 5 秒 → 客户端报“连接超时”。
// 现在按组件 id 逐个读准，遇到不认识的组件就停下（调用方负责跳过本包剩余字节）。

// 文本组件在网络上是“未命名 NBT 标签”：类型字节 8(TAG_String) + 2 字节长度 + UTF-8
static bool readTextComp_(PacketCodec& c, char* out, size_t out_len) {
  uint8_t type = 0;
  if (!c.readByte(type)) return false;
  if (type != 8) return false;                 // 只认纯字符串组件
  uint16_t n = 0;
  if (!c.readUint16(n)) return false;
  if (n > 512) return false;
  size_t copy = 0;
  if (out != nullptr && out_len > 0) {
    copy = n;
    if (copy >= out_len) copy = out_len - 1;
  }
  if (copy > 0 && !c.readExact((uint8_t*)out, copy)) return false;
  if (n > copy && !c.skipBytes(n - copy)) return false;
  if (out != nullptr && out_len > 0) out[copy] = 0;
  return true;
}

// ItemEnchantments：VarInt 条数 + 每条 (VarInt 附魔 id, VarInt 等级)
// 以前只留了“锋利”的等级，别的附魔（尤其创造给的附魔书）全被当成锋利或干脆丢了；
// 现在把附魔 id 也带出来，重发这一格时能原样还原。
static bool readEnchMap_(PacketCodec& c, uint8_t* level_out, uint8_t* id_out = nullptr) {
  int32_t n = 0;
  if (!c.readVarInt(n)) return false;
  if (n < 0 || n > 32) return false;
  uint8_t sharp = 0;
  uint8_t first_id = 0, first_lv = 0;
  for (int32_t i = 0; i < n; i++) {
    int32_t id = 0, lv = 0;
    if (!c.readVarInt(id) || !c.readVarInt(lv)) return false;
    if (lv > 0 && lv < 256) {
      if (first_id == 0) { first_id = (uint8_t)(id > 255 ? 33 : id); first_lv = (uint8_t)lv; }
      if (id == 33) sharp = (uint8_t)lv;   // sharpness 优先
    }
  }
  if (level_out) *level_out = (sharp != 0) ? sharp : first_lv;
  if (id_out) *id_out = (sharp != 0) ? 33 : first_id;
  return true;
}

// 读一个组件：id -> 值。返回 false 表示不认识这个组件（必须停止解析本包）。
static bool readOneComponent_(PacketCodec& c, int32_t id, char* name_out, size_t name_len,
                              uint8_t* ench_out, uint8_t* ench_id_out) {
  int32_t v = 0;
  switch (id) {
    case 2:  case 3:  case 19:                    // max_damage / damage / repair_cost
      return c.readVarInt(v);
    case 4:  case 21: {                            // unbreakable / enchantment_glint_override
      uint8_t b = 0;
      return c.readByte(b);
    }
    case 6:  case 9:                               // custom_name / item_name
      return readTextComp_(c, name_out, name_len);
    case 11: {                                     // lore：文本组件列表
      int32_t n = 0;
      if (!c.readVarInt(n)) return false;
      if (n < 0 || n > 32) return false;
      for (int32_t i = 0; i < n; i++) if (!readTextComp_(c, nullptr, 0)) return false;
      return true;
    }
    case 12:                                       // rarity
      return c.readVarInt(v);
    case 13: case 42:                              // enchantments / stored_enchantments
      return readEnchMap_(c, ench_out, ench_id_out);
    default:
      return false;
  }
}

bool MinecraftServer::handleClickContainer_(uint8_t slot_idx, PacketCodec& codec, int32_t packet_len) {
  ClientSlot& slot = clients_[slot_idx];
  size_t read_start = codec.readCount();
  PlayerData* player = (slot.player_index >= 0) ? &player_data[slot.player_index] : nullptr;
  if (!player) return codec.skipBytes((size_t)packet_len);

  int32_t window_id; codec.readVarInt(window_id);
  int32_t state_id; codec.readVarInt(state_id);
  uint16_t clicked_slot_raw; codec.readUint16(clicked_slot_raw);
  int16_t clicked_slot = (int16_t)clicked_slot_raw;
  uint8_t button; codec.readByte(button);
  int32_t mode_i; codec.readVarInt(mode_i);
  uint8_t mode = (uint8_t)mode_i;
  int32_t changes_count; codec.readVarInt(changes_count);

  PacketCodec pc(slot.fd);
  mcDiag_("[CLICK] win=%d click=%d btn=%u mode=%u n=%ld state=%ld",
          (int)window_id, (int)clicked_slot, (unsigned)button, (unsigned)mode,
          (long)changes_count, (long)state_id);
  uint8_t apply_changes = 1;
  if (window_id == 31) {                       // 锻造台（通用 9x3）
    uint8_t *buf = g_menu31[slot_idx];
    for (int32_t ci2 = 0; ci2 < changes_count; ci2++) {
      uint16_t cs; codec.readUint16(cs);
      uint8_t has; codec.readByte(has);
      uint8_t s31 = (uint8_t)cs;
      if (!has) { if (s31 < 27) memset(buf + s31 * 3, 0, 3); continue; }
      int32_t iid; codec.readVarInt(iid);
      int32_t icnt; codec.readVarInt(icnt);
      int32_t ad; codec.readVarInt(ad);
      // HashedPatchMap：每个组件只跟 4 字节哈希，不是组件本体
      for (int32_t c = 0; c < ad; c++) { int32_t tt; codec.readVarInt(tt); codec.skipBytes(4); }
      int32_t rm; codec.readVarInt(rm);
      for (int32_t c = 0; c < rm; c++) { int32_t tt; codec.readVarInt(tt); }
      if (s31 < 27 && icnt > 0) {
        uint16_t it = (uint16_t)iid; uint8_t cc = (uint8_t)icnt;
        memcpy(buf + s31 * 3, &it, 2); memcpy(buf + s31 * 3 + 2, &cc, 1);
      }
    }
    if (clicked_slot == 3 && mode == 0) {     // 取结果：0=模板 1=底材 2=附加 3=结果
      uint16_t r_item; uint8_t r_count;
      memcpy(&r_item, buf + 9, 2); memcpy(&r_count, buf + 11, 1);
      if (r_item != 0 && r_count > 0) {
        int fs = -1;
        for (int k = 0; k < 36; k++) if (player->inventory_items[k] == 0) { fs = k; break; }
        if (fs >= 0) {
          player->inventory_items[fs] = r_item;
          player->inventory_count[fs] = r_count;
          sendSetContainerSlotEnch_(pc, 0, serverSlotToClientSlot(0, (uint8_t)fs), r_count, r_item,
                                    enchantGet_(slot_idx, (uint8_t)fs));
        }
        memset(buf + 3, 0, 3);                // 底材
        memset(buf + 6, 0, 3);                // 附加
      }
    }
    memset(buf + 9, 0, 3);                    // 重算结果预览
    uint16_t base_item; uint8_t base_c;
    memcpy(&base_item, buf + 3, 2); memcpy(&base_c, buf + 5, 1);
    if (base_item != 0 && base_c > 0) {
      uint16_t add_item; uint8_t add_c;
      memcpy(&add_item, buf + 6, 2); memcpy(&add_c, buf + 8, 1);
      uint8_t out_c = base_c;
      if (add_item == base_item) {
        uint16_t tot = (uint16_t)base_c + add_c;
        out_c = (uint8_t)(tot > 64 ? 64 : tot);
      }
      uint16_t oi = base_item;
      memcpy(buf + 9, &oi, 2); memcpy(buf + 11, &out_c, 1);
    }
    for (int i = 0; i < 27; i++) {
      uint16_t it = 0; uint8_t cc = 0;
      memcpy(&it, buf + i * 3, 2); memcpy(&cc, buf + i * 3 + 2, 1);
      sendSetContainerSlot_(pc, 31, (uint16_t)i, cc, it);
    }
    return true;
  }


  // 点击合成输出槽 (slot 0) 时由服务器处理, 不接受客户端的变化
  // mode 0 = 普通点击（产物进手上），mode 1 = Shift 点击（产物直接进背包）
  if ((window_id == 0 || window_id == 12) && clicked_slot == 0 && (mode == 0 || mode == 1)) {
    uint8_t lattice = (window_id == 0) ? 2 : 3;   // 背包只能 2×2，工作台 3×3
    uint8_t out_count; uint16_t out_item;
    getCraftingOutput(player, lattice, lattice, &out_count, &out_item);
    if (out_item != 0 && out_count > 0) {
      // 消耗合成格材料
      for (int i = 0; i < 9; i++) {
        if (player->craft_items[i] != 0) {
          player->craft_count[i]--;
          if (player->craft_count[i] == 0) player->craft_items[i] = 0;
        }
      }
      if (mode == 1) {
        givePlayerItem(player, out_item, out_count);          // Shift 点击：直接进背包
      } else if (player->flagval_16 == 0) {
        player->flagval_16 = out_item;
        player->flagval_8 = out_count;
      } else if (player->flagval_16 == out_item) {
        player->flagval_8 += out_count;
      }
      // 重新计算合成输出并同步
      uint8_t new_count; uint16_t new_item;
      getCraftingOutput(player, lattice, lattice, &new_count, &new_item);
      sendSetContainerSlot_(pc, window_id, 0, new_count, new_item);
      for (int i = 0; i < 9; i++) {
        uint16_t cs = serverSlotToClientSlot(window_id, (uint8_t)(41 + i));
        // 玩家背包的 2×2 只有 4 个格子，多出来的下标映射结果是 255，
        // 以前直接把 255 当槽位发出去 → 客户端 IndexOutOfBounds(255) → 网络协议错误掉线
        if (cs == 255) continue;
        sendSetContainerSlot_(pc, window_id, cs, player->craft_count[i], player->craft_items[i]);
      }
    }
    // 读完剩余数据 (changes + cursor)
    apply_changes = 0;
  }

  // 熔炉产物槽（窗口 14 的客户端槽 2）：必须和合成输出一样，在“应用客户端改动”之前结算。
  // 以前这段放在应用改动之后：客户端每次点击都会把它自己那份原料/燃料数量发回来，
  // 先把服务端刚扣掉的数量还原，再扣一次 → 同一批原料可以无限取产物。
  if (window_id == 14 && clicked_slot == 2 && (mode == 0 || mode == 1)) {
    // 产物槽里是服务端算的“预览”。客户端一次能拿走多少就是多少：
    // mode 0 拿进鼠标手上（手上空间不够就少拿），mode 1 (Shift) 直接进背包。
    uint8_t want = player->craft_count[2];
    if (mode == 0) {
      uint8_t room = 64;
      if (player->flagval_16 == player->craft_items[2] && player->flagval_8 > 0 &&
          player->flagval_8 < 64) {
        room = (uint8_t)(64 - player->flagval_8);
      }
      if (want > room) want = room;
    }
    uint16_t got_item = 0;
    uint8_t got_count = furnaceTake(player, want, &got_item);
    if (got_item != 0 && got_count > 0) {
      if (mode == 1) {
        givePlayerItem(player, got_item, got_count);       // Shift 点击：直接进背包
      } else {
        // 鼠标手上那份由客户端自己放（本包 carriedItem 会同步成客户端的真值），
        // 服务端这里只维护镜像，绝不再自己造一份塞进背包
        if (player->flagval_16 == 0) {
          player->flagval_16 = got_item; player->flagval_8 = got_count;
        } else if (player->flagval_16 == got_item) {
          uint16_t total = (uint16_t)player->flagval_8 + got_count;
          player->flagval_8 = (uint8_t)(total > 64 ? 64 : total);
        }
      }
    }
    // 产物槽按剩余原料重算预览，三格都按服务端权威重发
    player->craft_items[2] = 0; player->craft_count[2] = 0;
    uint8_t pv_count = 0; uint16_t pv_item = 0;
    furnacePreview(player, &pv_count, &pv_item);
    player->craft_items[2] = pv_item; player->craft_count[2] = pv_count;
    for (int i = 0; i < 3; i++)
      sendSetContainerSlot_(pc, window_id, (uint16_t)i, player->craft_count[i], player->craft_items[i]);
    mcDiag_("[FURN] take click=%d want=%u got=%u x%u in=%u/%u fu=%u/%u pv=%u",
            (int)mode, (unsigned)want, (unsigned)got_count, (unsigned)got_item,
            (unsigned)player->craft_items[0], (unsigned)player->craft_count[0],
            (unsigned)player->craft_items[1], (unsigned)player->craft_count[1],
            (unsigned)pv_count);
    apply_changes = 0;
  }

  if (mode == 4 && clicked_slot != -999) {
    uint8_t s = clientSlotToServerSlot(window_id, (uint8_t)clicked_slot);
    if (s == 255) {                       // 没有对应服务端槽位：什么都不发，别再越界读 inventory[255]
      apply_changes = 0;
    } else if (window_id == 2 && slot.chest_buf && s >= 41 && s <= 67) {
      uint8_t k = (uint8_t)(s - 41);
      uint16_t it = 0; uint8_t cc = 0;
      if (k < 27) { memcpy(&it, slot.chest_buf + k * 3, 2); memcpy(&cc, slot.chest_buf + k * 3 + 2, 1); }
      sendSetContainerSlot_(pc, window_id, clicked_slot_raw, cc, it);
    } else {
      sendSetContainerSlot_(pc, window_id, clicked_slot_raw, player->inventory_count[s], player->inventory_items[s]);
    }
    apply_changes = 0;
  } else if (mode == 0 && clicked_slot == -999) {
    if (button == 0) {
      givePlayerItem(player, player->flagval_16, player->flagval_8);
      player->flagval_16 = 0; player->flagval_8 = 0;
    } else {
      givePlayerItem(player, player->flagval_16, 1);
      player->flagval_8 -= 1;
      if (player->flagval_8 == 0) player->flagval_16 = 0;
    }
    apply_changes = 0;
  }

  // 变更列表：客户端把每格的新内容发回来。必须整段读完（否则下一个包会从本包中间开始读）。
  // 一旦组件读不懂就停止应用这包改动，并把剩余字节吃掉，绝不能让解析位置错位。
  bool parse_ok = (changes_count >= 0 && changes_count <= 64);
  uint64_t changed_mask = 0;                 // 本包里动过的背包格（0..35），给“待携带”兜底用
  for (int32_t i = 0; parse_ok && i < changes_count; i++) {
    uint16_t change_slot = 0;
    if (!codec.readUint16(change_slot)) { parse_ok = false; break; }
    uint8_t s = clientSlotToServerSlot(window_id, (uint8_t)change_slot);

    uint8_t *chest_slot = nullptr;
    if (window_id == 2 && slot.chest_buf && s >= 41 && s <= 67) {
      uint8_t k = (uint8_t)(s - 41);
      if (k < 27) chest_slot = slot.chest_buf + k * 3;
    }

    uint16_t *p_item = nullptr;
    uint8_t  *p_count = nullptr;
    if (s < 41) {
      p_item  = &player->inventory_items[s];
      p_count = &player->inventory_count[s];
    } else if (s < 50) {
      // 合成格 41-49 -> craft_items[0-8]
      p_item  = &player->craft_items[s - 41];
      p_count = &player->craft_count[s - 41];
    }

    uint8_t has_item = 0;
    if (!codec.readByte(has_item)) { parse_ok = false; break; }

    if (!has_item) {
      // 清空的是附魔台/铁砧/锻造台的格子：里面那件东西的附魔和待应用名字先记成“待携带”，
      // 等它落进背包某一格再贴上去。不能指望客户端把组件补丁送回来（日志里就是 ok=0）。
      if (slot.player_index >= 0 && p_item && *p_item != 0) {
        CarryItem& cy = g_carry[slot_idx];
        if (!cy.valid) {                              // 已有的（铁砧产物）优先，别被清空动作覆盖掉
          uint8_t mlv = 0;
          const char* mn = nullptr;
          if (s >= 41 && s < 50) {
            mlv = g_craft_ench[slot_idx][s - 41];
            const char* pn = g_pending_name[slot.player_index];
            if (window_id == 8 && s == 41) {
              if (pn[0] != 0) mn = pn;
              else if (g_item_name[slot.player_index][41][0] != 0) mn = g_item_name[slot.player_index][41];
            }
          } else if (s < 41) {
            mlv = enchantGet_(slot_idx, s);
            if (g_item_name[slot.player_index][s][0] != 0) mn = g_item_name[slot.player_index][s];
          }
          if (mlv > 0 || mn != nullptr) {             // 没名字没附魔的东西不用“携带”
            cy.item = *p_item;
            cy.lv = mlv;
            cy.id = (uint8_t)kEnchSharpnessId;
            cy.name[0] = 0;
            cy.valid = 1;
            metaRemember_(slot.player_index, cy.item, cy.lv, (cy.name[0] != 0) ? cy.name : nullptr);
            mcDiag_("[CARRY] from=%u item=%u lv=%u name=%s",
                    (unsigned)s, (unsigned)cy.item, (unsigned)cy.lv, cy.name);
          }
        }
      }
      if (apply_changes) {
        if (chest_slot) { memset(chest_slot, 0, 3); }
        else if (p_item) { *p_item = 0; *p_count = 0; }
      }
      if (s < 41 && slot.player_index >= 0) g_item_name[slot.player_index][s][0] = 0;
      if (s >= 41 && s < 50) g_craft_ench[slot_idx][s - 41] = 0;   // 必须带下限，否则负下标越界
      continue;
    }

    int32_t item_id = 0, item_count = 0, comp_rem = 0, comp_add = 0;
    if (!codec.readVarInt(item_id) || !codec.readVarInt(item_count)) { parse_ok = false; break; }

    // 就算后面的组件读不懂，也必须把这一格的 id/数量记下来：
    // 组件丢失最多是名字/附魔没了，物品绝不能凭空消失（“一放进物品栏就被没收”就是这么来的）。
    // 26.1.2 的 container_click 里每格是「HashedStack」：
    //   数量 + 物品 id + HashedPatchMap（组件 id + **4 字节哈希**，没有组件本体）
    // 客户端从这一版起**不再把名字/附魔发回来**，只有哈希。以前按"组件本体"去读，
    // 读位立刻错位（日志 [CHG] ok=0 就是这个），后面每一格都读到垃圾。
    bool comp_ok = true;
    char nm[32]; nm[0] = 0;                  // 这一包拿不到名字：名字由服务端自己记（g_carry）
    uint8_t ench_lv = 0, ench_id = kEnchSharpnessId;
    if (!codec.readVarInt(comp_add) || comp_add < 0 || comp_add > 64) { parse_ok = false; break; }
    for (int32_t c = 0; c < comp_add; c++) {
      int32_t cid = 0;
      if (!codec.readVarInt(cid)) { parse_ok = false; break; }
      if (!codec.skipBytes(4)) { parse_ok = false; break; }    // 4 字节哈希
    }
    if (!parse_ok) break;
    if (!codec.readVarInt(comp_rem) || comp_rem < 0 || comp_rem > 64) { parse_ok = false; break; }
    for (int32_t c = 0; c < comp_rem; c++) {
      int32_t cid = 0;
      if (!codec.readVarInt(cid)) { parse_ok = false; break; }
    }

    if (apply_changes && item_count > 0 &&
        !(item_id == I_potion || item_id == I_splash_potion || item_id == I_lingering_potion)) {  // 药水已下线
      uint16_t prev_item = 0;
      if (s < 36) changed_mask |= (1ULL << s);
      if (chest_slot) {
        uint16_t it = (uint16_t)item_id;
        uint8_t  cc = (uint8_t)item_count;
        memcpy(chest_slot, &it, 2);
        memcpy(chest_slot + 2, &cc, 1);
      } else if (p_item) {
        prev_item = *p_item;
        *p_item  = (uint16_t)item_id;
        uint8_t mx = getItemStackSize((uint16_t)item_id);
        uint8_t cnt = (uint8_t)item_count;
        if (mx == 0) mx = 64;
        if (cnt > mx) {                       // 剑/甲这种不可堆叠的，最多 1
          mcDiag_("[CLAMP] s=%u item=%u cnt=%u -> %u", (unsigned)s, (unsigned)item_id,
                  (unsigned)cnt, (unsigned)mx);
          cnt = mx;
        }
        *p_count = cnt;
      }
      // 物品名 / 附魔跟着格子一起记下来：之后服务端重新下发这一格时才能带上它们
      if (s < 41 && slot.player_index >= 0) {
        // 这一格确实换成了另一件物品 → 旧名字/旧附魔不能跟着它；
        // 同一件物品在格子里挪来挪去（prev_item == item_id）则原样保留。
        if (prev_item != 0 && prev_item != (uint16_t)item_id) {
          g_item_name[slot.player_index][s][0] = 0;
          applyEnchant_(slot_idx, s, kEnchSharpnessId, 0);
        }
      }
      if (s >= 41 && s < 50) {
        uint8_t& clv = g_craft_ench[slot_idx][s - 41];
        CarryItem& cy = g_carry[slot_idx];
        if (prev_item != 0 && prev_item != (uint16_t)item_id) clv = 0;   // 换了另一件东西
        if (window_id == 8 && s == 41 && slot.player_index >= 0 && cy.valid &&
            cy.item == (uint16_t)item_id && cy.name[0] != 0) {
          strlcpy(g_item_name[slot.player_index][41], cy.name, sizeof(g_item_name[0][0]));
        }
        if (cy.valid && cy.item == (uint16_t)item_id && cy.lv > clv) {
          clv = cy.lv;
          mcDiag_("[CARRY] into s=%u item=%u lv=%u", (unsigned)s, (unsigned)cy.item, (unsigned)cy.lv);
          cy.valid = 0;
        }
        // 从鼠标/别的格子放进来的东西：这一格没有记录，就按物品记忆补回来
        // （“附魔后放回铁砧框里附魔就没了”就是这里没补）
        if (clv == 0 && slot.player_index >= 0) {
          uint8_t ml = 0; const char* mn2 = nullptr;
          if (metaRecall_(slot.player_index, (uint16_t)item_id, &ml, &mn2) && ml > clv) {
            clv = ml;
            mcDiag_("[META] slot=%u item=%u lv=%u (recall)", (unsigned)s, (unsigned)item_id, (unsigned)ml);
          }
          if (window_id == 8 && s == 41 && mn2 != nullptr &&
              g_item_name[slot.player_index][41][0] == 0) {
            strlcpy(g_item_name[slot.player_index][41], mn2, sizeof(g_item_name[0][0]));
          }
        }
        // 创造模式给的是裸 1245（原版这个物品本来就不带附魔 = 废纸），补锋利 I，
        // 客户端只有拿到带附魔的书，铁砧本地才会算出产物
        if (clv == 0 && (uint16_t)item_id == I_enchanted_book) clv = 1;
        if (slot.player_index >= 0 && (clv > 0 || g_item_name[slot.player_index][41][0] != 0)) {
          metaRemember_(slot.player_index, (uint16_t)item_id, clv,
                        (window_id == 8 && s == 41 && g_item_name[slot.player_index][41][0] != 0)
                            ? g_item_name[slot.player_index][41] : nullptr);
        }
      }
      // 裸附魔书（1245）落进背包任何一格都补锋利 I：创造模式给的就是空书，
      // 不补的话客户端手里那本是"废纸"，铁砧本地永远算不出产物
      if (s < 41 && slot.player_index >= 0 && (uint16_t)item_id == I_enchanted_book &&
          enchantGet_(slot_idx, (uint8_t)s) == 0) {
        applyEnchant_(slot_idx, (uint8_t)s, kEnchSharpnessId, 1);
        mcDiag_("[BOOK] s=%u -> sharpness I", (unsigned)s);
        sendSetContainerSlotEnch_(pc, 0, serverSlotToClientSlot(0, (uint8_t)s),
                                  player->inventory_count[s], player->inventory_items[s], 1,
                                  nullptr, (uint8_t)kEnchSharpnessId);
      }
      // 待携带的附魔/名字落到这一格：item id 对得上就贴上去
      if (s < 41 && slot.player_index >= 0) {
        CarryItem& cy = g_carry[slot_idx];
        if (cy.valid && cy.item == (uint16_t)item_id) {
          if (cy.lv > 0) applyEnchant_(slot_idx, s, cy.id, cy.lv);
          if (cy.name[0] != 0) strlcpy(g_item_name[slot.player_index][s], cy.name, sizeof(g_item_name[0][0]));
          mcDiag_("[CARRY] apply -> s=%u item=%u lv=%u name=%s",
                  (unsigned)s, (unsigned)cy.item, (unsigned)cy.lv, cy.name);
          cy.valid = 0;
        }
        // 上鼠标中转再放下：没有 carry 可贴，就用物品记忆把名字/附魔补回来
        uint8_t lv_now = (uint8_t)enchantGet_(slot_idx, (uint8_t)s);
        bool has_nm = (g_item_name[slot.player_index][s][0] != 0);
        if (!has_nm && lv_now == 0) {
          uint8_t ml2 = 0; const char* mn3 = nullptr;
          if (metaRecall_(slot.player_index, (uint16_t)item_id, &ml2, &mn3) &&
              (ml2 > 0 || mn3 != nullptr)) {
            if (ml2 > 0) applyEnchant_(slot_idx, (uint8_t)s, kEnchSharpnessId, ml2);
            if (mn3 != nullptr) strlcpy(g_item_name[slot.player_index][s], mn3, sizeof(g_item_name[0][0]));
            sendSetContainerSlotEnch_(pc, 0, serverSlotToClientSlot(0, (uint8_t)s),
                                      player->inventory_count[s], player->inventory_items[s],
                                      ml2, mn3, (uint8_t)kEnchSharpnessId);
            mcDiag_("[META] restore s=%u item=%u lv=%u name=%s", (unsigned)s, (unsigned)item_id,
                    (unsigned)ml2, (mn3 != nullptr) ? mn3 : "-");
          }
        } else {
          metaRemember_(slot.player_index, (uint16_t)item_id, lv_now, has_nm ? g_item_name[slot.player_index][s] : nullptr);
        }
      }
      mcDiag_("[CHG] win=%d s=%u item=%d cnt=%d lv=%u id=%u prev=%u ok=%d name=%s",
              (int)window_id, (unsigned)s, (int)item_id, (int)item_count,
              (unsigned)ench_lv, (unsigned)ench_id, (unsigned)prev_item,
              comp_ok ? 1 : 0, nm);
    }

    if (!comp_ok) { parse_ok = false; break; }
  }
  if (slot.player_index >= 0) {
    CarryItem& cy = g_carry[slot_idx];
    if (cy.valid) {
      for (uint8_t k = 0; k < 36; k++) {
        if (!(changed_mask & (1ULL << k))) continue;
        if (player->inventory_items[k] != cy.item || player->inventory_count[k] == 0) continue;
        if (cy.lv > 0) applyEnchant_(slot_idx, k, cy.id, cy.lv);
        if (cy.name[0] != 0) strlcpy(g_item_name[slot.player_index][k], cy.name, sizeof(g_item_name[0][0]));
        mcDiag_("[CARRY] flush -> s=%u item=%u lv=%u name=%s",
                (unsigned)k, (unsigned)cy.item, (unsigned)cy.lv, cy.name);
        // 立刻按服务端权威重发这一格（带名字+附魔），让背包和手持两份一致
        sendSetContainerSlotEnch_(pc, 0, serverSlotToClientSlot(0, k),
                                  player->inventory_count[k], player->inventory_items[k],
                                  enchantGet_(slot_idx, k),
                                  g_item_name[slot.player_index][k][0] ? g_item_name[slot.player_index][k] : nullptr,
                                  enchantIdGet_(slot_idx, k));
        cy.valid = 0;
        break;
      }
    }
  }

  if (!parse_ok) {
    // 组件不认识：这包的改动不用了，但流必须对齐
    apply_changes = 0;
  }

  if (window_id == 0 || window_id == 12) {
    uint8_t lattice = (window_id == 0) ? 2 : 3;
    uint8_t out_count; uint16_t out_item;
    getCraftingOutput(player, lattice, lattice, &out_count, &out_item);
    sendSetContainerSlot_(pc, window_id, 0, out_count, out_item);
  } else if (window_id == 8) {
    // 铁砧：0=物品 1=材料（同名物品 / 附魔书） 2=结果
    // 取走时服务端只负责“把输入消耗掉”：产物在客户端手上（客户端本地按原版算出了改名/附魔后的结果），
    // 服务端绝不再自己造一份塞进背包或结果槽，否则两边各一份必然互相覆盖。
    bool consumed = false;
    if (clicked_slot == 2 && mode == 0) {
      uint16_t i0 = player->craft_items[0], i1 = player->craft_items[1];
      uint8_t c0 = player->craft_count[0], c1 = player->craft_count[1];
      bool has_material = (i1 != 0 && c1 > 0 && (i1 == i0 || i1 == 1245));   // 1245 = enchanted_book
      // 不再要求"改过名 或 有材料"：名字改回原名时名字框是空的、槽 1 也是空的，
      // 以前这个条件不成立 → 服务端不认这次取走，输入格里的东西还在 → 关界面时又发一份（复制），
      // 表现就是"改名改不回来"。客户端只有在本地算出有效结果时才会点 2 号槽。
      if (i0 != 0 && c0 > 0) {
        if (slot.player_index >= 0) {
          uint8_t lv0 = (slot_idx < kMaxClients) ? g_craft_ench[slot_idx][0] : 0;
          uint8_t lv1 = (slot_idx < kMaxClients) ? g_craft_ench[slot_idx][1] : 0;
          uint8_t lv = (lv1 > lv0) ? lv1 : lv0;          // 铁砧取等级高的那个
          CarryItem& cy = g_carry[slot_idx];
          // 关键：清空分支（上面 has_item==0）可能已经把这个输入格的等级记进 cy 了，
          // 而那时 g_craft_ench[0] 已经被清零 → 这里的 lv 读出来是 0。
          // 以前直接 cy.lv = lv 就把 3 级盖成 0 了，表现就是"改完名附魔没了"。
          uint8_t use_lv = (lv > cy.lv) ? lv : cy.lv;
          cy.item = i0;
          cy.lv = use_lv;
          cy.id = (uint8_t)kEnchSharpnessId;
          const char* pn = g_pending_name[slot.player_index];
          if (pn[0] != 0) strlcpy(cy.name, pn, sizeof(cy.name));   // 改名优先
          // 名字框清空（改回原名）时保留物品原来的名字，不要用空名字盖掉
          cy.valid = (use_lv > 0 || cy.name[0] != 0) ? 1 : 0;
          if (cy.valid) {
            mcDiag_("[CARRY] anvil item=%u lv=%u lv0=%u lv1=%u name=%s",
                    (unsigned)i0, (unsigned)lv, (unsigned)lv0, (unsigned)lv1, cy.name);
          }
        }
        if (has_material && i1 == i0) {
          player->craft_items[1] = 0; player->craft_count[1] = 0;
        } else if (has_material && i1 == 1245) {
          if (c1 > 0) c1--;
          player->craft_count[1] = c1;
          if (c1 == 0) player->craft_items[1] = 0;
        }
        player->craft_items[0] = 0; player->craft_count[0] = 0;
        player->craft_items[2] = 0; player->craft_count[2] = 0;
        if (slot.player_index >= 0) {
          g_pending_name[slot.player_index][0] = 0;
          g_item_name[slot.player_index][41][0] = 0;
        }
        consumed = true;
      }
    }
    // 关键：**2 号（结果）槽一次都不回发**。客户端铁砧是本地算产物的，
    // 服务端把结果槽清空/覆盖一次，客户端不会自己重算 → 表现就是“附魔书放进去没反应、没有输出”。
    // 每次点铁砧都按服务端权威重发 0/1 两格（2 号结果槽始终不发，客户端自己算）：
    // 放附魔书进去时客户端手里那本是白的，只有我们重发带附魔的版本，它才会算出产物。
    // 内容没变就不重发：以前每点一次都重发 0/1 两格，客户端物品被替换
    // → 附魔提示（“锋利 I”）一直在闪。现在只在真的变了才发。
    uint8_t cw = wsBegin_((uint8_t)((slot_idx < MAX_PLAYERS) ? slot_idx : 0), 8);
    {
      uint16_t it0 = player->craft_items[0];
      uint8_t  ct0 = player->craft_count[0];
      uint8_t  lv0 = (slot_idx < kMaxClients) ? g_craft_ench[slot_idx][0] : 0;
      const char* nm0 = (slot.player_index >= 0 && g_item_name[slot.player_index][41][0])
                            ? g_item_name[slot.player_index][41] : nullptr;
      if (wsDiff_(cw, 0, it0, ct0, lv0, nm0)) {
        sendSetContainerSlotEnch_(pc, 8, 0, ct0, it0, lv0, nm0, (uint8_t)kEnchSharpnessId);
        wsSave_(cw, 0, it0, ct0, lv0, nm0);
      }
      uint16_t it1 = player->craft_items[1];
      uint8_t  ct1 = player->craft_count[1];
      uint8_t  lv1 = (slot_idx < kMaxClients) ? g_craft_ench[slot_idx][1] : 0;
      if (wsDiff_(cw, 1, it1, ct1, lv1, nullptr)) {
        sendSetContainerSlotEnch_(pc, 8, 1, ct1, it1, lv1, nullptr, (uint8_t)kEnchSharpnessId);
        wsSave_(cw, 1, it1, ct1, lv1, nullptr);
      }
    }
  } else if (window_id == 21) {
    // 锻造台：0 模板 1 底材 2 附加 3 结果
    // 规则：**必须有模板**。下界合金锭(910) + 钻石底材（工具 937..941 / 盔甲 971..974）→ 同名下界合金件
    //       其它附加只做同类合并；取走结果时消耗 1 个模板
    bool has_tpl = (player->craft_items[0] != 0 && player->craft_count[0] > 0);
    if (clicked_slot == 3 && mode == 0) {
      uint16_t base = player->craft_items[1]; uint8_t base_c = player->craft_count[1];
      uint16_t add = player->craft_items[2];  uint8_t add_c = player->craft_count[2];
      if (has_tpl && base != 0 && base_c > 0) {
        uint16_t out_item = base; uint8_t out_count = base_c;
        if (add == 910 && base >= 937 && base <= 941)       out_item = (uint16_t)(base + 5);
        else if (add == 910 && base >= 971 && base <= 974)  out_item = (uint16_t)(base + 8);
        else if (add != 0 && add == base) {
          uint16_t tot = (uint16_t)base_c + add_c;
          out_count = (uint8_t)(tot > 64 ? 64 : tot);
        }
        player->flagval_16 = out_item;      // 产物落鼠标手上，和合成/熔炉一致
        player->flagval_8 = out_count;
        // 消耗一个模板
        player->craft_count[0]--;
        if (player->craft_count[0] == 0) player->craft_items[0] = 0;
        player->craft_items[1] = 0; player->craft_count[1] = 0;
        player->craft_items[2] = 0; player->craft_count[2] = 0;
        player->craft_items[3] = 0; player->craft_count[3] = 0;
        worldStoreSavePlayer(slot.player_index);
      }
    }
    // 每次点击都重算结果格：没有模板就什么都不出（原版也是模板不给就不给结果）
    {
      uint16_t base = player->craft_items[1]; uint8_t base_c = player->craft_count[1];
      uint16_t add = player->craft_items[2];  uint8_t add_c = player->craft_count[2];
      uint16_t out_item = 0; uint8_t out_count = 0;
      bool tpl_ok = (player->craft_items[0] != 0 && player->craft_count[0] > 0);
      if (tpl_ok && base != 0 && base_c > 0) {
        out_item = base; out_count = base_c;
        if (add == 910 && base >= 937 && base <= 941)       out_item = (uint16_t)(base + 5);
        else if (add == 910 && base >= 971 && base <= 974)  out_item = (uint16_t)(base + 8);
        else if (add != 0 && add == base) {
          uint16_t tot = (uint16_t)base_c + add_c;
          out_count = (uint8_t)(tot > 64 ? 64 : tot);
        }
      }
      player->craft_items[3] = out_item; player->craft_count[3] = out_count;
      for (int i2 = 0; i2 < 4; i2++)
        sendSetContainerSlot_(pc, 21, (uint16_t)i2, player->craft_count[i2], player->craft_items[i2]);
      sendSetContainerData_(pc, 21, 0, 0);
    }  } else if (window_id == 13) {
    // 附魔台：0=物品 1=青金石，按钮由 container_button_click(0x11) 处理
    uint8_t cw13 = wsBegin_((uint8_t)((slot_idx < MAX_PLAYERS) ? slot_idx : 0), 13);
    if (wsDiff_(cw13, 0, player->craft_items[0], player->craft_count[0], g_craft_ench[slot_idx][0], nullptr)) {
      sendSetContainerSlotEnch_(pc, 13, 0, player->craft_count[0], player->craft_items[0],
                                g_craft_ench[slot_idx][0]);
      wsSave_(cw13, 0, player->craft_items[0], player->craft_count[0], g_craft_ench[slot_idx][0], nullptr);
    }
    if (wsDiff_(cw13, 1, player->craft_items[1], player->craft_count[1], 0, nullptr)) {
      sendSetContainerSlot_(pc, 13, 1, player->craft_count[1], player->craft_items[1]);
      wsSave_(cw13, 1, player->craft_items[1], player->craft_count[1], 0, nullptr);
    }
    // 数据槽的真实顺序（javap EnchantmentMenu 证实）：
    //   0,1,2 = costs[0..2]   3 = enchantmentSeed   4,5,6 = enchantClue   7,8,9 = levelClue
    // 以前整体错了一位（把“种子”写进了 cost[0]），所以会出现“没放东西按钮反而亮、放进去按钮没了”。
    {
      uint8_t has_item = (player->craft_items[0] != 0 && player->craft_count[0] > 0) ? 1 : 0;
      sendSetExperience_(pc, 0.0f, 30, 30);
      for (int i2 = 0; i2 < 3; i2++) {
        // 费用只看“格里有没有东西”；青金石不够时按钮会在客户端显示为不可点（原版也是这样）
        sendSetContainerData_(pc, 13, (int16_t)i2, has_item ? (int16_t)(i2 + 1) : 0);
      }
      sendSetContainerData_(pc, 13, 3, has_item ? 3 : 0);           // 附魔种子
      for (int i2 = 0; i2 < 3; i2++) {
        sendSetContainerData_(pc, 13, (int16_t)(4 + i2), has_item ? 33 : -1);
        sendSetContainerData_(pc, 13, (int16_t)(7 + i2), has_item ? (int16_t)(i2 + 1) : 0);
      }
    }
  } else if (window_id == 14) {
    // 熔炉：产物槽的结算已经挪到上面（应用改动之前），这里只刷新预览并按服务端权威回发
    uint8_t preview_count = 0; uint16_t preview_item = 0;
    furnacePreview(player, &preview_count, &preview_item);
    player->craft_items[2] = preview_item;
    player->craft_count[2] = preview_count;
    for (int i = 0; i < 3; i++)
      sendSetContainerSlot_(pc, window_id, (uint16_t)i, player->craft_count[i], player->craft_items[i]);
  }

  // 鼠标物品（carriedItem）同样是 HashedStack：物品 id + 数量 + HashedPatchMap(4 字节哈希)
  uint8_t has_cursor = 0;
  if (!codec.readByte(has_cursor)) { has_cursor = 0; }
  if (has_cursor) {
    int32_t cursor_item = 0, cursor_count = 0, ca = 0, cr = 0;
    bool cur_ok = codec.readVarInt(cursor_item) && codec.readVarInt(cursor_count) && codec.readVarInt(ca);
    if (cur_ok && ca >= 0 && ca <= 64) {
      for (int32_t c = 0; c < ca; c++) {
        int32_t cid = 0;
        if (!codec.readVarInt(cid) || !codec.skipBytes(4)) { cur_ok = false; break; }
      }
      if (cur_ok && codec.readVarInt(cr) && cr >= 0 && cr <= 64) {
        for (int32_t c = 0; c < cr; c++) { int32_t cid = 0; if (!codec.readVarInt(cid)) { cur_ok = false; break; } }
      }
      if (cur_ok) {
        player->flagval_16 = (uint16_t)cursor_item;
        player->flagval_8 = (uint8_t)cursor_count;
        mcDiag_("[CURSOR] item=%ld cnt=%ld", (long)cursor_item, (long)cursor_count);
      }
    }
  } else {
    player->flagval_16 = 0; player->flagval_8 = 0;
  }

  // 不管前面解析成不成功，都要把本包剩下的字节吃掉：否则下一个包会从本包中间开始读
  size_t consumed = codec.readCount() - read_start;
  if (consumed < (size_t)packet_len) codec.skipBytes((size_t)packet_len - consumed);

  return true;
}


// -------------------------------------------------------------------- 指令系统
// 取一个空格分隔的参数（不改动原串，只前移指针）
static bool nextToken_(const char*& p, char* out, size_t out_len) {
  while (*p == ' ') p++;
  if (*p == '\0') return false;
  size_t i = 0;
  while (*p && *p != ' ' && i + 1 < out_len) out[i++] = *p++;
  out[i] = '\0';
  return i > 0;
}

static bool tokenIsNumber_(const char* t) {
  if (*t == '-' || *t == '+') t++;
  if (*t < '0' || *t > '9') {
    if (*t != '.') return false;
  }
  return true;
}

void MinecraftServer::setWeather_(uint8_t w, bool announce) {
  weather_ = w;
  weather_change_ms_ = millis();
  for (uint8_t i = 0; i < kMaxClients; i++) {
    if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
    if (clients_[i].player_index < 0) continue;
    PacketCodec pc(clients_[i].fd);
    // Game Event: 1 = 开始下雨 / 2 = 停止下雨。value 是“雨强”，必须是 1.0，
    // 之前发 0.0 → 客户端收到事件但雨强为 0，看起来就是 /weather 没反应。
    sendGameEvent_(pc, w == 0 ? 2 : 1, w == 0 ? 0.0f : 1.0f);
    // 7 = 雨强（RAIN_LEVEL_CHANGE），8 = 雷强（THUNDER_LEVEL_CHANGE）。
    // 只发 1/2 时部分版本不会真的把雨拉起来，这里显式给强度。
    sendGameEvent_(pc, 7, w == 0 ? 0.0f : 1.0f);
    sendGameEvent_(pc, 8, (w == 2) ? 1.0f : 0.0f);
  }
  if (announce) {
    char buf[48];
    const char* name = (w == 0) ? "clear" : ((w == 2) ? "thunder" : "rain");
    int blen = snprintf(buf, sizeof(buf), "Weather set to %s", name);
    for (uint8_t i = 0; i < kMaxClients; i++) {
      if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
      if (clients_[i].player_index < 0) continue;
      PacketCodec pc(clients_[i].fd);
      sendSystemChat_(pc, buf, (uint16_t)blen);
    }
  }
}

void MinecraftServer::broadcastTime_() {
  for (uint8_t i = 0; i < kMaxClients; i++) {
    if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
    if (clients_[i].player_index < 0) continue;
    PacketCodec pc(clients_[i].fd);
    sendUpdateTime_(pc, world_time);
  }
}

// ---- 掉落物池（只存在内存里，重启即清空）----
// 包长度自检：声明值和实际写出字节数不一致时在串口爆出来（定位“xx bytes extra”这类流失步）
static void pktCheck_(PacketCodec& codec, size_t start, uint32_t declared, const char* name) {
  size_t written = codec.writeCount() - start;
  size_t expect = (size_t)declared + (size_t)codec.sizeVarInt(declared);
  if (written != expect) {
    Serial.printf("[PKT] %s decl=%u act=%u (%s)\r\n", name,
                  (unsigned)declared, (unsigned)(written - codec.sizeVarInt(declared)),
                  written > expect ? "TOO_LONG" : "TOO_SHORT");
  }
}

bool MinecraftServer::sendEntityDataProbe_(PacketCodec& codec, uint8_t entity_id) {
  // 无副作用的探包：给自己的实体设 air_supply=300（本来就是满值）
  uint32_t payload = codec.sizeVarInt(entity_id) + 1 + codec.sizeVarInt(1) + codec.sizeVarInt(300) + 1;
  uint32_t pkt_len = codec.sizeVarInt(g_entity_data_id) + payload;
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(g_entity_data_id) &&
            codec.writeVarInt((uint32_t)entity_id) &&
            codec.writeByte(1) && codec.writeVarInt(1) && codec.writeVarInt(300) &&
            codec.writeByte(0xFF);
  pktCheck_(codec, start, pkt_len, "probe");
  pktLog_(g_entity_data_id, pkt_len);
  return ok;
}

bool MinecraftServer::sendSpawnItemEntity_(PacketCodec& codec, int id, uint8_t* uuid, double x, double y, double z) {
  // 26.1.2 实测：x/y/z 之后是 5 个字节（和原来 sendSpawnEntity_ 完全一样）。
  // 之前写成 3 个字节 + varint0(=4字节) 少 1 字节，导致后面所有包流错位，
  // 客户端才会把后续数据当成 set_entity_link 并报 "xx bytes extra"。
  // 实体类型必须用 71 = minecraft:item（69 是 minecraft:interaction 交互实体，
  // 用它会被客户端当成 Interaction，随后 metadata 字段 8 类型不符而直接断线）。
  const uint32_t kEntityTypeItem = 71;
  uint32_t payload = codec.sizeVarInt((uint32_t)id) + 16 + codec.sizeVarInt(kEntityTypeItem) + 24 + 5;
  uint32_t pkt_len = codec.sizeVarInt(0x01) + payload;
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x01) &&
            codec.writeVarInt((uint32_t)id) && codec.writeExact(uuid, 16) &&
            codec.writeVarInt(kEntityTypeItem) &&
            codec.writeDouble(x) && codec.writeDouble(y) && codec.writeDouble(z) &&
            codec.writeByte(0) && codec.writeByte(0) && codec.writeByte(0) && codec.writeByte(0) &&
            codec.writeVarInt(0);
  pktCheck_(codec, start, pkt_len, "spawnitem");
  pktLog_(0x01, pkt_len);
  return ok;
}

bool MinecraftServer::sendSetEntityDataItem_(PacketCodec& codec, int id, uint16_t item, uint8_t count) {
  // 0x63 Set Entity Data: entityId + [index(1B) + type(varint) + value] + 0xFF
  // item 实体的 item 槽在 metadata index 8，类型 7 = item stack(Slot)
  uint32_t payload = codec.sizeVarInt((uint32_t)id) + 1 + codec.sizeVarInt(7)
                   + codec.sizeVarInt(count) + codec.sizeVarInt((uint32_t)item) + 1 + 1 + 1;
  uint32_t pkt_len = codec.sizeVarInt(g_entity_data_id) + payload;
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(g_entity_data_id) &&
            codec.writeVarInt((uint32_t)id) &&
            codec.writeByte(8) && codec.writeVarInt(7) &&
            codec.writeVarInt(count) && codec.writeVarInt((uint32_t)item) &&
            codec.writeVarInt(0) && codec.writeVarInt(0) &&   // added / removed 组件数 = 0
            codec.writeByte(0xFF);
  pktCheck_(codec, start, pkt_len, "itemmeta");
  pktLog_(g_entity_data_id, pkt_len);
  return ok;
}

void MinecraftServer::dropChestContents_(int16_t x, uint8_t y, int16_t z) {
  ChestStore *cs = findChestStore(x, y, z);
  if (cs == nullptr) return;
  uint8_t dropped = 0;
  for (int i = 0; i < 27; i++) {
    uint16_t it; uint8_t cc;
    memcpy(&it, cs->items + i * 3, 2);
    memcpy(&cc, cs->items + i * 3 + 2, 1);
    if (cc == 0 || it == 0) continue;
    addDrop_(x, y, z, it, cc);       // 27 格里的东西逐格掉在箱子这一格
    dropped++;
  }
  removeChestStore(x, y, z);         // 箱子没了，内容表一起清掉
  Serial.printf("[CHEST] break at %d,%u,%d dropped %u stacks\r\n",
                (int)x, (unsigned)y, (int)z, (unsigned)dropped);
}

void MinecraftServer::addDrop_(int16_t x, uint8_t y, int16_t z, uint16_t item, uint8_t count) {
  if (item == 0 || count == 0) return;
  // 药水/状态效果已整体下线：不带药水内容物的瓶子在这里就丢掉，别在地上生成“普通药水”
  if (item == I_potion || item == I_splash_potion || item == I_lingering_potion) return;
  if (!drops_) return;
  // 挖高处方块（树顶/山脊）时，服务端这里没有重力模拟，物品会停在原地的高空坐标，
  // 而拾取判定是按这组坐标算距离的 → 人站在地面永远吸不到（玩家报的“树上的东西掉下来捡不起来”）。
  // 所以生成掉落物时先把 Y 落到“正下方第一个实心方块的上方那一格”，跟视觉落点对齐。
  {
    uint8_t ly = y;
    while (ly > 1) {
      uint8_t below = getBlockAt(x, (uint8_t)(ly - 1), z);
      if (below != B_air && !isPassableBlock(below)) break;
      ly--;
    }
    y = ly;
  }
  for (uint8_t i = 0; i < kMaxDrops; i++) {
    if (drops_[i].item != 0) continue;   // 找空位
    if ((uint8_t)(y + 1) > 255) y = 255;
    drops_[i].x = x; drops_[i].y = y; drops_[i].z = z;
    drops_[i].item = item; drops_[i].count = count;
    drops_[i].born_ms = millis();
    Serial.printf("[DROP] slot=%u item=%u n=%u @%d,%u,%d\r\n", (unsigned)i, (unsigned)item,
                  (unsigned)count, (int)x, (unsigned)y, (int)z);
    dropSpawnEntity_(i);
    return;
  }
  // 满了就覆盖最老的一个
  uint8_t oldest = 0;
  for (uint8_t i = 1; i < kMaxDrops; i++)
    if ((int32_t)(drops_[i].born_ms - drops_[oldest].born_ms) < 0) oldest = i;
  dropRemoveEntity_(oldest);
  drops_[oldest].x = x; drops_[oldest].y = y; drops_[oldest].z = z;
  drops_[oldest].item = item; drops_[oldest].count = count;
  drops_[oldest].born_ms = millis();
  dropSpawnEntity_(oldest);
}

void MinecraftServer::dropSpawnEntity_(uint8_t idx) {
  if (!drops_ || idx >= kMaxDrops) return;
  DropItem& d = drops_[idx];
  if (d.item == 0) return;
  d.entity_id = 100 + (int)idx;             // 100+ 避开玩家槽 0..5 和怪物 -2-i
  // UUID 必须每次都不一样：之前只用坐标做哈希，同一位置掉两次就是同一个 UUID，
  // 客户端会报 "Duplicate entity UUID" 并丢掉新实体（表现就是掉落物看不见）。
  static uint32_t uuid_seq = 0;
  uint32_t h = 0x9E3779B9u ^ (++uuid_seq) ^ (millis() << 4)
             ^ ((uint32_t)d.x * 73856093u) ^ ((uint32_t)d.z * 19349663u) ^ ((uint32_t)d.y << 3);
  for (uint8_t k = 0; k < 16; k++) {
    h = h * 1664525u + 1013904223u;
    d.uuid[k] = (uint8_t)(h >> 24);
  }
  for (uint8_t i = 0; i < kMaxClients; i++) {
    if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
    if (clients_[i].player_index < 0) continue;
    PacketCodec pc(clients_[i].fd);
    sendSpawnItemEntity_(pc, d.entity_id, d.uuid, d.x + 0.5, (double)d.y + 0.2, d.z + 0.5);
    sendSetEntityDataItem_(pc, d.entity_id, d.item, d.count);
  }
}

void MinecraftServer::dropRemoveEntity_(uint8_t idx) {
  if (!drops_ || idx >= kMaxDrops) return;
  DropItem& d = drops_[idx];
  if (d.item == 0) return;
  for (uint8_t i = 0; i < kMaxClients; i++) {
    if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
    if (clients_[i].player_index < 0) continue;
    PacketCodec pc(clients_[i].fd);
    sendRemoveEntity_(pc, d.entity_id);
  }
}

void MinecraftServer::tickDrops_() {
  if (!drops_) return;
  // 掉落物按时间自动消失已按要求关闭：地上的物品不会因为"超时"被删掉，
  // 只有三种情况会消失——被玩家捡走、池子（kMaxDrops 个）用完后覆盖最老的那个、
  // 或者掉电重启（掉落物只在内存里，不写 flash 存档）。
  for (uint8_t d = 0; d < kMaxDrops; d++) {
    if (drops_[d].item == 0) continue;
    // 刚掉出来 0.5 秒内不能被捡：原版丢出的物品有拾取延迟，
    // 否则 Q 丢东西 / 死亡爆背包都会瞬间被自己吸回去，看起来像"没掉出来"。
    if ((uint32_t)(millis() - drops_[d].born_ms) < 500) continue;

    for (uint8_t i = 0; i < kMaxClients; i++) {
      if (!clients_[i].used || clients_[i].state != STATE_PLAY) continue;
      if (clients_[i].player_index < 0) continue;
      PlayerData* p = &player_data[clients_[i].player_index];
      if (p->client_fd == -1) continue;
      if (p->health == 0) continue;   // 尸体不能捡东西：否则 /kill 爆出来的背包会被自己立刻吸回去
      int16_t dx = (int16_t)(p->x - drops_[d].x); if (dx < 0) dx = -dx;
      int16_t dz = (int16_t)(p->z - drops_[d].z); if (dz < 0) dz = -dz;
      int16_t dy = (int16_t)p->y - (int16_t)drops_[d].y; if (dy < 0) dy = -dy;
      // 原来半径只有 1 格、Y 只有 2 格，必须几乎站在物品那一格上才吸得到，
      // 走快了还会漏（这个判定每秒只跑一次）→ 手感就是"掉落物很难拿"。
      // 放宽到水平 2 格、垂直 3 格，走过去就会捡起来。
      if (dx > 2 || dz > 2 || dy > 3) continue;
      if (givePlayerItem(p, drops_[d].item, drops_[d].count) == 0) {
        dropRemoveEntity_(d);
        drops_[d].item = 0;
        break;
      }
    }
  }
}

// 串口调试：不开客户端也能塞一个掉落物 / 看地上还有什么。
// 位置固定在出生点附近 (5,66,5) 的空地上，只为验证"不会超时消失"。
void MinecraftServer::debugAddDrop(uint16_t item, uint8_t count) {
  addDrop_(5, 66, 5, item, count);
}

void MinecraftServer::debugPrintDrops() {
  if (!drops_) { sePrintln("[DROPS] none (池子未分配)"); return; }
  uint32_t now = millis();
  bool any = false;
  for (uint8_t d = 0; d < kMaxDrops; d++) {
    if (drops_[d].item == 0) continue;
    any = true;
    sePrintf("[DROPS] #%u item=%u n=%u @%d,%u,%d age=%lus eid=%d\r\n",
             (unsigned)d, (unsigned)drops_[d].item, (unsigned)drops_[d].count,
             (int)drops_[d].x, (unsigned)drops_[d].y, (int)drops_[d].z,
             (unsigned long)((uint32_t)(now - drops_[d].born_ms) / 1000UL),
             drops_[d].entity_id);
  }
  if (!any) sePrintln("[DROPS] none");
}

void MinecraftServer::dropAllInventory_(PlayerData* player) {
  for (int i = 0; i < 41; i++) {
    if (player->inventory_items[i] && player->inventory_count[i])
      addDrop_(player->x, player->y, player->z, player->inventory_items[i], player->inventory_count[i]);
    player->inventory_items[i] = 0;
    player->inventory_count[i] = 0;
  }
}

void MinecraftServer::killPlayer_(uint8_t slot_idx) {
  if (slot_idx >= kMaxClients) return;
  ClientSlot& slot = clients_[slot_idx];
  if (slot.player_index < 0) return;
  PlayerData* player = &player_data[slot.player_index];

  if (player->health == 0) return;
  player->health = 0;
  player->flags &= ~0x10;   // 停掉吃东西
  // 死亡后必须重新选出生点（回世界出生点），否则原地复活会第一帧就把刚爆出来的物品全捡回去，
  // 现象就是“/kill 自己没爆掉落物”。
  player->flags |= 0x02;
  s_die_x = (int16_t)player->x;
  s_die_z = (int16_t)player->z;
  s_die_ms = millis();
  dropAllInventory_(player);

  PacketCodec pc(slot.fd);
  // 背包已经清空，必须逐个格子告诉客户端，否则客户端界面上东西还在（假物品，点一下就被服务端拒绝）
  for (uint8_t i = 0; i < 41; i++)
    sendSetContainerSlot_(pc, 0, serverSlotToClientSlot(0, i), 0, 0);
  char where[72];
  int wl = snprintf(where, sizeof(where), "物品掉落在 %d, %u, %d",
                    (int)player->x, (unsigned)player->y, (int)player->z);
  sendSystemChat_(pc, where, (uint16_t)wl);
  sendSetHealth_(pc, 0, player->hunger, player->saturation);   // 客户端收到 0 血 → 死亡界面
  sendEntityEvent_(pc, slot_idx, 3);                           // 3 = 死亡动画
  worldStoreSavePlayer(slot.player_index);
  worldStoreFlush();
  Serial.printf("[CMD] kill slot=%u (%s) pos=%d,%u,%d drops done\r\n",
                (unsigned)slot_idx, player->name, (int)player->x, (unsigned)player->y, (int)player->z);
}

void MinecraftServer::teleportPlayer_(uint8_t slot_idx, int16_t tx, uint8_t ty, int16_t tz) {
  if (slot_idx >= kMaxClients) return;
  ClientSlot& slot = clients_[slot_idx];
  if (slot.player_index < 0) return;
  PlayerData* player = &player_data[slot.player_index];

  player->x = tx; player->y = ty; player->z = tz;
  player->grounded_y = ty;
  player->flags &= ~0x02;

  PacketCodec pc(slot.fd);
  sendSynchronizePlayerPosition_(pc, (double)tx + 0.5, (double)ty, (double)tz + 0.5,
                                 (float)player->yaw * 180.0f / 127.0f,
                                 (float)player->pitch * 90.0f / 127.0f);
  sendStartWaitingForChunks_(pc);

  int16_t cx = div_floor(tx, 16), cz = div_floor(tz, 16);
  sendSetCenterChunk_(pc, cx, cz);
  sendChunkDataAndUpdateLight_(pc, cx, cz);
  for (uint8_t i = 0; i < 41; i++)
    sendSetContainerSlotEnch_(pc, 0, serverSlotToClientSlot(0, i),
                               player->inventory_count[i], player->inventory_items[i],
                               enchantGet_(slot_idx, i),
                               g_item_name[slot.player_index][i][0] ? g_item_name[slot.player_index][i] : nullptr,
                               enchantIdGet_(slot_idx, i));
  sendSetHeldItem_(pc, player->hotbar);

  slot.chunk_queue_idx = 0;
  slot.chunk_center_x = cx;
  slot.chunk_center_z = cz;
  slot.chunk_next_send_ms = millis() + slot.chunk_interval_ms;
}

void MinecraftServer::setPlayerGameMode_(uint8_t slot_idx, uint8_t mode) {
  if (slot_idx >= kMaxClients) return;
  ClientSlot& slot = clients_[slot_idx];
  if (slot.player_index < 0) return;
  PlayerData* player = &player_data[slot.player_index];
  player->game_mode = (mode == 1) ? 1 : 0;
  worldStoreSavePlayer(slot.player_index);   // 立刻落盘：即使被断线/重启，重进仍是新模式
  worldStoreFlush();
  gm_last_change_ms_ = millis();
  gm_last_slot_ = slot_idx;

  Serial.printf("[CMD] gamemode slot=%u -> %s heap=%u\r\n", (unsigned)slot_idx,
                player->game_mode == 1 ? "creative" : "survival", ESP.getFreeHeap());

  // 用 Game Event(reason=3, change game mode) 直接改客户端模式：
  // 这个包 id 和格式在本项目里已被「start waiting for chunks(reason=13)」验证过，
  // 不需要 respawn，也就不会因为 respawn 包把客户端踢下线。
  {
    PacketCodec pc(slot.fd);
    sendGameEvent_(pc, 3, (float)player->game_mode);
    sendPlayerAbilities_(pc, (player->game_mode == 1) ? 0x0D : 0x00);
  }
}

// ---------------- 附魔（服务端记录 + 物品光效）----------------------------
// 说明：真实附魔组件（id 13）需要客户端动态注册表里的附魔 id，顺序随内置 datapack 变化，
// 直接猜会让客户端 DecoderException 掉线（历史上已经掉过一次）。所以这里只发
// enchantment_glint_override（组件 id 21，payload 就是一个布尔字节），
// 附魔等级由服务端自己记在 /eN.dat，攻击时按锋利算加伤。
#define kEnchantFileBytes 41   // 每个玩家 41 个格子：每格 1 字节 = 等级（0 表示没附）

static uint8_t g_enchant[MAX_PLAYERS][kEnchantFileBytes];
static uint8_t g_enchant_id[MAX_PLAYERS][kEnchantFileBytes];   // 附魔 id（0 = 没记，按锋利 33 处理）
static bool g_enchant_loaded[MAX_PLAYERS] = {false};

static void enchantFilePath_(uint8_t slot_idx, char* out, size_t n) {
  snprintf(out, n, "/e%d.dat", (int)slot_idx);
}

static void enchantLoad_(uint8_t slot_idx) {
  if (slot_idx >= MAX_PLAYERS || g_enchant_loaded[slot_idx]) return;
  g_enchant_loaded[slot_idx] = true;
  memset(g_enchant[slot_idx], 0, kEnchantFileBytes);
  char path[16];
  enchantFilePath_(slot_idx, path, sizeof(path));
  if (!SPIFFS.exists(path)) return;
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return;
  if ((size_t)f.size() >= kEnchantFileBytes) f.read(g_enchant[slot_idx], kEnchantFileBytes);
  f.close();
}

static void enchantSave_(uint8_t slot_idx) {
  if (slot_idx >= MAX_PLAYERS) return;
  char path[16];
  enchantFilePath_(slot_idx, path, sizeof(path));
  File f = SPIFFS.open(path, FILE_WRITE);
  if (!f) return;
  f.write(g_enchant[slot_idx], kEnchantFileBytes);
  f.flush();
  f.close();
}

// 串口 #INV#：服务端权威背包（含自定义名 / 附魔等级 / 合成格 / 鼠标）。
// 客户端说“附魔过一会就没了”时先看这里：这里有=服务端没丢（问题在回包/客户端），
// 这里没了=我们哪一步把它清掉了。没有 GUI 也能判断到底谁丢的。
void mcServerInvDump() {
  for (int p = 0; p < MAX_PLAYERS; p++) {
    PlayerData* pl = &player_data[p];
    if (pl->name[0] == 0) continue;
    Serial.printf("+INV:P%d name=%s hot=%u cursor=%u*%u\r\n", p, pl->name,
                  (unsigned)pl->hotbar, (unsigned)pl->flagval_16, (unsigned)pl->flagval_8);
    for (int sl = 0; sl < 41; sl++) {
      if (pl->inventory_items[sl] == 0) continue;
      const char* nm = g_item_name[p][sl];
      Serial.printf("+INV:  s%d item=%u cnt=%u lv=%u nm=%s\r\n", sl,
                    (unsigned)pl->inventory_items[sl], (unsigned)pl->inventory_count[sl],
                    (unsigned)g_enchant[p][sl], (nm[0] != 0) ? nm : "-");
    }
    for (int sl = 0; sl < 9; sl++) {
      if (pl->craft_items[sl] == 0) continue;
      Serial.printf("+INV:  craft%d item=%u cnt=%u lv=%u\r\n", sl,
                    (unsigned)pl->craft_items[sl], (unsigned)pl->craft_count[sl],
                    (unsigned)g_craft_ench[p][sl]);
    }
    Serial.printf("+INV:  carry valid=%u item=%u lv=%u\r\n",
                  (unsigned)g_carry[p].valid, (unsigned)g_carry[p].item, (unsigned)g_carry[p].lv);
  }
  Serial.println("+INV:END");
}

uint16_t MinecraftServer::enchantGet_(uint8_t slot_idx, uint8_t inv_slot) {
  if (slot_idx >= kMaxClients || inv_slot >= kEnchantFileBytes) return 0;
  enchantLoad_(slot_idx);
  return g_enchant[slot_idx][inv_slot];
}

uint8_t MinecraftServer::enchantIdGet_(uint8_t slot_idx, uint8_t inv_slot) {
  if (slot_idx >= kMaxClients || inv_slot >= kEnchantFileBytes) return kEnchSharpnessId;
  enchantLoad_(slot_idx);
  uint8_t id = g_enchant_id[slot_idx][inv_slot];
  return (id == 0) ? (uint8_t)kEnchSharpnessId : id;
}

void MinecraftServer::applyEnchant_(uint8_t slot_idx, uint8_t inv_slot, uint8_t id, uint8_t level) {
  if (slot_idx >= kMaxClients || inv_slot >= kEnchantFileBytes) return;
  enchantLoad_(slot_idx);
  g_enchant[slot_idx][inv_slot] = level;   // 等级
  g_enchant_id[slot_idx][inv_slot] = (level > 0) ? id : 0;
  enchantSave_(slot_idx);
  Serial.printf("[ENCH] slot=%u inv=%u id=%u lvl=%u\r\n",
                (unsigned)slot_idx, (unsigned)inv_slot, (unsigned)id, (unsigned)level);
}

bool MinecraftServer::runCommand_(uint8_t slot_idx, PlayerData* player, const char* cmdline) {
  while (*cmdline == ' ') cmdline++;
  if (*cmdline == '\0') return false;
  Serial.printf("[CMDLINE] slot=%u %s\r\n", (unsigned)slot_idx, cmdline);
  PacketCodec pc(clients_[slot_idx].fd);

  // ---- /help ----  列出全部可用指令（和指令树、runCommand_ 保持一致）
  if (strcmp(cmdline, "help") == 0 || strcmp(cmdline, "?") == 0) {
    static const char* kHelp[] = {
      "/gamemode <creative|survival> [player]",
      "/tp <x> <y> <z>   /tp <player>",
      "/kill [player]",
      "/time <set|add> <day|night|noon|midnight|ticks>   /time <ticks>",
      "/weather <clear|rain|thunder>",
      "/give <item name|id> [count]",
      "/drop [all]   /inv   /drops",
      "/recipes     (list what can be crafted / smelted)",
    };
    for (size_t i = 0; i < sizeof(kHelp) / sizeof(kHelp[0]); i++)
      sendSystemChat_(pc, kHelp[i], (uint16_t)strlen(kHelp[i]));
    return true;
  }

  // ---- /recipes ---- 合成表 / 烧炼表速查（和工作台、熔炉里真正能做的完全一致）
  if (strcmp(cmdline, "recipes") == 0) {
    static const char* kRecipesHelp[] = {
      "Craft (3x3 table; player inventory only fits 2x2 recipes):",
      "1 log -> 4 planks; 2 planks -> 4 sticks; 4 planks -> crafting table; 8 planks -> chest",
      "8 cobblestone -> furnace; coal/charcoal over stick -> 4 torches; 3 wheat -> bread",
      "pickaxe/axe/shovel/sword: wood, stone, iron, gold, diamond",
      "iron armor: helmet, chestplate, leggings, boots; iron V -> bucket",
      "3 in a row -> 6 slabs (oak/cobblestone/stone); 4 stone -> 4 stone bricks",
      "9 of a kind -> block (iron/gold/diamond/redstone/coal/copper), and the reverse",
      "Furnace: cobblestone->stone, sand->glass, raw iron/gold/copper->ingot,",
      "log->charcoal, beef/porkchop/chicken/mutton->cooked, clay ball->brick",
      "Fuel: coal/charcoal 8 items, coal block 80, log/planks/table/chest 2, stick/sapling 1",
    };
    for (size_t i = 0; i < sizeof(kRecipesHelp) / sizeof(kRecipesHelp[0]); i++)
      sendSystemChat_(pc, kRecipesHelp[i], (uint16_t)strlen(kRecipesHelp[i]));
    return true;
  }


  if (strncmp(cmdline, "gamemode", 8) == 0 && (cmdline[8] == '\0' || cmdline[8] == ' ')) {
    const char* p = cmdline + 8;
    char mode_tok[16];
    if (!nextToken_(p, mode_tok, sizeof(mode_tok))) {
      char out[64];
      int ol = snprintf(out, sizeof(out), "Gamemode is %s (default %s)",
                        player->game_mode == 1 ? "creative" : "survival",
                        GAMEMODE == 1 ? "creative" : "survival");
      sendSystemChat_(pc, out, (uint16_t)ol);
      return true;
    }
    uint8_t mode = 255;
    if (!strcmp(mode_tok, "creative") || !strcmp(mode_tok, "c") || !strcmp(mode_tok, "1")) mode = 1;
    else if (!strcmp(mode_tok, "survival") || !strcmp(mode_tok, "s") || !strcmp(mode_tok, "0")) mode = 0;
    if (mode == 255) {
      sendSystemChat_(pc, "Usage: /gamemode <creative|survival> [player]", 44);
      return true;
    }
    int target = (int)slot_idx;
    char who[16];
    if (nextToken_(p, who, sizeof(who))) {
      target = -1;
      for (int i = 0; i < MAX_PLAYERS; i++) {
        if (player_data[i].client_fd == -1) continue;
        if (strcmp(player_data[i].name, who) == 0) {
          target = slotIndexForPlayer_(&player_data[i]);
          break;
        }
      }
      if (target < 0) { sendSystemChat_(pc, "Player not found", 16); return true; }
    }
    setPlayerGameMode_((uint8_t)target, mode);
    return true;
  }

  // ---- /time ----   set day|noon|night|midnight|<ticks> / add <ticks> / 无参数查询
  if (strncmp(cmdline, "time", 4) == 0 && (cmdline[4] == '\0' || cmdline[4] == ' ')) {
    const char* p = cmdline + 4;
    char a[16] = {0}, b[16] = {0};
    if (!nextToken_(p, a, sizeof(a))) {
      char out[48];
      bool day = (world_time >= 0 && world_time < 13000);
      int ol = snprintf(out, sizeof(out), "Time %u (%s)", (unsigned)world_time, day ? "day" : "night");
      sendSystemChat_(pc, out, (uint16_t)ol);
      return true;
    }
    if (strcmp(a, "set") == 0) { nextToken_(p, b, sizeof(b)); }
    else { strncpy(b, a, sizeof(b) - 1); }

    uint32_t t = 0xFFFFFFFF;
    if      (strcmp(b, "day") == 0)      t = 1000;
    else if (strcmp(b, "noon") == 0)     t = 6000;
    else if (strcmp(b, "night") == 0)    t = 13000;
    else if (strcmp(b, "midnight") == 0) t = 18000;
    else if (strcmp(a, "add") == 0) {
      int add = atoi(b);
      world_time = (uint16_t)(((uint32_t)world_time + (uint32_t)add) % 24000);
      broadcastTime_();
      char out[40];
      int ol = snprintf(out, sizeof(out), "Time %u", (unsigned)world_time);
      sendSystemChat_(pc, out, (uint16_t)ol);
      return true;
    } else if (b[0] >= '0' && b[0] <= '9') t = (uint32_t)atoi(b);

    if (t == 0xFFFFFFFF) {
      sendSystemChat_(pc, "Usage: /time day|noon|night|midnight|<ticks>  or  !time add <ticks>", 62);
      return true;
    }
    world_time = (uint16_t)(t % 24000);
    broadcastTime_();
    char out[40];
    int ol = snprintf(out, sizeof(out), "Time set to %u", (unsigned)world_time);
    sendSystemChat_(pc, out, (uint16_t)ol);
    return true;
  }

  // ---- /weather clear|rain|thunder ----
  if (strncmp(cmdline, "weather", 7) == 0 && (cmdline[7] == '\0' || cmdline[7] == ' ')) {
    const char* p = cmdline + 7;
    char a[16] = {0};
    if (!nextToken_(p, a, sizeof(a))) {
      const char* name = (weather_ == 0) ? "clear" : ((weather_ == 2) ? "thunder" : "rain");
      char out[40];
      int ol = snprintf(out, sizeof(out), "Weather: %s", name);
      sendSystemChat_(pc, out, (uint16_t)ol);
      return true;
    }
    if      (strcmp(a, "clear") == 0)   setWeather_(0, true);
    else if (strcmp(a, "rain") == 0)    setWeather_(1, true);
    else if (strcmp(a, "thunder") == 0) setWeather_(2, true);
    else sendSystemChat_(pc, "Usage: /weather clear|rain|thunder", 34);
    return true;
  }

  // ---- /drop  （等同按 Q 丢一个手上物品，便于验证落点；/drop all 丢整叠）----
  if (strncmp(cmdline, "drop", 4) == 0 && (cmdline[4] == '\0' || cmdline[4] == ' ')) {
    uint8_t have = player->inventory_count[player->hotbar];
    dropHeldItem_(player, (cmdline[4] == ' ' && strncmp(cmdline + 5, "all", 3) == 0) ? have : 1);
    return true;
  }

  // ---- /inv  手上 9 格内容； /drops  地上掉落物坐标（诊断用）----
  if (!strcmp(cmdline, "inv")) {
    for (uint8_t i = 0; i < 9; i++) {
      char out[64];
      int ol = snprintf(out, sizeof(out), "hotbar%d: item=%u n=%u%s", (unsigned)i,
                        (unsigned)player->inventory_items[i],
                        (unsigned)player->inventory_count[i],
                        (i == player->hotbar) ? " *" : "");
      Serial.printf("[INV] %s\r\n", out);
      sendSystemChat_(pc, out, (uint16_t)ol);
    }
    return true;
  }
  if (!strcmp(cmdline, "drops")) {
    int shown = 0;
    for (uint8_t i = 0; i < kMaxDrops; i++) {
      if (!drops_ || drops_[i].item == 0) continue;
      char out[72];
      int ol = snprintf(out, sizeof(out), "drop#%u item=%u n=%u @%d,%u,%d", (unsigned)i,
                        (unsigned)drops_[i].item, (unsigned)drops_[i].count,
                        (int)drops_[i].x, (unsigned)drops_[i].y, (int)drops_[i].z);
      Serial.printf("[DROPS] %s\r\n", out);
      sendSystemChat_(pc, out, (uint16_t)ol);
      shown++;
      if (shown >= 12) break;
    }
    if (shown == 0) { Serial.printf("[DROPS] none\r\n"); sendSystemChat_(pc, "no drops on ground", 18); }
    return true;
  }

  // ---- /kill  或  /kill <player> ----
  if (strncmp(cmdline, "kill", 4) == 0 && (cmdline[4] == '\0' || cmdline[4] == ' ')) {
    const char* p = cmdline + 4;
    char who[16];
    if (!nextToken_(p, who, sizeof(who)) || strcmp(who, "me") == 0 || strcmp(who, "self") == 0) {
      killPlayer_(slot_idx);
      return true;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (player_data[i].client_fd == -1) continue;
      if (strcmp(player_data[i].name, who) != 0) continue;
      int ti = slotIndexForPlayer_(&player_data[i]);
      if (ti < 0) break;
      killPlayer_((uint8_t)ti);
      return true;
    }
    sendSystemChat_(pc, "Player not found", 16);
    return true;
  }

  // ---- /tp <x> <y> <z>   或   /tp <player> ----
  if (strncmp(cmdline, "tp", 2) == 0 && (cmdline[2] == '\0' || cmdline[2] == ' ')) {
    const char* p = cmdline + 2;
    char t1[24], t2[24], t3[24];
    if (!nextToken_(p, t1, sizeof(t1))) {
      sendSystemChat_(pc, "Usage: /tp <x> <y> <z>  or  /tp <player>", 45);
      return true;
    }
    if (tokenIsNumber_(t1)) {
      if (!nextToken_(p, t2, sizeof(t2)) || !nextToken_(p, t3, sizeof(t3)) ||
          !tokenIsNumber_(t2) || !tokenIsNumber_(t3)) {
        sendSystemChat_(pc, "Usage: /tp <x> <y> <z>", 23);
        return true;
      }
      double dx = atof(t1), dy = atof(t2), dz = atof(t3);
      int16_t tx = (int16_t)(dx < -30000 ? -30000 : (dx > 30000 ? 30000 : dx));
      int16_t tz = (int16_t)(dz < -30000 ? -30000 : (dz > 30000 ? 30000 : dz));
      uint8_t ty = (uint8_t)(dy < 1 ? 1 : (dy > 255 ? 255 : dy));
      teleportPlayer_(slot_idx, tx, ty, tz);
      char out[64];
      int ol = snprintf(out, sizeof(out), "Teleported to %d %d %d", (int)tx, (int)ty, (int)tz);
      sendSystemChat_(pc, out, (uint16_t)ol);
      return true;
    }
    // /tp <player>：传送到那个玩家身边
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (player_data[i].client_fd == -1) continue;
      if (strcmp(player_data[i].name, t1) != 0) continue;
      int ti = slotIndexForPlayer_(&player_data[i]);
      if (ti < 0) break;
      teleportPlayer_(slot_idx, player_data[i].x, player_data[i].y, player_data[i].z);
      char out[64];
      int ol = snprintf(out, sizeof(out), "Teleported to %s", t1);
      sendSystemChat_(pc, out, (uint16_t)ol);
      return true;
    }
    sendSystemChat_(pc, "Player not found", 16);
    return true;
  }

  // ---- /give <物品名|数字> [数量] ----  补发物品（也用来把被截断吃掉的东西要回来）
  if (strncmp(cmdline, "give", 4) == 0 && (cmdline[4] == '\0' || cmdline[4] == ' ')) {
    struct GiveN { const char* name; uint16_t id; };
    static const GiveN kGive[] = {
      {"netherite_sword", I_netherite_sword}, {"diamond_sword", I_diamond_sword},
      {"iron_sword", I_iron_sword},           {"stone_sword", I_stone_sword},
      {"wooden_sword", I_wooden_sword},       {"golden_sword", I_golden_sword},
      {"netherite_pickaxe", I_netherite_pickaxe}, {"diamond_pickaxe", I_diamond_pickaxe},
      {"iron_pickaxe", I_iron_pickaxe},       {"stone_pickaxe", I_stone_pickaxe},
      {"wooden_pickaxe", I_wooden_pickaxe},   {"stone", I_stone},
      {"dirt", I_dirt},                       {"grass_block", I_grass_block},
      {"cobblestone", I_cobblestone},         {"chest", I_chest},
      {"crafting_table", I_crafting_table},   {"ladder", I_ladder},
      {"torch", I_torch},                     {"oak_log", I_oak_log},
      {"apple", I_apple},                     {"bread", I_bread},
      {"anvil", I_anvil},                     {"oak_slab", I_oak_slab},
      {"stone_slab", I_stone_slab},           {"cobblestone_slab", I_cobblestone_slab},
      {"oak_stairs", I_oak_stairs},           {"oak_fence", I_oak_fence},
    };
    const char* p = cmdline + 4;
    char name[24];
    if (!nextToken_(p, name, sizeof(name))) {
      sendSystemChat_(pc, "Usage: /give <item name> [count]", 30);
      return true;
    }
    uint16_t gid = 0;
    for (size_t i = 0; i < sizeof(kGive) / sizeof(kGive[0]); i++)
      if (strcmp(name, kGive[i].name) == 0) { gid = kGive[i].id; break; }
    if (gid == 0 && tokenIsNumber_(name)) gid = (uint16_t)atoi(name);
    if (gid == 0) { sendSystemChat_(pc, "Unknown item", 12); return true; }
    uint8_t gcount = 1;
    char tok[16];
    if (nextToken_(p, tok, sizeof(tok)) && tokenIsNumber_(tok)) {
      int v = atoi(tok);
      gcount = (uint8_t)(v < 1 ? 1 : (v > 64 ? 64 : v));
    }
    givePlayerItem(player, gid, gcount);
    char out[72];
    int ol = snprintf(out, sizeof(out), "Gave %s x%u (id %u)", name, (unsigned)gcount, (unsigned)gid);
    sendSystemChat_(pc, out, (uint16_t)ol);
    return true;
  }

  return false;
}

bool MinecraftServer::canPlayerEat_(PlayerData* player) {
  if (player->hunger >= 20) return false;
  uint16_t held = player->inventory_items[player->hotbar];
  if (held == 0 || player->inventory_count[player->hotbar] == 0) return false;
  switch (held) {
    case I_chicken: case I_beef: case I_porkchop: case I_mutton:
    case I_cooked_chicken: case I_cooked_beef: case I_cooked_porkchop: case I_cooked_mutton:
    case I_apple:
      return true;
    default: return false;
  }
}

void MinecraftServer::doPlayerEat_(PlayerData* player) {
  if (player->hunger >= 20) return;
  uint16_t *held_item = &player->inventory_items[player->hotbar];
  uint8_t *held_count = &player->inventory_count[player->hotbar];
  if (*held_item == 0 || *held_count == 0) return;

  uint8_t food = 0; uint16_t sat = 0;
  switch (*held_item) {
    case I_chicken: food = 2; sat = 600; break;
    case I_beef: food = 3; sat = 900; break;
    case I_porkchop: food = 3; sat = 300; break;
    case I_mutton: food = 2; sat = 600; break;
    case I_cooked_chicken: food = 6; sat = 3600; break;
    case I_cooked_beef: food = 8; sat = 6400; break;
    case I_cooked_porkchop: food = 8; sat = 6400; break;
    case I_cooked_mutton: food = 6; sat = 4800; break;
    case I_apple: food = 4; sat = 1200; break;
    default: return;
  }

  player->saturation += sat;
  player->hunger += food;
  if (player->hunger > 20) player->hunger = 20;
  *held_count -= 1;
  if (*held_count == 0) *held_item = 0;

  int pi = slotIndexForPlayer_(player);
  if (pi < 0) return;
  PacketCodec pc(clients_[pi].fd);
  sendEntityEvent_(pc, pi, 9);
  sendSetHealth_(pc, player->health, player->hunger, player->saturation);
  sendHeldSlot_(pc, player, *held_count, *held_item);
}

uint8_t MinecraftServer::getArmorItemSlot_(uint16_t item) {
  // 四套护甲 + 下界合金一套；鞘翅固定穿在胸甲槽（38）。
  // 以前只认 leather/iron/golden/diamond 四套，下界合金护甲和鞘翅根本没有装备入口。
  switch (item) {
    case I_leather_helmet: case I_copper_helmet: case I_chainmail_helmet: case I_iron_helmet:
    case I_golden_helmet: case I_diamond_helmet: case I_netherite_helmet: return 39;
    case I_leather_chestplate: case I_copper_chestplate: case I_chainmail_chestplate: case I_iron_chestplate:
    case I_golden_chestplate: case I_diamond_chestplate: case I_netherite_chestplate: return 38;
    // 鞘翅（I_elytra = 863）本版禁用：不进护甲槽，右键只会回一条“已禁用”提示。
    case I_leather_leggings: case I_copper_leggings: case I_chainmail_leggings: case I_iron_leggings:
    case I_golden_leggings: case I_diamond_leggings: case I_netherite_leggings: return 37;
    case I_leather_boots: case I_copper_boots: case I_chainmail_boots: case I_iron_boots:
    case I_golden_boots: case I_diamond_boots: case I_netherite_boots: return 36;
    default: return 255;
  }
}

void MinecraftServer::broadcastBlockChangesInArea_(int16_t x1, int16_t z1, int16_t x2, int16_t z2) {
  for (int i = 0; i < block_changes_count; i++) {
    if (block_changes[i].block == 0xFF) continue;
    if (block_changes[i].x >= x1 && block_changes[i].x <= x2 &&
        block_changes[i].z >= z1 && block_changes[i].z <= z2) {
      for (uint8_t j = 0; j < kMaxClients; j++) {
        if (!clients_[j].used || clients_[j].state != STATE_PLAY) continue;
        PacketCodec oc(clients_[j].fd);
        uint16_t st = netStateAt(block_changes[i].x, block_changes[i].y, block_changes[i].z, block_changes[i].block);
        if (block_changes[i].block == B_air) st = block_palette[B_air];
        sendBlockUpdateState_(oc, block_changes[i].x, block_changes[i].y, block_changes[i].z, st);
      }
    }
  }
}

// ============================================================
// Mob 逻辑
// ============================================================

void MinecraftServer::tickMobs_() {
  for (int i = 0; i < MAX_MOBS; i++) {
    if (mob_data[i].type == 0) continue;
    int entity_id = -2 - i;

    // 死亡回收
    if ((mob_data[i].data & 31) == 0) {
      if (mob_data[i].y < (unsigned int)TICKS_PER_SECOND) { mob_data[i].y++; continue; }
      mob_data[i].type = 0;
      for (uint8_t j = 0; j < kMaxClients; j++) {
        if (!clients_[j].used || clients_[j].state != STATE_PLAY) continue;
        PacketCodec oc(clients_[j].fd);
        sendEntityEvent_(oc, entity_id, 60);
        sendRemoveEntity_(oc, entity_id);
      }
      continue;
    }

    uint8_t passive = (mob_data[i].type == 26 || mob_data[i].type == 30 ||
                       mob_data[i].type == 100 || mob_data[i].type == 111);

    // 找最近玩家
    PlayerData* closest = nullptr;
    uint32_t closest_dist = 0xFFFFFFFF;
    for (int j = 0; j < MAX_PLAYERS; j++) {
      if (player_data[j].client_fd == -1) continue;
      uint32_t d = abs(mob_data[i].x - player_data[j].x) + abs(mob_data[i].z - player_data[j].z);
      if (d < closest_dist) { closest_dist = d; closest = &player_data[j]; }
    }

    if (closest_dist > MOB_DESPAWN_DISTANCE) { mob_data[i].type = 0; continue; }

    if (passive) {
      uint32_t r = fast_rand();
      if (r % (4 * (unsigned int)TICKS_PER_SECOND) != 0) continue;

      int16_t new_x = mob_data[i].x, new_z = mob_data[i].z;
      uint8_t yaw = 0;
      if ((r >> 2) & 1) { if ((r >> 1) & 1) { new_x += 1; yaw = 192; } else { new_x -= 1; yaw = 64; } }
      else { if ((r >> 1) & 1) { new_z += 1; yaw = 0; } else { new_z -= 1; yaw = 128; } }

      uint8_t b = getBlockAt(new_x, mob_data[i].y, new_z);
      if (isPassableBlock(b) && !isPassableBlock(getBlockAt(new_x, mob_data[i].y - 1, new_z))) {
        mob_data[i].x = new_x; mob_data[i].z = new_z;
        for (uint8_t j = 0; j < kMaxClients; j++) {
          if (!clients_[j].used || clients_[j].state != STATE_PLAY) continue;
          PacketCodec oc(clients_[j].fd);
          sendTeleportEntity_(oc, entity_id, new_x + 0.5, mob_data[i].y, new_z + 0.5, yaw * 360.0f / 256, 0);
        }
      }
    } else {
      if (!closest) continue;
      if (closest_dist < 3 && abs(mob_data[i].y - closest->y) < 2) {
        int ci = slotIndexForPlayer_(closest);
        if (ci >= 0) hurtEntity_(ci, -1, D_generic, 6);
        continue;
      }

      int16_t new_x = mob_data[i].x, new_z = mob_data[i].z;
      if (closest->x < mob_data[i].x) new_x--;
      else if (closest->x > mob_data[i].x) new_x++;
      if (closest->z < mob_data[i].z) new_z--;
      else if (closest->z > mob_data[i].z) new_z++;

      uint8_t b = getBlockAt(new_x, mob_data[i].y, new_z);
      uint8_t b_above = getBlockAt(new_x, mob_data[i].y + 1, new_z);
      uint8_t b_below = getBlockAt(new_x, mob_data[i].y - 1, new_z);
      if (isPassableBlock(b) && isPassableBlock(b_above) && !isPassableBlock(b_below)) {
        mob_data[i].x = new_x; mob_data[i].z = new_z;
        for (uint8_t j = 0; j < kMaxClients; j++) {
          if (!clients_[j].used || clients_[j].state != STATE_PLAY) continue;
          PacketCodec oc(clients_[j].fd);
          sendTeleportEntity_(oc, entity_id, new_x + 0.5, mob_data[i].y, new_z + 0.5, 0, 0);
        }
      }
    }
  }
}

void MinecraftServer::trySpawnMobNearPlayer_(PlayerData* player) {
  uint32_t r = fast_rand();
  if ((r & 3) != 0) return;

  int16_t cx = div_floor(player->x, 16);
  int16_t cz = div_floor(player->z, 16);
  int16_t mob_x = (cx + ((r & 1) ? ACTIVE_VIEW_DISTANCE : -ACTIVE_VIEW_DISTANCE)) * 16 + ((r >> 4) & 15);
  int16_t mob_z = (cz + ((r & 2) ? ACTIVE_VIEW_DISTANCE : -ACTIVE_VIEW_DISTANCE)) * 16 + ((r >> 8) & 15);

  uint8_t mob_y = player->y > 8 ? player->y - 8 : 0;
  for (int tries = 0; tries < 20; tries++) {
    uint8_t b_low = getBlockAt(mob_x, mob_y - 1, mob_z);
    uint8_t b_mid = getBlockAt(mob_x, mob_y, mob_z);
    uint8_t b_top = getBlockAt(mob_x, mob_y + 1, mob_z);
    if (!isPassableBlock(b_low) && isPassableSpawnBlock(b_mid) && isPassableSpawnBlock(b_top)) break;
    mob_y++;
    if (mob_y > 250) return;
  }

  if ((world_time < 13000 || world_time > 23460) && mob_y > 48) {
    // 白天地面: 只生成动物
    uint32_t choice = (r >> 12) & 3;
    uint8_t type = 26; uint8_t hp = 4;
    if (choice == 1) { type = 30; hp = 10; }
    else if (choice == 2) { type = 100; hp = 10; }
    else if (choice == 3) { type = 111; hp = 8; }
    spawnMob(type, mob_x, mob_y, mob_z, hp);
    broadcastMobSpawn_(type, mob_x, mob_y, mob_z);
  } else if (mob_y > 48) {
    // 晚上地面: 动物和怪物都能生成
    uint32_t choice = (r >> 12) & 7;
    if (choice <= 3) {
      // 50% 概率生成动物
      uint8_t type = 26; uint8_t hp = 4;
      if (choice == 1) { type = 30; hp = 10; }
      else if (choice == 2) { type = 100; hp = 10; }
      else if (choice == 3) { type = 111; hp = 8; }
      spawnMob(type, mob_x, mob_y, mob_z, hp);
      broadcastMobSpawn_(type, mob_x, mob_y, mob_z);
    } else {
      // 50% 概率生成敌对: 僵尸/骷髅/蜘蛛
      uint8_t hostile_choice = (r >> 16) & 3;
      uint8_t type = 150; uint8_t hp = 20;  // zombie
      if (hostile_choice == 1) { type = 115; hp = 20; }  // skeleton
      else if (hostile_choice == 2) { type = 124; hp = 16; }  // spider
      spawnMob(type, mob_x, mob_y, mob_z, hp);
      broadcastMobSpawn_(type, mob_x, mob_y, mob_z);
    }
  } else {
    // 地下: 僵尸/骷髅/蜘蛛
    uint8_t hostile_choice = (r >> 16) & 3;
    uint8_t type = 150; uint8_t hp = 20;
    if (hostile_choice == 1) { type = 115; hp = 20; }
    else if (hostile_choice == 2) { type = 124; hp = 16; }
    spawnMob(type, mob_x, mob_y, mob_z, hp);
    broadcastMobSpawn_(type, mob_x, mob_y, mob_z);
  }
}

void MinecraftServer::broadcastMobSpawn_(uint8_t type, int16_t x, uint8_t y, int16_t z) {
  int mob_idx = -1;
  for (int i = 0; i < MAX_MOBS; i++) {
    if (mob_data[i].type == type && mob_data[i].x == x && mob_data[i].y == y && mob_data[i].z == z) {
      mob_idx = i; break;
    }
  }
  if (mob_idx < 0) return;

  uint8_t uuid[16];
  uint32_t r = fast_rand();
  memcpy(uuid, &r, 4);
  memcpy(uuid + 4, &mob_idx, 4);
  memset(uuid + 8, 0, 8);

  for (uint8_t j = 0; j < kMaxClients; j++) {
    if (!clients_[j].used || clients_[j].state != STATE_PLAY) continue;
    PacketCodec oc(clients_[j].fd);
    sendSpawnEntity_(oc, -2 - mob_idx, uuid, type, x + 0.5, y, z + 0.5, 0, 0);
  }
}

// ============================================================
// 发包实现: Configuration
// ============================================================

bool MinecraftServer::sendBrand_(PacketCodec& codec) {
  const char* brand = "ESP32MC";
  return sendPluginMessage_(codec, "minecraft:brand", (const uint8_t*)brand, (uint32_t)strlen(brand));
}

bool MinecraftServer::sendPluginMessage_(PacketCodec& codec, const char* channel, const uint8_t* data, uint32_t data_len) {
  uint32_t channel_len = (uint32_t)strlen(channel);
  uint32_t pkt_len = 1 + codec.sizeVarInt(channel_len) + channel_len + codec.sizeVarInt(data_len) + data_len;
  pktLog_(0x01, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x01) &&
         codec.writeString(channel) && codec.writeVarInt(data_len) && codec.writeExact(data, data_len);
  pktCheck_(codec, start, pkt_len, "id0x01");
  return ok;
}

bool MinecraftServer::sendKnownPacks_(PacketCodec& codec) {
  const char* ns = "minecraft"; const char* pack = "core"; const char* ver = VERSION_NAME;
  uint32_t ns_len = (uint32_t)strlen(ns), pack_len = (uint32_t)strlen(pack), ver_len = (uint32_t)strlen(ver);
  uint32_t pkt_len = 1 + codec.sizeVarInt(1) + codec.sizeVarInt(ns_len) + ns_len +
                     codec.sizeVarInt(pack_len) + pack_len + codec.sizeVarInt(ver_len) + ver_len;
  return codec.writeVarInt(pkt_len) && codec.writeVarInt(0x0E) && codec.writeVarInt(1) &&
         codec.writeString(ns) && codec.writeString(pack) && codec.writeString(ver);
}

bool MinecraftServer::sendEnabledFeatures_(PacketCodec& codec) {
  const char* feature = "minecraft:vanilla";
  uint32_t flen = (uint32_t)strlen(feature);
  uint32_t pkt_len = 1 + codec.sizeVarInt(1) + codec.sizeVarInt(flen) + flen;
  return codec.writeVarInt(pkt_len) && codec.writeVarInt(0x0C) && codec.writeVarInt(1) &&
         codec.writeVarInt(flen) && codec.writeExact((const uint8_t*)feature, flen);
}

bool MinecraftServer::sendRegistries_(PacketCodec& codec) {
  return codec.writeExact(registries_bin, sizeof(registries_bin)) &&
         codec.writeExact(tags_bin, sizeof(tags_bin));
}

bool MinecraftServer::sendFinishConfiguration_(PacketCodec& codec) {
  return codec.writeVarInt(1) && codec.writeVarInt(0x03);
}

bool MinecraftServer::consumeClientInformation_(PacketCodec& codec) {
  char locale[32]; uint8_t tmp8; int32_t tmp32;
  if (!codec.readString(locale, sizeof(locale))) return false;
  if (!codec.readByte(tmp8)) return false;
  if (!codec.readVarInt(tmp32)) return false;
  if (!codec.readByte(tmp8)) return false;
  if (!codec.readByte(tmp8)) return false;
  if (!codec.readVarInt(tmp32)) return false;
  if (!codec.readByte(tmp8)) return false;
  if (!codec.readByte(tmp8)) return false;
  if (!codec.readVarInt(tmp32)) return false;
  return true;
}

bool MinecraftServer::consumePluginMessage_(PacketCodec& codec, int32_t payload_len) {
  char channel[64];
  if (!codec.readString(channel, sizeof(channel))) return false;
  int32_t consumed = codec.sizeVarInt((uint32_t)strlen(channel)) + (int32_t)strlen(channel);
  if (payload_len > consumed) return codec.skipBytes((size_t)(payload_len - consumed));
  return true;
}

bool MinecraftServer::consumeKnownPacks_(PacketCodec& codec) {
  int32_t count = 0;
  if (!codec.readVarInt(count)) return false;
  char buf[64];
  for (int32_t i = 0; i < count; i++) {
    if (!codec.readString(buf, sizeof(buf))) return false;
    if (!codec.readString(buf, sizeof(buf))) return false;
    if (!codec.readString(buf, sizeof(buf))) return false;
  }
  return true;
}

bool MinecraftServer::skipRemainingPacket_(PacketCodec& codec, int32_t packet_len, int32_t packet_id) {
  int32_t remaining = packet_len - codec.sizeVarInt((uint32_t)packet_id);
  if (remaining <= 0) return true;
  return codec.skipBytes((size_t)remaining);
}

// ============================================================
// 发包实现: Play
// ============================================================

bool MinecraftServer::sendLoginPlay_(PacketCodec& codec, uint32_t entity_id) {
  const char* dim = "overworld";
  // 用该槽玩家自己保存的模式（/gamemode 改过就按改过的进世界）
  uint8_t login_mode = GAMEMODE;
  if (entity_id < kMaxClients && clients_[entity_id].player_index >= 0)
    login_mode = player_data[clients_[entity_id].player_index].game_mode;
  uint32_t pkt_len = 1 + 4 + 1 + 1 + 1 + 9
                   + codec.sizeVarInt(MAX_PLAYERS) + codec.sizeVarInt(ACTIVE_VIEW_DISTANCE) + codec.sizeVarInt(ACTIVE_VIEW_DISTANCE)
                   + 1 + 1 + 1
                   + 1 + 1 + 9 + 8
                   + 1 + 1 + 1 + 1 + 1
                   + 1 + codec.sizeVarInt(63) + 1;
  codec.resetWriteCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeVarInt(0x31) &&
         codec.writeUint32(entity_id) && codec.writeByte(0) &&
         codec.writeVarInt(1) && codec.writeVarInt(9) && codec.writeExact((const uint8_t*)dim, 9) &&
         codec.writeVarInt(MAX_PLAYERS) && codec.writeVarInt(ACTIVE_VIEW_DISTANCE) &&
         codec.writeVarInt(ACTIVE_VIEW_DISTANCE) && codec.writeByte(0) && codec.writeByte(1) &&
         codec.writeByte(0) && codec.writeVarInt(0) && codec.writeVarInt(9) &&
         codec.writeExact((const uint8_t*)dim, 9) && codec.writeUint64(0x0123456789ABCDEFULL) &&
         codec.writeByte(login_mode) && codec.writeByte(0xFF) && codec.writeByte(0) &&
         codec.writeByte(0) && codec.writeByte(0) && codec.writeVarInt(0) &&
         codec.writeVarInt(63) && codec.writeByte(0);
  // 验证: 实际写入 = pkt_len 的 VarInt 大小 + pkt_len
  size_t expected = (size_t)codec.sizeVarInt(pkt_len) + pkt_len;
  if (codec.writeCount() != expected)
    Serial.printf("[ERR] sendLoginPlay_ len mismatch: declared=%u actual=%u\n", (unsigned)expected, (unsigned)codec.writeCount());
  return ok;
}

// 被方块埋住/卡住时的纠正。搭高时客户端会预测方块、本地碰撞把人往上推，
// 服务端如果每次都跟着抬高并回发位置同步，人会越飘越高（现象就是“放太快就飞出去”）。
// 现在：① 放置成功不再调用它（放置路径自己回发方块、撤掉预测）；② 这里只在“连续卡住”
// 一段时间后才动，而且一次只抬 1 格，丢掉了“一跳到方柱顶面”的跳变。
void MinecraftServer::fixPlayerEmbedded_(uint8_t slot_idx, PlayerData* player) {
  if (slot_idx >= kMaxClients || player == nullptr) return;
  int16_t px = (int16_t)player->x, pz = (int16_t)player->z;
  uint8_t py = player->y;
  if (py == 0) return;
  if (isPassableBlock(getBlockAt(px, py, pz))) { s_stuck_since_ms[slot_idx] = 0; return; }   // 正常站着，不动
  uint32_t now_ms = millis();
  if (s_stuck_since_ms[slot_idx] == 0) { s_stuck_since_ms[slot_idx] = now_ms; return; }
  if ((uint32_t)(now_ms - s_stuck_since_ms[slot_idx]) < 600) return;   // 刚被预测方块顶一下，先不管
  uint8_t ny = (uint8_t)(py + 1);
  if (ny >= 250) return;
  if (!isPassableBlock(getBlockAt(px, ny, pz))) return;                // 头上一格还是实心，等下一轮
  s_stuck_since_ms[slot_idx] = 0;
  player->y = ny;
  PacketCodec pc(clients_[slot_idx].fd);
  sendSynchronizePlayerPosition_(pc, (double)px + 0.5, (double)ny, (double)pz + 0.5,
                                 (float)player->yaw * 180.0f / 127.0f,
                                 (float)player->pitch * 90.0f / 127.0f);
  Serial.printf("[STUCK] slot=%u y %u -> %u\r\n", (unsigned)slot_idx, (unsigned)py, (unsigned)ny);
}

// set_entity_motion 的 Vec3 用的是 26.1.2 的 Vec3.LP_STREAM_CODEC，
// 实现取自客户端 jar 里 net.minecraft.network.LpVec3 的字节码（不是猜）：
//   b0 的低 2 位 = scale 的低 2 位；bit2 = 后面还跟一个 VarInt(scale>>2)；
//   三个分量各 15 bit：pack(v/scale) = round((v/scale*0.5+0.5)*32766)，依次放在 bit3 / bit18 / bit33；
//   scale = ceil(max|v|)；整条 48 bit 数据按 b0、b1、u32 三段写出。
bool MinecraftServer::sendSetEntityMotion_(PacketCodec& codec, int entity_id,
                                           double vx, double vy, double vz) {
  double ax = fabs(vx), ay = fabs(vy), az = fabs(vz);
  double amax = ax;
  if (ay > amax) amax = ay;
  if (az > amax) amax = az;

  bool tiny = (amax < 3.051944088384301E-5);
  uint64_t data = 0;
  uint64_t scale = 0;
  bool extra = false;
  if (!tiny) {
    double scale_d = ceil(amax);
    if (scale_d < 1.0) scale_d = 1.0;
    scale = (uint64_t)scale_d;
    extra = ((scale & 3) != scale);
    uint64_t field = extra ? ((scale & 3) | 4) : scale;
    double inv = 1.0 / scale_d;
    long xp = lround((vx * inv * 0.5 + 0.5) * 32766.0);
    long yp = lround((vy * inv * 0.5 + 0.5) * 32766.0);
    long zp = lround((vz * inv * 0.5 + 0.5) * 32766.0);
    if (xp < 0) xp = 0; if (xp > 32766) xp = 32766;
    if (yp < 0) yp = 0; if (yp > 32766) yp = 32766;
    if (zp < 0) zp = 0; if (zp > 32766) zp = 32766;
    data = (field & 0x7) | ((uint64_t)xp << 3) | ((uint64_t)yp << 18) | ((uint64_t)zp << 33);
  }

  uint32_t lp_bytes = tiny ? 1 : (6 + (extra ? codec.sizeVarInt((uint32_t)(scale >> 2)) : 0));
  uint32_t pkt_len = 1 + codec.sizeVarInt((uint32_t)entity_id) + lp_bytes;
  pktLog_(0x65, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x65)
            && codec.writeVarInt((uint32_t)entity_id);
  if (!ok) return false;
  if (tiny) {
    ok = codec.writeByte(0);
  } else {
    ok = codec.writeByte((uint8_t)(data & 0xFF))
         && codec.writeByte((uint8_t)((data >> 8) & 0xFF))
         && codec.writeUint32((uint32_t)((data >> 16) & 0xFFFFFFFF));
    if (ok && extra) ok = codec.writeVarInt((uint32_t)(scale >> 2));
  }
  pktCheck_(codec, start, pkt_len, "id0x65");
  return ok;
}

bool MinecraftServer::sendSynchronizePlayerPosition_(PacketCodec& codec, double x, double y, double z, float yaw, float pitch) {
  uint32_t pkt_len = 61 + codec.sizeVarInt((uint32_t)-1);
  pktLog_(0x48, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x48) && codec.writeVarInt((uint32_t)-1) &&
         codec.writeDouble(x) && codec.writeDouble(y) && codec.writeDouble(z) &&
         codec.writeDouble(0) && codec.writeDouble(0) && codec.writeDouble(0) &&
         codec.writeFloat(yaw) && codec.writeFloat(pitch) && codec.writeUint32(0);
  pktCheck_(codec, start, pkt_len, "id0x48");
  return ok;
}

bool MinecraftServer::sendSetDefaultSpawnPosition_(PacketCodec& codec, int64_t x, int64_t y, int64_t z, float yaw, float pitch) {
  const char* dim = "minecraft:overworld";
  uint32_t dim_len = (uint32_t)strlen(dim);
  uint64_t packed = (((uint64_t)x & 0x3FFFFFFULL) << 38) | (((uint64_t)z & 0x3FFFFFFULL) << 12) | ((uint64_t)y & 0xFFFULL);
  uint32_t payload = codec.sizeVarInt(dim_len) + dim_len + 8 + 4 + 4;
  uint32_t pkt_len = codec.sizeVarInt(0x61) + payload;
  return codec.writeVarInt(pkt_len) && codec.writeVarInt(0x61) &&
         codec.writeVarInt(dim_len) && codec.writeExact((const uint8_t*)dim, dim_len) &&
         codec.writeUint64(packed) && codec.writeFloat(yaw) && codec.writeFloat(pitch);
}


bool MinecraftServer::sendGameEvent_(PacketCodec& codec, uint8_t reason, float value) {
  return codec.writeVarInt(6) && codec.writeByte(0x26) && codec.writeByte(reason) &&
         codec.writeFloat(value);
}

bool MinecraftServer::sendCommandTree_(PacketCodec& codec, uint8_t packet_id) {
  // 混合树：字面量节点 + 参数节点（brigadier:integer=3 / brigadier:string=5）。
  // 不声明参数节点的话，客户端会在 "/tp 5 66 5" 这种带参数的命令上直接报
  // “错误的命令参数”，命令根本发不到服务端。
  // 这里把 runCommand_ 支持的**所有**斜杠指令都坐进树里，避免“有的指令敲不出来”：
  //   /gamemode <creative|survival|0|1> [player]   /tp <x> <y> <z> | /tp <player>
  //   /help   /kill [player]   /time <set|add|day|night|noon|midnight|ticks>
  //   /weather <clear|rain|thunder>   /enchant [level]
  //   /drop [all]   /inv   /drops   /give <item> [count]   /recipes
  struct TreeN {
    const char* name;   // nullptr = root
    uint8_t     type;   // 0 root / 1 literal / 2 argument
    bool        exec;
    uint8_t     nchild;
    uint8_t     child[16];
    uint8_t     parser; // 参数节点专用：3 = brigadier:integer, 5 = brigadier:string
    uint8_t     str_mode; // 字符串参数模式：0 = 单个词
  };
  static const TreeN kNodes[] = {
    {nullptr     , 0, false, 11, {1, 2, 3, 4, 5, 6, 8, 9, 10, 33, 36}, 0, 0}, // 0
    {"gamemode"   , 1, true ,  3, {11, 12, 19}, 0, 0}, // 1
    {"tp"         , 1, true ,  1, {13}, 0, 0}, // 2
    {"help"       , 1, true ,  0, {}, 0, 0}, // 3
    {"kill"       , 1, true ,  1, {14}, 0, 0}, // 4
    {"time"       , 1, true ,  7, {15, 16, 17, 18, 20, 21, 22}, 0, 0}, // 5
    {"weather"    , 1, true ,  3, {23, 24, 25}, 0, 0}, // 6
    {"noop"       , 1, true ,  1, {26}, 0, 0}, // 7  (原 /enchant，已删除)
    {"drop"       , 1, true ,  1, {27}, 0, 0}, // 8
    {"inv"        , 1, true ,  0, {}, 0, 0}, // 9
    {"drops"      , 1, true ,  0, {}, 0, 0}, // 10
    {"creative"   , 1, true ,  1, {28}, 0, 0}, // 11
    {"survival"   , 1, true ,  1, {28}, 0, 0}, // 12
    {"x"          , 2, true ,  1, {29}, 3, 0}, // 13
    {"player"     , 2, true ,  0, {}, 5, 0}, // 14
    {"set"        , 1, true ,  5, {17, 18, 20, 21, 22}, 0, 0}, // 15
    {"add"        , 1, true ,  1, {22}, 0, 0}, // 16
    {"day"        , 1, true ,  0, {}, 0, 0}, // 17
    {"night"      , 1, true ,  0, {}, 0, 0}, // 18
    {"mode"       , 2, true ,  0, {}, 3, 0}, // 19
    {"noon"       , 1, true ,  0, {}, 0, 0}, // 20
    {"midnight"   , 1, true ,  0, {}, 0, 0}, // 21
    {"ticks"      , 2, true ,  0, {}, 3, 0}, // 22
    {"clear"      , 1, true ,  0, {}, 0, 0}, // 23
    {"rain"       , 1, true ,  0, {}, 0, 0}, // 24
    {"thunder"    , 1, true ,  0, {}, 0, 0}, // 25
    {"enchantname" , 2, true ,  1, {30}, 5, 0}, // 26
    {"all"        , 1, true ,  0, {}, 0, 0}, // 27
    {"player"     , 2, true ,  0, {}, 5, 0}, // 28
    {"y"          , 2, true ,  1, {31}, 3, 0}, // 29
    {"seconds"    , 2, true ,  1, {32}, 3, 0}, // 30
    {"z"          , 2, true ,  0, {}, 3, 0}, // 31
    {"amplifier"  , 2, true ,  0, {}, 3, 0}, // 32
    {"give"       , 1, true ,  1, {34}, 0, 0}, // 33
    {"itemname"   , 2, true ,  1, {35}, 5, 0}, // 34
    {"count"      , 2, true ,  0, {}, 3, 0}, // 35
    {"recipes"    , 1, true ,  0, {}, 0, 0}, // 36
  };
  const uint8_t kNodeCount = (uint8_t)(sizeof(kNodes) / sizeof(kNodes[0]));

  uint32_t nodes_len = 0;
  for (uint8_t i = 0; i < kNodeCount; i++) {
    nodes_len += 1 + codec.sizeVarInt(kNodes[i].nchild) + kNodes[i].nchild;
    if (kNodes[i].name) {
      uint32_t l = (uint32_t)strlen(kNodes[i].name);
      nodes_len += codec.sizeVarInt(l) + l;
    }
    if (kNodes[i].type == 2) {
      nodes_len += codec.sizeVarInt(kNodes[i].parser);
      if (kNodes[i].parser == 3) nodes_len += 1;          // 无上下界的标志字节
      else nodes_len += codec.sizeVarInt(kNodes[i].str_mode);
    }
  }
  uint32_t pkt_len = 1 + codec.sizeVarInt(kNodeCount) + nodes_len + 1;

  codec.resetWriteCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeVarInt(packet_id) &&
            codec.writeVarInt(kNodeCount);
  for (uint8_t i = 0; i < kNodeCount && ok; i++) {
    uint8_t flags = kNodes[i].type | (kNodes[i].exec ? 0x04 : 0x00);
    ok = codec.writeByte(flags) && codec.writeVarInt(kNodes[i].nchild);
    for (uint8_t c = 0; c < kNodes[i].nchild && ok; c++)
      ok = codec.writeVarInt(kNodes[i].child[c]);
    if (ok && kNodes[i].name) ok = codec.writeString(kNodes[i].name);
    if (ok && kNodes[i].type == 2) {
      ok = codec.writeVarInt(kNodes[i].parser);
      if (ok) {
        if (kNodes[i].parser == 3) ok = codec.writeByte(0x00);   // 不设 min/max
        else ok = codec.writeVarInt(kNodes[i].str_mode);
      }
    }
  }
  ok = ok && codec.writeVarInt(0);   // rootIndex

  size_t expected = (size_t)codec.sizeVarInt(pkt_len) + pkt_len;
  if (codec.writeCount() != expected)
    Serial.printf("[CMD] tree len mismatch: declared=%u actual=%u\r\n",
                  (unsigned)expected, (unsigned)codec.writeCount());
  return ok;
}

bool MinecraftServer::sendStartWaitingForChunks_(PacketCodec& codec) {
  return sendGameEvent_(codec, 13, 0.0f);
}

bool MinecraftServer::sendSetCenterChunk_(PacketCodec& codec, int x, int z) {
  uint32_t pkt_len = 1 + codec.sizeVarInt((uint32_t)x) + codec.sizeVarInt((uint32_t)z);
  pktLog_(0x5E, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x5E) && codec.writeVarInt((uint32_t)x) && codec.writeVarInt((uint32_t)z);
  pktCheck_(codec, start, pkt_len, "id0x5E");
  return ok;
}

bool MinecraftServer::sendKeepAlive_(PacketCodec& codec) {
  return codec.writeVarInt(9) && codec.writeByte(0x2C) && codec.writeUint64(0);
}

bool MinecraftServer::sendUpdateTime_(PacketCodec& codec, uint64_t ticks) {
  // 26.1.2: world_age(8) + clock_count(varint) + [clock_id(varint) + ticks(varlong) + min(float) + max(float)] * count
  int varlong_size = 1;
  { uint64_t v = ticks; while (v & ~0x7FULL) { varlong_size++; v >>= 7; } }

  int clock_state_size = varlong_size + 4 + 4;
  int payload_length = 8 + 1 + 1 + clock_state_size + 1 + clock_state_size;
  int pkt_len = codec.sizeVarInt(0x71) + payload_length;

  if (!codec.writeVarInt((uint32_t)pkt_len)) return false;
  if (!codec.writeVarInt(0x71)) return false;
  if (!codec.writeUint64(ticks)) return false;
  if (!codec.writeVarInt(2)) return false;

  // Overworld clock (id=0)
  if (!codec.writeVarInt(0)) return false;
  { uint64_t v = ticks;
    while (v & ~0x7FULL) { uint8_t b = (v & 0x7F) | 0x80; codec.writeByte(b); v >>= 7; }
    codec.writeByte((uint8_t)v);
  }
  if (!codec.writeFloat(0.0f)) return false;
  if (!codec.writeFloat(1.0f)) return false;

  // The End clock (id=1)
  if (!codec.writeVarInt(1)) return false;
  { uint64_t v = ticks;
    while (v & ~0x7FULL) { uint8_t b = (v & 0x7F) | 0x80; codec.writeByte(b); v >>= 7; }
    codec.writeByte((uint8_t)v);
  }
  if (!codec.writeFloat(0.0f)) return false;
  if (!codec.writeFloat(1.0f)) return false;

  return true;
}

bool MinecraftServer::sendSetHealth_(PacketCodec& codec, uint8_t health, uint8_t food, uint16_t saturation) {
  uint32_t pkt_len = 9 + codec.sizeVarInt(food);
  pktLog_(0x68, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x68) &&
         codec.writeFloat((float)health) && codec.writeVarInt(food) &&
         codec.writeFloat((float)(saturation - 200) / 500.0f);
  pktCheck_(codec, start, pkt_len, "id0x68");
  return ok;
}

bool MinecraftServer::sendSetHeldItem_(PacketCodec& codec, uint8_t slot) {
  uint32_t pkt_len = codec.sizeVarInt(0x69) + 1;
  return codec.writeVarInt(pkt_len) && codec.writeVarInt(0x69) && codec.writeByte(slot);
}

bool MinecraftServer::sendSetContainerSlot_(PacketCodec& codec, int window_id, uint16_t slot, uint8_t count, uint16_t item) {
  return sendSetContainerSlotEnch_(codec, window_id, slot, count, item, 0);
}

// 手持格统一入口：带上名字与附魔。以前手持格一律用 sendSetContainerSlot_，
// 改过名的工具一用（放置/吃/骨粉/伐木…）就被服务端这份“没名字”的回包洗回去了。
void MinecraftServer::sendHeldSlot_(PacketCodec& codec, PlayerData* player, uint8_t count, uint16_t item) {
  if (player == nullptr) return;
  uint8_t hb = player->hotbar;
  if (hb >= 9) hb = 0;
  int pi = slotIndexForPlayer_(player);
  sendSetContainerSlotEnch_(codec, 0, serverSlotToClientSlot(0, hb), count, item,
                            (pi >= 0) ? enchantGet_((uint8_t)pi, hb) : 0,
                            (pi >= 0 && g_item_name[pi][hb][0]) ? g_item_name[pi][hb] : nullptr,
                            (pi >= 0) ? enchantIdGet_((uint8_t)pi, hb) : (uint8_t)kEnchSharpnessId);
}

// 物品栈网络格式（1.20.5+）：
//   item id VarInt | count VarInt | added components patch | removed components patch
// 附魔走真组件：data_component_type 里 minecraft:enchantments = 13（客户端 26.1.2 的
// registries.json 导出值）。它的值是 ItemEnchantments，网络编码就是一个 map：
//   VarInt 条数 | 每条 VarInt 附魔注册表 id + VarInt 等级
// 客户端 jar 里 ItemEnchantments 只有 enchantments 一个字段（没有 show_in_tooltip 位），
// 所以这里只写这一层；带附魔的物品客户端会自动发光并显示附魔名。

bool MinecraftServer::sendSetContainerSlotEnch_(PacketCodec& codec, int window_id, uint16_t slot,
                                                uint8_t count, uint16_t item, uint8_t ench_level,
                                                const char* name, uint8_t ench_id) {
  if (slot == 255) return true;
  if (count > 64) count = 64;
  { uint8_t mx = getItemStackSize(item); if (mx > 0 && count > mx) count = mx; }
  // 附魔书（1245）的附魔存在 stored_enchantments；工具/护甲用 enchantments。
  // 发错组件客户端就不会把这本书认成“有内容的附魔书”，铁砧自然也不认它。
  int32_t ench_comp = (item == I_enchanted_book) ? kCompStoredEnchantments : kCompEnchantments;
  uint16_t name_len = 0;
  if (name != nullptr && count > 0) {
    while (name[name_len] != 0 && name_len < 32) name_len++;
  }
  uint32_t added = 0;
  if (name_len > 0) added += 1;          // custom_name(6)
  if (ench_level > 0) added += 1;        // enchantments(13) / stored_enchantments(42)
  uint32_t pkt_len = 1 + codec.sizeVarInt((uint32_t)window_id) + 1 + 2 + codec.sizeVarInt(count);
  if (count > 0) {
    pkt_len += codec.sizeVarInt(item) + codec.sizeVarInt(added);
    if (name_len > 0) pkt_len += codec.sizeVarInt(6) + 1 + 2 + name_len;
    if (ench_level > 0) {
      pkt_len += codec.sizeVarInt((uint32_t)ench_comp) + codec.sizeVarInt(1)
               + codec.sizeVarInt(ench_id) + codec.sizeVarInt(ench_level);
    }
    pkt_len += codec.sizeVarInt(0);      // 空删除表
  }
  if (!codec.writeVarInt(pkt_len) || !codec.writeByte(0x14)) return false;
  if (!codec.writeVarInt((uint32_t)window_id) || !codec.writeVarInt(0) || !codec.writeUint16(slot)) return false;
  if (!codec.writeVarInt(count)) return false;
  if (count > 0) {
    if (!codec.writeVarInt(item) || !codec.writeVarInt(added)) return false;
    // 这里的顺序是协议定的：先 新增数量、再 删除数量，之后才是逐条组件。
    // 之前把「删除数量 0」写在条目后面，客户端会把组件 id 当成删除数量接着读，直接 VarInt too big 掉线。
    if (!codec.writeVarInt(0)) return false;
    if (name_len > 0) {
      // custom_name = 文本组件；与开窗标题同一种编码：类型 8（TAG_String，无名）+ 2 字节长度 + UTF-8
      if (!codec.writeVarInt(6) || !codec.writeByte(8) || !codec.writeUint16(name_len) ||
          !codec.writeExact((const uint8_t*)name, name_len)) return false;
    }
    if (ench_level > 0) {
      if (!codec.writeVarInt((uint32_t)ench_comp) || !codec.writeVarInt(1) ||
          !codec.writeVarInt(ench_id) || !codec.writeVarInt(ench_level)) return false;
    }
  }
  return true;
}
bool MinecraftServer::sendBlockUpdate_(PacketCodec& codec, int64_t x, int64_t y, int64_t z, uint8_t block) {
  return sendBlockUpdateState_(codec, x, y, z, block_palette[block]);
}

// 直接发“方块状态 id”：楼梯/梯子这类同一方块有多个朝向/半格时，只有走这条才能画出正确的形状。
bool MinecraftServer::sendBlockUpdateState_(PacketCodec& codec, int64_t x, int64_t y, int64_t z, uint16_t state) {
  uint32_t pkt_len = 9 + codec.sizeVarInt(state);
  pktLog_(0x08, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x08) &&
         codec.writeUint64(((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)) &&
         codec.writeVarInt(state);
  pktCheck_(codec, start, pkt_len, "id0x08");
  return ok;
}

bool MinecraftServer::sendAcknowledgeBlockChange_(PacketCodec& codec, int sequence) {
  uint32_t pkt_len = 1 + codec.sizeVarInt((uint32_t)sequence);
  pktLog_(0x04, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x04) && codec.writeVarInt((uint32_t)sequence);
  pktCheck_(codec, start, pkt_len, "id0x04");
  return ok;
}

bool MinecraftServer::sendPlayerInfoUpdateAddPlayer_(PacketCodec& codec, PlayerData& player) {
  uint32_t name_len = (uint32_t)strlen(player.name);
  // action bitmask: 0x01 AddPlayer | 0x08 UpdateListed | 0x10 UpdateLatency
  uint8_t actions = 0x01 | 0x08 | 0x10;
  // 每个玩家的数据: uuid(16) + name(varint+str) + properties(varint=0) + listed(bool=true) + latency(varint=0)
  uint32_t per_player = 16 + codec.sizeVarInt(name_len) + name_len + codec.sizeVarInt(0) + 1 + codec.sizeVarInt(0);
  uint32_t pkt_len = 1 + 1 + codec.sizeVarInt(1) + per_player;
  if (!codec.writeVarInt(pkt_len)) return false;
  if (!codec.writeByte(0x46)) return false;   // packet id
  if (!codec.writeByte(actions)) return false; // action bitmask
  if (!codec.writeVarInt(1)) return false;     // player count
  // uuid
  if (!codec.writeExact(player.uuid, 16)) return false;
  // 0x01 AddPlayer: name + properties
  if (!codec.writeVarInt(name_len)) return false;
  if (!codec.writeExact((const uint8_t*)player.name, name_len)) return false;
  if (!codec.writeVarInt(0)) return false;     // 0 properties
  // 0x08 UpdateListed: listed=true
  if (!codec.writeByte(1)) return false;
  // 0x10 UpdateLatency: latency=0
  if (!codec.writeVarInt(0)) return false;
  return true;
}

bool MinecraftServer::sendSpawnEntity_(PacketCodec& codec, int id, uint8_t* uuid, int type, double x, double y, double z, uint8_t yaw, uint8_t pitch) {
  uint32_t pkt_len = 45 + codec.sizeVarInt((uint32_t)id) + codec.sizeVarInt((uint32_t)type) + codec.sizeVarInt(0);
  pktLog_(0x01, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x01) &&
         codec.writeVarInt((uint32_t)id) && codec.writeExact(uuid, 16) && codec.writeVarInt((uint32_t)type) &&
         codec.writeDouble(x) && codec.writeDouble(y) && codec.writeDouble(z) &&
         codec.writeByte(0) && codec.writeByte(pitch) && codec.writeByte(yaw) && codec.writeByte(yaw) &&
         codec.writeVarInt(0);
  pktCheck_(codec, start, pkt_len, "id0x01");
  return ok;
}

bool MinecraftServer::sendEntityAnimation_(PacketCodec& codec, int id, uint8_t animation) {
  uint32_t pkt_len = 2 + codec.sizeVarInt((uint32_t)id);
  pktLog_(0x02, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x02) && codec.writeVarInt((uint32_t)id) && codec.writeByte(animation);
  pktCheck_(codec, start, pkt_len, "id0x02");
  return ok;
}

bool MinecraftServer::sendTeleportEntity_(PacketCodec& codec, int id, double x, double y, double z, float yaw, float pitch) {
  uint32_t pkt_len = 62 + codec.sizeVarInt((uint32_t)id);
  pktLog_(0x7D, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x7D) && codec.writeVarInt((uint32_t)id) &&
         codec.writeDouble(x) && codec.writeDouble(y) && codec.writeDouble(z) &&
         codec.writeUint64(0) && codec.writeUint64(0) && codec.writeUint64(0) &&
         codec.writeFloat(yaw) && codec.writeFloat(pitch) && codec.writeByte(1) && codec.writeUint32(0);
  pktCheck_(codec, start, pkt_len, "id0x7D");
  return ok;
}

bool MinecraftServer::sendSetHeadRotation_(PacketCodec& codec, int id, uint8_t yaw) {
  uint32_t pkt_len = 2 + codec.sizeVarInt((uint32_t)id);
  pktLog_(0x53, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x53) && codec.writeVarInt((uint32_t)id) && codec.writeByte(yaw);
  pktCheck_(codec, start, pkt_len, "id0x53");
  return ok;
}

bool MinecraftServer::sendUpdateEntityRotation_(PacketCodec& codec, int id, uint8_t yaw, uint8_t pitch) {
  uint32_t pkt_len = 4 + codec.sizeVarInt((uint32_t)id);
  pktLog_(0x38, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x38) && codec.writeVarInt((uint32_t)id) &&
         codec.writeByte(yaw) && codec.writeByte(pitch) && codec.writeByte(1);
  pktCheck_(codec, start, pkt_len, "id0x38");
  return ok;
}

bool MinecraftServer::sendDamageEvent_(PacketCodec& codec, int entity_id, int type) {
  uint32_t pkt_len = 4 + codec.sizeVarInt((uint32_t)entity_id) + codec.sizeVarInt((uint32_t)type);
  pktLog_(0x19, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x19) && codec.writeVarInt((uint32_t)entity_id) &&
         codec.writeVarInt((uint32_t)type) && codec.writeByte(0) && codec.writeByte(0) && codec.writeByte(0);
  pktCheck_(codec, start, pkt_len, "id0x19");
  return ok;
}

bool MinecraftServer::sendRemoveEntity_(PacketCodec& codec, int entity_id) {
  uint32_t pkt_len = 2 + codec.sizeVarInt((uint32_t)entity_id);
  pktLog_(0x4D, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x4D) && codec.writeByte(1) && codec.writeVarInt((uint32_t)entity_id);
  pktCheck_(codec, start, pkt_len, "id0x4D");
  return ok;
}

bool MinecraftServer::sendSystemChat_(PacketCodec& codec, const char* message, uint16_t len) {
  uint32_t pkt_len = 5 + len;
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x79) &&
            codec.writeByte(8) && codec.writeUint16(len) && codec.writeExact((const uint8_t*)message, len) &&
            codec.writeByte(0);
  pktCheck_(codec, start, pkt_len, "syschat");
  pktLog_(0x79, pkt_len);
  return ok;
}

bool MinecraftServer::sendEntityEvent_(PacketCodec& codec, int entity_id, uint8_t status) {
  return codec.writeVarInt(6) && codec.writeByte(0x22) && codec.writeUint32((uint32_t)entity_id) && codec.writeByte(status);
}

bool MinecraftServer::sendOpenScreen_(PacketCodec& codec, uint8_t window, const char* title, uint16_t length) {
  uint32_t pkt_len = 1 + 2 * codec.sizeVarInt(window) + 1 + 2 + length;
  pktLog_(0x3B, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x3B) &&
         codec.writeVarInt(window) && codec.writeVarInt(window) &&
         codec.writeByte(8) && codec.writeUint16(length) && codec.writeExact((const uint8_t*)title, length);
  pktCheck_(codec, start, pkt_len, "id0x3B");
  return ok;
}

// 26.1.2 的 update_recipes(0x85) 只带「配方属性表」：smithing_template / base / addition
// 客户端拿这三张表判断锻造台三个输入格能不能放东西。不发 = 表为空 = 什么都放不进去。
// 这里把 1..1200 号物品全部塞进三张表，等于「任意物品都能摆进任意格」，产物由我自己算。
// 手写 VarInt 的内存写入（配方书那块要先拼成一块，再整包发）
static void rbVar(uint8_t*& p, uint32_t v) {
  while (v & 0xFFFFFF80u) { *p++ = (uint8_t)((v & 0x7Fu) | 0x80u); v >>= 7; }
  *p++ = (uint8_t)v;
}

bool MinecraftServer::sendRecipeBookAdd_(PacketCodec& codec) {
  // 26.1.2 的“配方书”= clientbound recipe_book_add(74)：每条形如
  //   RecipeDisplayId(VarInt) + RecipeDisplay + OptionalInt(group) + RecipeBookCategory(VarInt)
  //   + Optional<List<Ingredient>> + flags(byte)
  // 服务端只要把“已解锁的配方”按这个格式推下去，工作台的配方书里就能看到并直接点。
  // RecipeDisplay 是 dispatch：VarInt 类型 id（crafting_shaped=1）+ 本体。
  // crafting_shaped 本体 = width + height + ingredients(list<SlotDisplay>) + result + craftingStation。
  // SlotDisplay 也是 dispatch：empty=0 / item=4（后面跟一个物品 id）。
  uint8_t* buf = (uint8_t*)heap_caps_malloc(12000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf == nullptr) return false;
  uint8_t* p = buf;
  int n = craftRecipeCount();
  rbVar(p, (uint32_t)n);
  for (int i = 0; i < n; i++) {
    uint8_t w = 0, h = 0, cnt = 0;
    uint16_t need[9] = {0};
    uint16_t out = 0;
    if (!craftRecipeInfo(i, &w, &h, need, &out, &cnt)) continue;
    rbVar(p, (uint32_t)i);            // RecipeDisplayId
    rbVar(p, 1);                      // display 类型 = crafting_shaped
    rbVar(p, w); rbVar(p, h);
    rbVar(p, (uint32_t)w * (uint32_t)h);
    for (uint8_t dr = 0; dr < h; dr++) {
      for (uint8_t dc = 0; dc < w; dc++) {
        uint16_t it = need[dr * 3 + dc];   // 内部表是 3×3 行优先
        if (it == 0) { rbVar(p, 0); }      // SlotDisplay.empty
        else { rbVar(p, 4); rbVar(p, it); } // SlotDisplay.item(id)
      }
    }
    rbVar(p, 4); rbVar(p, out);                      // result：SlotDisplay.item(产物)
    rbVar(p, 4); rbVar(p, 333);                      // craftingStation：工作台物品(333)
    rbVar(p, 0);                                     // OptionalInt 组 = 空
    rbVar(p, 3);                                     // RecipeBookCategory = crafting_misc(3)
    rbVar(p, 0);                                     // 材料需求表 = 空（只显示，不做“可合成”高亮）
    if ((size_t)(p - buf) + 4 > 11800) break;         // 别写爆缓冲：踩坏 PSRAM 会让进服直接重启
    *p++ = 0;                                        // flags：无提醒/无高亮
  }
  *p++ = 1;                                          // replace = true（先清空再加）
  size_t body = (size_t)(p - buf);
  mcDiag_("[RB] recipes=%d body=%u", n, (unsigned)body);
  uint32_t total = (uint32_t)(body + 1);
  pktLog_(0x4A, total);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(total) && codec.writeByte(0x4A) && codec.writeExact(buf, body);
  pktCheck_(codec, start, total, "id0x4A");
  heap_caps_free(buf);
  return ok;
}

bool MinecraftServer::sendUpdateRecipes_(PacketCodec& codec) {
  // 原版锻造台三格各收什么，由这三张「配方属性表」决定（客户端本地校验，不在表里就放不进去）。
  // 只放真正的原版成员：模板=各类锻造模板；底材=钻石/下界合金的工具与盔甲；附加=下界合金锭。
  static const uint16_t kTpl[] = {1427,1428,1429,1430,1431,1432,1433,1434,1435,1436,1437,1438,1439,1440,1441,1442,1443,1444,1445};
  static const uint16_t kBase[] = {937,938,939,940,941,942,943,944,945,946,971,972,973,974,979,980,981,982};
  static const uint16_t kAdd[] = {910};
  static const uint16_t kSize[3] = {19, 18, 1};
  static const char* const kKey[3] = {"minecraft:smithing_template","minecraft:smithing_base","minecraft:smithing_addition"};
  static const uint32_t kKeyLen[3] = {27, 23, 27};
  const uint16_t* kSet[3] = {kTpl, kBase, kAdd};
  uint32_t pkt_len = 2 + 1 + 1;                 // 包 id(133 需 2 字节) + 表数量(3) + 切石机表数量(0)
  for (int s = 0; s < 3; s++) {
    pkt_len += codec.sizeVarInt(kKeyLen[s]) + kKeyLen[s] + codec.sizeVarInt(kSize[s]);
    for (int i = 0; i < kSize[s]; i++) pkt_len += codec.sizeVarInt(kSet[s][i]);
  }
  pktLog_(0x85, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeVarInt(133) && codec.writeVarInt(3);
  for (int s = 0; s < 3; s++) {
    ok = ok && codec.writeVarInt(kKeyLen[s]) && codec.writeExact((const uint8_t*)kKey[s], kKeyLen[s]);
    ok = ok && codec.writeVarInt(kSize[s]);
    for (int i = 0; i < kSize[s] && ok; i++) ok = codec.writeVarInt(kSet[s][i]);
  }
  ok = ok && codec.writeVarInt(0);              // 切石机空表
  pktCheck_(codec, start, pkt_len, "id0x85");
  return ok;
}
bool MinecraftServer::sendOpenScreenTyped_(PacketCodec& codec, uint8_t container_id, uint8_t menu_type, const char* title, uint16_t length) {
  uint32_t pkt_len = 1 + 2 * codec.sizeVarInt(container_id) + 2 + length;
  pktLog_(0x3B, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x3B) &&
         codec.writeVarInt(container_id) && codec.writeVarInt(menu_type) &&
         codec.writeByte(8) && codec.writeUint16(length) && codec.writeExact((const uint8_t*)title, length);
  pktCheck_(codec, start, pkt_len, "id0x3B");
  return ok;
}

bool MinecraftServer::sendSetExperience_(PacketCodec& codec, float progress, int32_t level, int32_t total) {
  uint32_t pkt_len = 1 + 4 + codec.sizeVarInt((uint32_t)level) + codec.sizeVarInt((uint32_t)total);
  pktLog_(0x67, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x67) &&
         codec.writeFloat(progress) && codec.writeVarInt((uint32_t)level) && codec.writeVarInt((uint32_t)total);
  pktCheck_(codec, start, pkt_len, "id0x67");
  return ok;
}

bool MinecraftServer::sendSetContainerData_(PacketCodec& codec, uint8_t window, int16_t data_id, int16_t value) {
  uint32_t pkt_len = 1 + codec.sizeVarInt(window) + 2 + 2;
  pktLog_(0x13, pkt_len);
  size_t start = codec.writeCount();
  bool ok = codec.writeVarInt(pkt_len) && codec.writeByte(0x13) && codec.writeVarInt(window) &&
         codec.writeUint16((uint16_t)data_id) && codec.writeUint16((uint16_t)value);
  pktCheck_(codec, start, pkt_len, "id0x13");
  return ok;
}
bool MinecraftServer::sendRespawn_(PacketCodec& codec, uint8_t mode) {
  const char* dim = "overworld";
  return codec.writeVarInt(28) && codec.writeByte(0x52) && codec.writeVarInt(0) &&
         codec.writeVarInt(9) && codec.writeExact((const uint8_t*)dim, 9) &&
         codec.writeUint64(0x0123456789ABCDEFULL) && codec.writeByte(mode) &&
         codec.writeByte(0xFF) && codec.writeByte(0) && codec.writeByte(0) &&
         codec.writeByte(0) && codec.writeVarInt(0) && codec.writeVarInt(63) && codec.writeByte(0);
}

bool MinecraftServer::sendPlayerAbilities_(PacketCodec& codec, uint8_t flags) {
  return codec.writeVarInt(10) && codec.writeByte(0x40) && codec.writeByte(flags) &&
         codec.writeFloat(0.05f) && codec.writeFloat(0.1f);
}

// ============================================================
// 区块数据
// ============================================================

bool MinecraftServer::sendChunkDataAndUpdateLight_(PacketCodec& codec, int chunk_x, int chunk_z) {
  static const int TOTAL_SECTIONS = 24;
  static const int EMPTY_BELOW = 4;
  static const int GEN_SECTIONS = 6;
  static const int EMPTY_ABOVE = TOTAL_SECTIONS - EMPTY_BELOW - GEN_SECTIONS;
  // 只给真正有方块的 section 发天空光数据（EMPTY_BELOW..EMPTY_BELOW+GEN_SECTIONS-1）
  // 其余 section 放进 empty 掩码（客户端视作全亮），每区块省约 20KB 传输量
  static const int LIT_FIRST = 4;                            // = EMPTY_BELOW
  static const int LIT_LAST = 4 + 6 - 1;                     // = EMPTY_BELOW + GEN_SECTIONS - 1
  static const int LIT_SECTIONS = LIT_LAST - LIT_FIRST + 1;  // 6

  int cx = chunk_x * 16, cz = chunk_z * 16;
  int bedrock_sec_size = 2 + 2 + 1 + codec.sizeVarInt(block_palette[B_bedrock]) + 1 + 1;
  int air_sec_size = 2 + 2 + 1 + codec.sizeVarInt(block_palette[B_air]) + 1 + 1;

  static uint8_t enc[4400];  // 实测单段最大 2596B，收紧以缓解 dram0 压力
  static uint64_t longs_buf[512];
  static uint16_t sec_enc_lens[6];
  // 区块只生成一遍：原来“算长度”和“真正编码”各 buildChunkSection 一次，
  // 等于每区块白算 6 段地形（实测每区块 ~1.2s，视距一调大进服就卡几分钟）。
  // 缓存放在 PSRAM（板子是 8MB PSRAM 的 LilyGO T-Call V1.4），DRAM 装不下这 29KB。
  static uint8_t  (*sec_raw)[4096] = nullptr;      // 6 × 4KB = 24KB
  static uint16_t (*sec_pal_idx)[256] = nullptr;   // 6 × 512B = 3KB
  static uint8_t  (*sec_pal_block)[256] = nullptr; // 6 × 256B = 1.5KB
  static bool cache_inited = false;
  static bool cache_ok = false;
  static uint16_t sec_pal_len[6];
  static uint8_t  sec_bits[6];
  static uint16_t sec_non_air[6];
  static uint8_t  sec_biome[6];

  if (!cache_inited) {
    cache_inited = true;
    void *blk = heap_caps_malloc(6 * 4096 + 6 * 256 * 2 + 6 * 256, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (blk) {
      uint8_t *p = (uint8_t *)blk;
      sec_raw = (uint8_t (*)[4096])p;              p += 6 * 4096;
      sec_pal_idx = (uint16_t (*)[256])p;          p += 6 * 256 * 2;
      sec_pal_block = (uint8_t (*)[256])p;
      cache_ok = true;
    }
    Serial.printf("[CHUNK] section cache %s, psram=%u\r\n", cache_ok ? "OK(PSRAM)" : "FAILED",
                  (unsigned)ESP.getPsramSize());
  }
  if (!cache_ok) return false;   // 没有 PSRAM 就不发区块（宁可不发，也不能发出错的地形）

  // 先计算每个 section 的编码长度
  int chunk_data_size = bedrock_sec_size * EMPTY_BELOW + air_sec_size * EMPTY_ABOVE;

  for (int sec = 0; sec < GEN_SECTIONS; sec++) {
    sec_biome[sec] = buildChunkSectionInto(sec_raw[sec], cx, sec * 16, cz);
    uint16_t palette_len = 0; uint16_t non_air = 0;
    uint16_t *pidx = sec_pal_idx[sec];
    uint8_t *pblk = sec_pal_block[sec];
    memset(pidx, 0xFF, sizeof(sec_pal_idx[sec]));
    for (int j = 0; j < 4096; j++) {
      uint8_t b = sec_raw[sec][j];
      if (b != B_air) non_air++;
      if (pidx[b] == 0xFFFF) { pidx[b] = palette_len; pblk[palette_len] = b; palette_len++; }
    }
    uint8_t bits = 0;
    if (palette_len > 1) { uint16_t m = palette_len - 1; while (m > 0) { bits++; m >>= 1; } if (bits < 4) bits = 4; }
    sec_pal_len[sec] = palette_len;
    sec_bits[sec] = bits;
    sec_non_air[sec] = non_air;
    int size = 2 + 2 + 1; // block_count + fluid_count + bits
    if (bits == 0) {
      size += codec.sizeVarInt(block_palette[pblk[0]]);
    } else {
      size += codec.sizeVarInt(palette_len);
      for (uint16_t p = 0; p < palette_len; p++) size += codec.sizeVarInt(block_palette[pblk[p]]);
      int epl = 64 / bits;                       // 每个 long 装的条目数（不能跨边界）
      size += ((4096 + epl - 1) / epl) * 8;
    }
    size += 2; // biome
    sec_enc_lens[sec] = (uint16_t)size;
    chunk_data_size += size;
  }

  // ---- 高度图：1.21.5+ 的 Prefixed Array of Heightmap ----
  // 每项 = (VarInt 类型) + (Prefixed Array of Long)
  //   type 4 = MOTION_BLOCKING, type 1 = WORLD_SURFACE
  static const int HM_LONGS = 37;   // 9bit × 256，每 long 装 7 个（不跨边界）
  int hm_len = 1 + 2 * (1 + 1 + HM_LONGS * 8);   // count(1) + 2*(type + len + longs)

  int light_data_size = 9 + 1 + 9 + 1 + 1 + LIT_SECTIONS * (2 + 2048) + 1;
  int pkt_id_size = codec.sizeVarInt(0x2D);
  int data_len_size = codec.sizeVarInt((uint32_t)chunk_data_size);
  uint32_t total_pkt_len = pkt_id_size + 4 + 4 + hm_len
                           + data_len_size + chunk_data_size + 1 + light_data_size;

  codec.resetWriteCount();
  if (!codec.writeVarInt(total_pkt_len)) return false;
  if (!codec.writeVarInt(0x2D)) return false;
  if (!codec.writeUint32((uint32_t)chunk_x)) return false;
  if (!codec.writeUint32((uint32_t)chunk_z)) return false;
  // heightmaps（流式，不占静态 DRAM）
  if (!codec.writeVarInt(2)) return false;                 // Prefixed Array: 2 张
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < HM_LONGS; i++) longs_buf[i] = 0;
    for (int z = 0; z < 16; z++) {
      for (int xx = 0; xx < 16; xx++) {
        int top = (int)getHeightAt(cx + xx, cz + z);
        if (top > 95) top = 95;
        uint32_t value = (uint32_t)(top + 64);   // 世界底部 y = -64
        uint32_t idx = (uint32_t)(z * 16 + xx);  // x 变化最快
        uint32_t word = idx / 7;                 // 每 long 7 个 9bit 条目
        uint8_t off = (uint8_t)((idx % 7) * 9);
        longs_buf[word] |= (uint64_t)value << off;
      }
    }
    if (!codec.writeVarInt(pass == 0 ? 4u : 1u)) return false;  // MOTION_BLOCKING / WORLD_SURFACE
    if (!codec.writeVarInt((uint32_t)HM_LONGS)) return false;
    for (int i = 0; i < HM_LONGS; i++) if (!codec.writeUint64(longs_buf[i])) return false;
    yield();
  }
  if (!codec.writeVarInt((uint32_t)chunk_data_size)) return false;

  // 底部基岩 sections
  for (int i = 0; i < EMPTY_BELOW; i++) {
    if (!codec.writeUint16(4096) || !codec.writeUint16(0) || !codec.writeByte(0)) return false;
    if (!codec.writeVarInt(block_palette[B_bedrock]) || !codec.writeByte(0) || !codec.writeByte(0)) return false;
  }

  // 生成的 sections（地形在第一遍就算好了，这里不再重新生成）
  for (int sec = 0; sec < GEN_SECTIONS; sec++) {
    uint8_t biome = sec_biome[sec];
    uint16_t *palette_index = sec_pal_idx[sec];
    uint8_t *palette_blocks = sec_pal_block[sec];
    uint16_t palette_len = sec_pal_len[sec];
    uint16_t non_air = sec_non_air[sec];
    uint8_t bits = sec_bits[sec];
    uint8_t *raw = sec_raw[sec];

    size_t used = 0;
    enc[used++] = (uint8_t)(non_air >> 8); enc[used++] = (uint8_t)non_air;
    enc[used++] = 0; enc[used++] = 0; // fluid count
    enc[used++] = bits;
    if (bits == 0) {
      uint32_t val = block_palette[palette_blocks[0]];
      do { uint8_t b = val & 0x7F; val >>= 7; if (val) b |= 0x80; enc[used++] = b; } while (val);
    } else {
      uint32_t pl = palette_len;
      do { uint8_t b = pl & 0x7F; pl >>= 7; if (pl) b |= 0x80; enc[used++] = b; } while (pl);
      for (uint16_t p = 0; p < palette_len; p++) {
        uint32_t val = block_palette[palette_blocks[p]];
        do { uint8_t b = val & 0x7F; val >>= 7; if (val) b |= 0x80; enc[used++] = b; } while (val);
      }
      // 数据数组：条目不能跨 long 边界（1.16+ 规范），长度不再随包发送
      uint32_t epl = 64 / bits;                        // 每个 long 装几个条目
      uint32_t long_count = (4096 + epl - 1) / epl;
      memset(longs_buf, 0, long_count * 8);
      for (int j = 0; j < 4096; j++) {
        uint64_t value = palette_index[raw[j]];
        uint32_t word_index = (uint32_t)j / epl;
        uint8_t bit_offset = (uint8_t)(((uint32_t)j % epl) * bits);
        longs_buf[word_index] |= value << bit_offset;
      }
      for (uint32_t j = 0; j < long_count; j++) {
        uint64_t be = __builtin_bswap64(longs_buf[j]);
        memcpy(enc + used, &be, 8); used += 8;
      }
    }
    // biome: single-valued palette
    enc[used++] = 0; enc[used++] = biome;

    if (used != (size_t)sec_enc_lens[sec]) {
      Serial.printf("[SEC!!] chunk %d sec %d predicted=%u used=%u\n", chunk_x, sec, sec_enc_lens[sec], (unsigned)used);
    }
    if (!codec.writeExact(enc, sec_enc_lens[sec])) return false;
    yield();
  }

  // 顶部空气 sections
  for (int i = 0; i < EMPTY_ABOVE; i++) {
    if (!codec.writeUint16(0) || !codec.writeUint16(0) || !codec.writeByte(0)) return false;
    if (!codec.writeVarInt(block_palette[B_air]) || !codec.writeByte(0) || !codec.writeByte(0)) return false;
  }
  if (!codec.writeVarInt(0)) return false; // block entities

  // 光照数据
  // mask 位 0 对应世界最低段下面一段，世界第 k 段 -> bit k+1；总位数 = 段数 + 2
  uint64_t sky_light_mask = ((1ULL << (LIT_LAST + 2)) - 1) & ~((1ULL << (LIT_FIRST + 1)) - 1);
  uint64_t empty_sky_mask = ((1ULL << (TOTAL_SECTIONS + 2)) - 1) & ~sky_light_mask;
  if (!codec.writeVarInt(1) || !codec.writeUint64(sky_light_mask)) return false;
  if (!codec.writeVarInt(0)) return false;  // block light mask
  if (!codec.writeVarInt(1) || !codec.writeUint64(empty_sky_mask)) return false;
  if (!codec.writeVarInt(0)) return false;  // empty block light mask
  if (!codec.writeVarInt(LIT_SECTIONS)) return false;
  memset(chunk_section, 0xFF, 2048);
  for (int i = 0; i < LIT_SECTIONS; i++) {
    if (!codec.writeVarInt(2048) || !codec.writeExact(chunk_section, 2048)) return false;
    yield();
  }
  if (!codec.writeVarInt(0)) return false; // block light arrays
  {
    static int dbg_chunks = 0;
    if (dbg_chunks < 3) {
      size_t actual = codec.writeCount();
      size_t expect = (size_t)codec.sizeVarInt(total_pkt_len) + (size_t)total_pkt_len;
      Serial.printf("[CHUNK] x=%d z=%d declared=%u written=%u diff=%d hm=%d data=%d light=%d mask=%llx/%llx sec=%u,%u,%u,%u,%u,%u\n",
        chunk_x, chunk_z, (unsigned)total_pkt_len, (unsigned)actual, (int)actual - (int)expect,
        hm_len, chunk_data_size, light_data_size,
        (unsigned long long)sky_light_mask, (unsigned long long)empty_sky_mask,
        sec_enc_lens[0], sec_enc_lens[1], sec_enc_lens[2], sec_enc_lens[3], sec_enc_lens[4], sec_enc_lens[5]);
      dbg_chunks++;
    }
  }

  // 发送该区块内的方块改动
  for (int i = 0; i < block_changes_count; i++) {
    if (block_changes[i].block == 0xFF) continue;
    int bx = block_changes[i].x, bz = block_changes[i].z;
    if (bx >= cx && bx < cx + 16 && bz >= cz && bz < cz + 16)
      sendBlockUpdate_(codec, block_changes[i].x, block_changes[i].y, block_changes[i].z, block_changes[i].block);
  }
  return true;
}

// ============================================================
// 辅助方法
// ============================================================

uint16_t MinecraftServer::onlineCount_() const {
  uint16_t count = 0;
  for (uint8_t i = 0; i < kMaxClients; i++)
    if (clients_[i].used && clients_[i].state == STATE_PLAY) count++;
  return count;
}

PacketCodec MinecraftServer::codecForSlot_(uint8_t slot_index) {
  return PacketCodec(clients_[slot_index].fd);
}

int MinecraftServer::slotIndexForPlayer_(PlayerData* player) {
  for (uint8_t i = 0; i < kMaxClients; i++) {
    if (clients_[i].player_index >= 0 && &player_data[clients_[i].player_index] == player) return i;
  }
  return -1;
}


// 创造模式改背包时，客户端给的是“界面槽位号”（36~44 是快捷栏，9~35 是背包，
// 45 是副手，41~44 是护甲），这里换算成内部索引。
static uint8_t clientSlotToServerSlot_(uint16_t cslot) {
  if (cslot >= 36 && cslot <= 44) return (uint8_t)(cslot - 36);  // 快捷栏 0~8
  if (cslot >= 9 && cslot <= 35) return (uint8_t)cslot;          // 背包 9~35
  if (cslot == 45) return 40;                                    // 副手
  // 创造模式包的槽号就是玩家背包菜单的槽号：5=头盔 6=胸甲 7=护腿 8=靴。
  // 原来这里写的是 41~44（那是客户端快捷栏 41~44，上面的 36~44 分支已经吃掉），
  // 于是护甲槽永远返回 255 被丢包：客户端本地已经把快捷栏清空、把护甲搬进 5~8，
  // 服务端却整包丢弃 → “一穿戴上就没了”。日志实证：[CSLOT] cslot=36..39 item=0。
  if (cslot >= 5 && cslot <= 8) return (uint8_t)(44 - cslot);     // 护甲 36~39
  return 255;
}

void MinecraftServer::processDeferredChunks_(uint8_t slot_index) {
  ClientSlot& slot = clients_[slot_index];
  if (slot.chunk_queue_idx < 0) return;
  if (slot.fd < 0) { slot.chunk_queue_idx = -1; return; }

  int side = ACTIVE_VIEW_DISTANCE * 2 + 1;
  int total = side * side - 1; // 排除中心

  if (slot.chunk_queue_idx >= total) {
    slot.chunk_queue_idx = -1;
    if (slot.player_index >= 0) {
      PlayerData* player = &player_data[slot.player_index];
      PacketCodec pc(slot.fd);
      sendSynchronizePlayerPosition_(pc, player->x + 0.5, player->y, player->z + 0.5,
        player->yaw * 180.0f / 127, player->pitch * 90.0f / 127);
      // 关键：26.1.2 客户端在“加载地形中”界面里，只有收到 game_event 的
      // LEVEL_CHUNKS_LOAD_START(=13) 且参数为 1.0 时才调用 loadingPacketsReceived()，
      // 状态机才会从 WaitingForServer 走到 WaitingForPlayerChunk 并关闭该界面。
      // 之前一直没发这一包，客户端就永远停在“加载地形中”（看着像没进世界）。
      sendGameEvent_(pc, 13, 1.0f);
      int pi_join = clients_[slot_index].player_index;
      if (pi_join < 0) pi_join = 0;
      for (uint8_t i = 0; i < 41; i++)
        sendSetContainerSlotEnch_(pc, 0, serverSlotToClientSlot(0, i), player->inventory_count[i],
                                   player->inventory_items[i], enchantGet_(slot_index, i),
                                   g_item_name[pi_join][i][0] ? g_item_name[pi_join][i] : nullptr,
                                   enchantIdGet_(slot_index, i));
      sendSetHeldItem_(pc, player->hotbar);
      sendSetHealth_(pc, player->health, player->hunger, player->saturation);
      sendUpdateTime_(pc, world_time);
      if (g_join_mask & 0x04) {   // 进世界时同步当前天气（雨强必须是 1.0，否则雨拉不起来）
        sendGameEvent_(pc, weather_ == 0 ? 2 : 1, weather_ == 0 ? 0.0f : 1.0f);
        sendGameEvent_(pc, 7, weather_ == 0 ? 0.0f : 1.0f);
        sendGameEvent_(pc, 8, (weather_ == 2) ? 1.0f : 0.0f);
      }
      for (uint8_t d = 0; drops_ && d < kMaxDrops; d++) {   // 地上已有掉落物也补发
        if (drops_[d].item == 0) continue;
        sendSpawnItemEntity_(pc, drops_[d].entity_id, drops_[d].uuid,
                             drops_[d].x + 0.5, (double)drops_[d].y + 0.2, drops_[d].z + 0.5);
        sendSetEntityDataItem_(pc, drops_[d].entity_id, drops_[d].item, drops_[d].count);
      }
      // update_attributes: movement_speed=0.1 (默认值, 让客户端正确计算水中阻力)
      {
        int eid_size = pc.sizeVarInt((uint32_t)(slot.player_index));
        // 1 attribute: id(varint=22) + base(double=0.1) + modifiers_count(varint=0)
        uint32_t attr_pkt_len = pc.sizeVarInt(0x83) + eid_size + pc.sizeVarInt(1)
                              + pc.sizeVarInt(22) + 8 + pc.sizeVarInt(0);
        pc.writeVarInt(attr_pkt_len);
        pc.writeVarInt(0x83);
        pc.writeVarInt((uint32_t)(slot.player_index));
        pc.writeVarInt(1);           // 1 attribute
        pc.writeVarInt(22);          // minecraft:movement_speed
        pc.writeDouble(0.1);         // base value
        pc.writeVarInt(0);           // 0 modifiers
      }
      player->flags &= ~0x20;
    }
    return;
  }

  int idx = slot.chunk_queue_idx;
  int raw = idx;
  int center_offset = ACTIVE_VIEW_DISTANCE * side + ACTIVE_VIEW_DISTANCE;
  if (raw >= center_offset) raw++;
  int ox = (raw / side) - ACTIVE_VIEW_DISTANCE;
  int oz = (raw % side) - ACTIVE_VIEW_DISTANCE;

  PacketCodec pc(slot.fd);
  pc.setWriteTimeout(15000);  // 发块较大，放宽写超时
  unsigned heap_before = ESP.getFreeHeap();
  if (!sendChunkDataAndUpdateLight_(pc, slot.chunk_center_x + ox, slot.chunk_center_z + oz)) {
    Serial.printf("[CHUNK-FAIL] #%d x=%d z=%d heap=%u\n", idx,
                  slot.chunk_center_x + ox, slot.chunk_center_z + oz, heap_before);
    closeClient_(slot_index, 9);  // 包已残缺，连接流已错位，必须断开
    return;
  } else if ((idx % 4) == 0) {
    Serial.printf("[CHUNK-OK] #%d x=%d z=%d heap=%u->%u\n", idx,
                  slot.chunk_center_x + ox, slot.chunk_center_z + oz,
                  heap_before, (unsigned)ESP.getFreeHeap());
  }
  vTaskDelay(1);  // 让出 CPU，避免发块期间触发任务看门狗

  slot.chunk_queue_idx++;
}

// 串口 AT+CID=<hex>：强制指定 declare commands 包 id（0 = 不发送）
void mcCommandTreeSetPacketId(uint8_t id) {
  s_tree_forced_id = id;
  s_tree_confirmed = false;
  s_tree_pending = false;
  Serial.printf("[CMD] declare_commands forced id=0x%02X (%s)\r\n",
                (unsigned)id, id == 0 ? "disabled" : "enabled");
}

void mcCommandTreePrintState() {
  Serial.printf("[CMD] tree cand=%u/%u id=0x%02X confirmed=%d forced=0x%02X acks=%u\r\n",
                (unsigned)(s_tree_cand_idx + 1), (unsigned)kTreeCandidateCount,
                (unsigned)s_tree_cur_id, s_tree_confirmed ? 1 : 0,
                (unsigned)s_tree_forced_id, (unsigned)s_tree_acks);
}


void MinecraftServer::debugUseBlock(int16_t x, int16_t y, int16_t z) {
  for (uint8_t i = 0; i < kMaxClients; i++) {
    if (!clients_[i].used || clients_[i].state != STATE_PLAY || clients_[i].player_index < 0) continue;
    sePrintf("[USE] slot=%u at %d,%d,%d block=%u\r\n",
             (unsigned)i, (int)x, (int)y, (int)z, (unsigned)getBlockAt(x, y, z));
    handlePlayerUseItem_(&player_data[clients_[i].player_index], x, y, z, 1);
    return;
  }
  sePrintln("[USE] NO_PLAYER");
}
