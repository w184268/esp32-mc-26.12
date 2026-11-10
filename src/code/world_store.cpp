// SPDX-License-Identifier: GPL-3.0-only
// 世界存档：种子 + 方块改动（SPIFFS 追加日志）
//
// 文件：/w.seed  = magic 'W''S' 0x01 0x00 + uint32 world_seed + uint32 rng_seed
//       /w.log   = 追加日志，每条 7 字节：type(0) x_lo x_hi z_lo z_hi y block
//
// 设计取舍：地形本身是纯函数（种子 + 坐标），不需要存；只存玩家改过的方块。
// 写入按 16 条一批缓冲，15 秒或攒满才落盘，避免频繁擦写 SPIFFS。

#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#include <string.h>

#include "world_store.h"
#include "serial_echo.h"   // 存档诊断输出同时给网页串口控制台
#include "game_state.h"
#include "game_types.h"
#include "procedures.h"
#include "registries.h"

namespace {

const char *kSeedPath = "/w.seed";
const char *kSeedTmpPath = "/w.seed.tmp";
const char *kLogPath = "/w.log";
const char *kLogTmpPath = "/w.log.tmp";
const char *kLogBakPath = "/w.log.bak";
const char *kLogBadPath = "/w.log.bad";
const char *kStatePath = "/w.state";        // 楼梯/梯子的 state id（7 字节/条，独立文件，不动 /w.log）

const uint32_t kLogCompactBytes = 200000;   // 超过这个就压缩一次
const uint8_t kChestBytes = 81;             // 27 格 × 3 字节（item 2 + count 1）
const unsigned long kPlayerSaveIntervalMs = 10000;  // 玩家档 10 秒存一次（掉电最多丢 10 秒的位置/背包）
const size_t kPendingCap = 16;              // 每批最多缓存 16 条
const unsigned long kFlushIntervalMs = 2000;  // 改动最多 2 秒就落盘（旧版 15 秒，掉电窗口太大）

bool g_mounted = false;
bool g_seed_loaded = false;
bool g_replaying = false;

// 待落盘的记录缓冲（16 × 7 = 112 字节）
uint8_t g_pending[kPendingCap * 7];
size_t g_pending_count = 0;
unsigned long g_last_flush_ms = 0;
unsigned long g_last_player_save_ms = 0;

// 方块状态缓冲（16 × 7 = 112 字节）
uint8_t g_spending[kPendingCap * 7];
size_t g_spending_count = 0;

void encodeRecord(uint8_t *out, int16_t x, uint8_t y, int16_t z, uint8_t block) {
  out[0] = 0;                       // 记录类型：普通方块改动
  out[1] = (uint8_t)(x & 0xFF);
  out[2] = (uint8_t)((x >> 8) & 0xFF);
  out[3] = (uint8_t)(z & 0xFF);
  out[4] = (uint8_t)((z >> 8) & 0xFF);
  out[5] = y;
  out[6] = block;
}

long logBytes() {
  if (!SPIFFS.exists(kLogPath)) return -1;
  File f = SPIFFS.open(kLogPath, FILE_READ);
  if (!f) return -1;
  long sz = (long)f.size();
  f.close();
  return sz;
}

// 只负责让 /w.log 存在。不再长期持有 append 句柄：
// 旧固件一直开着它，边写边让玩家存档 rename 别的文件，最后 SPIFFS 索引坏了，
// /w.log 变成“有长度、读不出内容”的坏档，开机回放永远是 0 条。
bool ensureLogExists() {
  if (SPIFFS.exists(kLogPath)) return true;
  File f = SPIFFS.open(kLogPath, FILE_WRITE);
  if (!f) return false;
  f.close();
  return true;
}

// 把缓冲写进 /w.log。旧版不检查写入结果，写失败就静默把改动丢掉；
// 现在失败会保留缓冲、下次 tick 重试，并在串口报警。
void flushPending() {
  if (g_pending_count == 0) return;
  if (!g_mounted) {
    Serial.println("[WORLD] SPIFFS 未挂载，改动无法落盘");
    g_pending_count = 0;
    return;
  }
  if (!ensureLogExists()) {
    Serial.println("[WORLD] 日志打开失败，改动留在内存下次重试");
    return;                                   // 缓冲保留
  }
  const size_t want = g_pending_count * 7;
  File f = SPIFFS.open(kLogPath, FILE_APPEND);
  if (!f) {
    Serial.println("[WORLD] 日志打开失败，改动留在内存下次重试");
    return;
  }
  const size_t before = (size_t)f.size();
  const size_t wrote = f.write(g_pending, want);
  f.flush();
  const size_t after = (size_t)f.size();
  f.close();                                  // 写完立刻关闭，不长期持有句柄

  if (wrote != want) {
    Serial.printf("[WORLD] 落盘失败 wrote=%u/%u size=%u→%u，改动留在内存下次重试\n",
                  (unsigned)wrote, (unsigned)want, (unsigned)before, (unsigned)after);
    // 写入成功了一部分：把已经进盘的那几条从缓冲里去掉，避免下次重复写
    if (wrote >= 7 && (wrote % 7) == 0 && wrote < want) {
      const size_t used = wrote / 7;
      memmove(g_pending, g_pending + wrote, want - wrote);
      g_pending_count -= used;
    }
    return;
  }
  // 写完立刻回读最后一条，确认真的能在下次开机读回来
  {
    File rf = SPIFFS.open(kLogPath, FILE_READ);
    if (rf) {
      uint8_t verify[7] = {0};
      const uint8_t *expect = &g_pending[(g_pending_count - 1) * 7];
      long off = (long)((after >= want) ? (after - 7) : (before + want - 7));
      size_t got = 0;
      if (rf.seek((size_t)off)) got = rf.read(verify, 7);
      rf.close();
      if (got != 7 || memcmp(verify, expect, 7) != 0) {
        Serial.printf("[WORLD] 回读校验失败 off=%ld got=%u（写入可能没真正落盘）\n",
                      off, (unsigned)got);
        return;                               // 缓冲保留，下次重写
      }
    }
  }
  g_pending_count = 0;
  g_last_flush_ms = millis();
}

// 把状态缓冲写进 /w.state（每批 7 字节/条，写完立刻关闭句柄）。
void flushStates() {
  if (g_spending_count == 0) return;
  if (!g_mounted) { g_spending_count = 0; return; }
  const size_t want = g_spending_count * 7;
  File f = SPIFFS.open(kStatePath, FILE_APPEND);
  if (!f) { Serial.println("[WORLD] /w.state 打开失败，状态留内存下次重试"); return; }
  const size_t wrote = f.write(g_spending, want);
  f.flush();
  f.close();
  if (wrote != want) {
    Serial.printf("[WORLD] 状态落盘失败 wrote=%u/%u\n", (unsigned)wrote, (unsigned)want);
    if (wrote >= 7 && (wrote % 7) == 0) {
      const size_t used = wrote / 7;
      memmove(g_spending, g_spending + wrote, want - wrote);
      g_spending_count -= used;
    }
    return;
  }
  g_spending_count = 0;
}

// 开机把 /w.state 读回来（同一坐标后写的覆盖先写的）。
void loadStates() {
  if (!g_mounted || !SPIFFS.exists(kStatePath)) return;
  File f = SPIFFS.open(kStatePath, FILE_READ);
  if (!f) return;
  const long bytes = (long)f.size();
  uint8_t rec[7];
  long pos = 0;
  size_t applied = 0;
  while (pos + 7 <= bytes) {
    if (f.read(rec, 7) != 7) break;
    pos += 7;
    int16_t x = (int16_t)(rec[0] | (rec[1] << 8));
    int16_t z = (int16_t)(rec[2] | (rec[3] << 8));
    uint8_t y = rec[4];
    uint16_t st = (uint16_t)(rec[5] | (rec[6] << 8));
    if (st == 0) continue;
    blockStateExtraSet(x, y, z, st);
    applied++;
    if ((applied % 256) == 0) yield();
  }
  f.close();
  Serial.printf("[WORLD] 载入方块状态 %u 条\n", (unsigned)applied);
}

// 箱子内容现在存在独立的 chest_stores[] 里，这里按坐标查它的数据段
const uint8_t *findChestData(int16_t x, uint8_t y, int16_t z) {
  ChestStore *cs = findChestStore(x, y, z);
  return cs ? cs->items : nullptr;
}

void appendChestRecord(int16_t x, uint8_t y, int16_t z, const uint8_t *data) {
  flushPending();   // 保证顺序：箱子方块记录先落盘
  if (!ensureLogExists()) return;
  uint8_t head[7];
  head[0] = 1;
  head[1] = (uint8_t)(x & 0xFF);
  head[2] = (uint8_t)((x >> 8) & 0xFF);
  head[3] = (uint8_t)(z & 0xFF);
  head[4] = (uint8_t)((z >> 8) & 0xFF);
  head[5] = y;
  head[6] = 0;
  File f = SPIFFS.open(kLogPath, FILE_APPEND);
  if (!f) return;
  f.write(head, 7);
  f.write(data, kChestBytes);
  f.flush();
  f.close();
}

// 把内存里现有的改动整体重写成一份干净日志（压缩用）
void compactLog() {
  Serial.println("[WORLD] 压缩存档日志...");
  flushPending();

  File tmp = SPIFFS.open(kLogTmpPath, FILE_WRITE);
  if (!tmp) {
    Serial.println("[WORLD] 压缩失败（无法创建临时文件）");
    ensureLogExists();
    return;
  }

  uint8_t rec[7];
  size_t written = 0;
  for (int i = 0; i < block_changes_count && i < MAX_BLOCK_CHANGES; i++) {
    if (block_changes[i].block == 0xFF) continue;
    encodeRecord(rec, block_changes[i].x, block_changes[i].y, block_changes[i].z, block_changes[i].block);
    tmp.write(rec, 7);
    written++;
    if (block_changes[i].block == B_chest) {
      const uint8_t *data = findChestData(block_changes[i].x, block_changes[i].y, block_changes[i].z);
      if (data) {
        uint8_t head[7];
        head[0] = 1;
        head[1] = (uint8_t)(block_changes[i].x & 0xFF);
        head[2] = (uint8_t)((block_changes[i].x >> 8) & 0xFF);
        head[3] = (uint8_t)(block_changes[i].z & 0xFF);
        head[4] = (uint8_t)((block_changes[i].z >> 8) & 0xFF);
        head[5] = block_changes[i].y;
        head[6] = 0;
        tmp.write(head, 7);
        tmp.write(data, kChestBytes);
        written++;
      }
    }
    written++;
    if (written % 256 == 0) yield();
  }
  size_t expect_bytes = 0;
  {
    // 先按同样规则算一遍总字节数，写完对比，避免半截 tmp 覆盖好档
    for (int i = 0; i < block_changes_count && i < MAX_BLOCK_CHANGES; i++) {
      if (block_changes[i].block == 0xFF) continue;
      expect_bytes += 7;
      if (block_changes[i].block == B_chest &&
          findChestData(block_changes[i].x, block_changes[i].y, block_changes[i].z) != nullptr) {
        expect_bytes += 7 + kChestBytes;
      }
    }
  }
  tmp.flush();
  size_t real_bytes = (size_t)tmp.size();
  tmp.close();

  if (real_bytes != expect_bytes) {
    // 写不完整（多半是 Flash 满了）：丢弃 tmp，旧的 /w.log 原样保留
    Serial.printf("[WORLD] 压缩写不完整(%u/%u)，保留旧日志\n",
                  (unsigned)real_bytes, (unsigned)expect_bytes);
    SPIFFS.remove(kLogTmpPath);
    ensureLogExists();
    return;
  }

  // 换档顺序：旧日志先改名成 .bak → 新日志就位 → 最后删 .bak。
  // 旧版是 remove(/w.log) 之后才 rename，两步之间掉电整份改动日志就没了。
  SPIFFS.remove(kLogBakPath);
  if (SPIFFS.exists(kLogPath) && !SPIFFS.rename(kLogPath, kLogBakPath)) {
    Serial.println("[WORLD] 压缩：旧日志改名失败，放弃本次压缩");
    SPIFFS.remove(kLogTmpPath);
    ensureLogExists();
    return;
  }
  if (!SPIFFS.rename(kLogTmpPath, kLogPath)) {
    Serial.println("[WORLD] 压缩重命名失败，回滚旧日志");
    if (SPIFFS.exists(kLogBakPath)) SPIFFS.rename(kLogBakPath, kLogPath);
    SPIFFS.remove(kLogTmpPath);
    ensureLogExists();
    return;
  }
  SPIFFS.remove(kLogBakPath);
  ensureLogExists();
  Serial.printf("[WORLD] 压缩完成，%u 条（%u 字节）\n", (unsigned)written, (unsigned)real_bytes);
}

} // namespace

void worldStoreBegin() {
  g_mounted = SPIFFS.begin(true);   // 首次使用自动格式化
  if (!g_mounted) {
    Serial.println("[WORLD] SPIFFS 挂载失败：本次改动不会保存（检查分区是否含 SPIFFS）");
    return;
  }

  // 掉电恢复顺序：主日志缺失时先用 /w.log.tmp（压缩写完整了但还没改名），
  // 其次用 /w.log.bak（旧日志已改名、新日志还没就位）。
  if (!SPIFFS.exists(kLogPath)) {
    if (SPIFFS.exists(kLogTmpPath) && SPIFFS.rename(kLogTmpPath, kLogPath)) {
      Serial.println("[WORLD] 从 /w.log.tmp 恢复改动日志");
    } else if (SPIFFS.exists(kLogBakPath) && SPIFFS.rename(kLogBakPath, kLogPath)) {
      Serial.println("[WORLD] 从 /w.log.bak 恢复改动日志");
    } else {
      Serial.println("[WORLD] 没有可恢复的改动日志");
    }
  } else {
    // 主日志在：残留的 tmp 一般是无用的，但如果主日志是空的而 tmp 有内容，
    // 说明上次压缩倒在“改名”之前，tmp 才是真数据。
    if (SPIFFS.exists(kLogTmpPath)) {
      long main_size = 0, tmp_size = 0;
      File mf = SPIFFS.open(kLogPath, FILE_READ);
      if (mf) { main_size = (long)mf.size(); mf.close(); }
      File tf = SPIFFS.open(kLogTmpPath, FILE_READ);
      if (tf) { tmp_size = (long)tf.size(); tf.close(); }
      if (main_size < 7 && tmp_size >= 7) {
        SPIFFS.remove(kLogPath);
        if (SPIFFS.rename(kLogTmpPath, kLogPath))
          Serial.println("[WORLD] 主日志为空，已用 /w.log.tmp 覆盖");
      } else {
        SPIFFS.remove(kLogTmpPath);
      }
    }
    if (SPIFFS.exists(kLogBakPath)) SPIFFS.remove(kLogBakPath);
  }

  // 坏档检测：旧固件长期保持 append 句柄，可能写出“有长度但读不出内容”的 /w.log。
  // 这种文件留着只会让开机回放永远是 0 条，这里直接挪走、重新开始记录。
  if (SPIFFS.exists(kLogPath)) {
    File cf = SPIFFS.open(kLogPath, FILE_READ);
    if (cf) {
      long sz = (long)cf.size();
      uint8_t probe[7] = {0};
      size_t got = cf.read(probe, 7);
      cf.close();
      if (sz >= 7 && got != 7) {
        SPIFFS.remove(kLogBadPath);
        if (SPIFFS.rename(kLogPath, kLogBadPath)) {
          Serial.printf("[WORLD] /w.log 读不出内容(%ld 字节)，已改名 /w.log.bad 并重新开始\n", sz);
          ensureLogExists();
        } else {
          Serial.println("[WORLD] /w.log 坏档改名失败");
        }
      }
    }
  }

  if (!SPIFFS.exists(kSeedPath) && SPIFFS.exists(kSeedTmpPath)) {
    SPIFFS.rename(kSeedTmpPath, kSeedPath);
  }

  File f = SPIFFS.open(kSeedPath, FILE_READ);
  if (f && f.size() >= 12) {
    uint8_t head[4];
    f.read(head, 4);
    if (head[0] == 'W' && head[1] == 'S' && head[2] == 0x01) {
      uint32_t ws = 0, rs = 0;
      f.read((uint8_t *)&ws, 4);
      f.read((uint8_t *)&rs, 4);
      if (ws != 0) {
        world_seed = ws;
        rng_seed = rs;
        g_seed_loaded = true;
      }
    }
  }
  if (f) f.close();

  if (g_seed_loaded) {
    Serial.println("[WORLD] 已载入世界种子");
  }
}

bool worldStoreSeedLoaded() { return g_seed_loaded; }

void worldStoreSaveSeed() {
  if (!g_mounted) return;
  // 原子写：/w.seed.tmp 写完校验 → 替换 /w.seed，断电不会留半截种子
  File f = SPIFFS.open(kSeedTmpPath, FILE_WRITE);
  if (!f) { Serial.println("[WORLD] 种子写入失败"); return; }
  uint8_t head[4] = {'W', 'S', 0x01, 0x00};
  bool ok = (f.write(head, 4) == 4) &&
            (f.write((uint8_t *)&world_seed, 4) == 4) &&
            (f.write((uint8_t *)&rng_seed, 4) == 4);
  f.flush();
  size_t real = (size_t)f.size();
  f.close();
  if (!ok || real != 12) {
    Serial.println("[WORLD] 种子写入不完整，保留旧种子");
    SPIFFS.remove(kSeedTmpPath);
    return;
  }
  SPIFFS.remove(kSeedPath);
  if (!SPIFFS.rename(kSeedTmpPath, kSeedPath)) {
    Serial.println("[WORLD] 种子重命名失败");
    return;
  }
  Serial.println("[WORLD] 种子已保存");
}

void worldStoreReplayChanges() {
  if (!g_mounted) return;

  File f = SPIFFS.open(kLogPath, FILE_READ);
  if (!f) {
    Serial.println("[WORLD] 无历史改动，新世界");
    ensureLogExists();
    return;
  }

  g_replaying = true;
  uint8_t rec[7];
  size_t applied = 0, skipped = 0, chests = 0;
  const long log_bytes = (long)f.size();
  // 注意：这块板上的 SPIFFS 实现里 File::available() 会返回 0（旧版用它做循环条件，
  // 导致 /w.log 明明有几千字节，开机却一条都读不出来，玩家挖的方块全部“消失”）。
  // 这里改成自己数已读字节，只依赖 size() 和 read()。
  long pos = 0;
  Serial.printf("[WORLD] 回放开始 size=%ld pos=%ld\n", log_bytes, (long)f.position());
  while (pos + 7 <= log_bytes) {
    if (f.read(rec, 7) != 7) {
      Serial.printf("[WORLD] 回放在 pos=%ld 处读不动，停止\n", pos);
      break;
    }
    pos += 7;
    int16_t x = (int16_t)(rec[1] | (rec[2] << 8));
    int16_t z = (int16_t)(rec[3] | (rec[4] << 8));
    if (rec[0] == 1) {                       // 箱子内容
      uint8_t payload[kChestBytes];
      if (f.read(payload, kChestBytes) != kChestBytes) break;
      pos += kChestBytes;
      if (findChestStore(x, rec[5], z) == nullptr) makeBlockChange(x, rec[5], z, B_chest);
      ChestStore *cs = ensureChestStore(x, rec[5], z);
      if (cs) {
        memcpy(cs->items, payload, kChestBytes);
        chests++;
      } else skipped++;
      continue;
    }
    if (rec[0] != 0) { skipped++; continue; }
    if (makeBlockChange(x, rec[5], z, rec[6]) != 0) skipped++;
    else applied++;
    if ((applied % 256) == 0) yield();
  }
  f.close();
  g_replaying = false;

  Serial.printf("[WORLD] 回放改动 %u 条，箱子 %u 个（跳过 %u），日志 %ld 字节\n",
                (unsigned)applied, (unsigned)chests, (unsigned)skipped, log_bytes);
  g_last_flush_ms = millis();
  ensureLogExists();
  loadStates();     // 方块改动回放完再补 state（被挖掉的格子其 state 会自动失效）
}

void worldStoreNoteChange(int16_t x, uint8_t y, int16_t z, uint8_t block) {
  if (!g_mounted || g_replaying) return;
  if (g_pending_count >= kPendingCap) flushPending();
  encodeRecord(&g_pending[g_pending_count * 7], x, y, z, block);
  g_pending_count++;
}

void worldStoreNoteState(int16_t x, uint8_t y, int16_t z, uint16_t state) {
  if (!g_mounted || g_replaying) return;
  if (g_spending_count >= kPendingCap) flushStates();
  uint8_t *out = &g_spending[g_spending_count * 7];
  out[0] = (uint8_t)(x & 0xFF);
  out[1] = (uint8_t)((x >> 8) & 0xFF);
  out[2] = (uint8_t)(z & 0xFF);
  out[3] = (uint8_t)((z >> 8) & 0xFF);
  out[4] = y;
  out[5] = (uint8_t)(state & 0xFF);
  out[6] = (uint8_t)((state >> 8) & 0xFF);
  g_spending_count++;
}

void worldStoreTick() {
  if (!g_mounted) return;
  unsigned long now = millis();
  if ((now - g_last_player_save_ms) >= kPlayerSaveIntervalMs) worldStoreSavePlayers();
  if (g_spending_count > 0 && (g_spending_count >= kPendingCap || (now - g_last_flush_ms) >= kFlushIntervalMs)) {
    flushStates();
  }
  if (g_pending_count == 0) {
    // 没改动也要偶尔检查日志体积
    if ((now - g_last_flush_ms) < kFlushIntervalMs) return;
    g_last_flush_ms = now;
    { long lb = logBytes(); if (lb > (long)kLogCompactBytes) compactLog(); }
    return;
  }
  if (g_pending_count >= kPendingCap || (now - g_last_flush_ms) >= kFlushIntervalMs) {
    flushPending();
    { long lb = logBytes(); if (lb > (long)kLogCompactBytes) compactLog(); }
  }
}

void worldStoreSaveChest(int16_t x, uint8_t y, int16_t z) {
  if (!g_mounted) return;
  const uint8_t *data = findChestData(x, y, z);
  if (!data) return;
  appendChestRecord(x, y, z, data);
}

bool worldStoreHasPlayer(int idx) {
  if (!g_mounted || idx < 0 || idx >= MAX_PLAYERS) return false;
  char path[16];
  snprintf(path, sizeof(path), "/p%d.dat", idx);
  return SPIFFS.exists(path);
}

void worldStoreSavePlayer(int idx) {
  if (!g_mounted || idx < 0 || idx >= MAX_PLAYERS) return;
  PlayerData *p = &player_data[idx];
  bool empty = true;
  for (int i = 0; i < 16; i++) { if (p->uuid[i] != 0) { empty = false; break; } }
  if (empty) return;

  // 原子写：先写 /pN.tmp，成功后再换档。
  // 换档顺序改成 bak 三步：/pN.dat → /pN.bak → /pN.tmp → /pN.dat → 删 bak。
  // 旧版是 remove(/pN.dat) 再 rename，两步之间掉电就整份玩家档没了。
  char path[16];
  char tmp[20];
  char bak[20];
  snprintf(path, sizeof(path), "/p%d.dat", idx);
  snprintf(tmp, sizeof(tmp), "/p%d.tmp", idx);
  snprintf(bak, sizeof(bak), "/p%d.bak", idx);
  File f = SPIFFS.open(tmp, FILE_WRITE);
  if (!f) { Serial.println("[WORLD] 玩家存档写入失败"); return; }
  uint8_t head[4] = {'P', 'D', 0x01, 0x00};
  bool ok = (f.write(head, 4) == 4) &&
            (f.write((const uint8_t *)p, sizeof(PlayerData)) == sizeof(PlayerData));  // client_fd 载入时为 -1
  f.flush();
  size_t real = (size_t)f.size();
  f.close();
  if (!ok || real != 4 + sizeof(PlayerData)) {
    Serial.println("[WORLD] 玩家存档写入不完整，保留旧档");
    SPIFFS.remove(tmp);
    return;
  }
  const bool had_old = SPIFFS.exists(path);
  if (had_old) {
    SPIFFS.remove(bak);
    if (!SPIFFS.rename(path, bak)) {          // 旧档先挪到 bak，不直接删
      Serial.println("[WORLD] 玩家存档旧档改名失败，保留 tmp");
      return;
    }
  }
  if (!SPIFFS.rename(tmp, path)) {
    Serial.println("[WORLD] 玩家存档 rename 失败，回滚旧档");
    if (had_old) SPIFFS.rename(bak, path);
    return;
  }
  if (had_old) SPIFFS.remove(bak);           // 新档就位后才删备份
}

void worldStoreSavePlayers() {
  if (!g_mounted) return;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd < 0) continue;      // 只存在线的，断线时单独存
    worldStoreSavePlayer(i);
  }
  g_last_player_save_ms = millis();
}

// ---- 药水效果映射（/qN.dat：'Q','T' + 9 字节，255=该格没药水）----
bool worldStoreLoadPotionMap(int idx, uint8_t *map9) {
  if (!map9) return false;
  memset(map9, 255, 9);
  if (!g_mounted || idx < 0 || idx >= MAX_PLAYERS) return false;
  char path[16], tmp[20];
  snprintf(path, sizeof(path), "/q%d.dat", idx);
  snprintf(tmp, sizeof(tmp), "/q%d.tmp", idx);
  const char *src = path;
  if (!SPIFFS.exists(path)) {
    if (SPIFFS.exists(tmp)) src = tmp;              // 写了一半掉电：tmp 也认
    else return false;
  }
  File f = SPIFFS.open(src, FILE_READ);
  if (!f || f.size() < 11) { if (f) f.close(); return false; }
  uint8_t head[2];
  f.read(head, 2);
  if (head[0] != 'Q' || head[1] != 'T') { f.close(); return false; }
  f.read(map9, 9);
  f.close();
  return true;
}

void worldStoreSavePotionMap(int idx, const uint8_t *map9) {
  if (!g_mounted || !map9 || idx < 0 || idx >= MAX_PLAYERS) return;
  char path[16], tmp[20];
  snprintf(path, sizeof(path), "/q%d.dat", idx);
  snprintf(tmp, sizeof(tmp), "/q%d.tmp", idx);
  File f = SPIFFS.open(tmp, FILE_WRITE);
  if (!f) return;
  uint8_t head[2] = {'Q', 'T'};
  bool ok = (f.write(head, 2) == 2) && (f.write(map9, 9) == 9);
  f.flush();
  size_t real = (size_t)f.size();
  f.close();
  if (!ok || real != 11) { SPIFFS.remove(tmp); return; }
  SPIFFS.remove(path);
  SPIFFS.rename(tmp, path);
}

void worldStoreLoadPlayers() {
  if (!g_mounted) return;
  int loaded = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    char path[16];
    char tmp[20];
    char bak[20];
    snprintf(path, sizeof(path), "/p%d.dat", i);
    snprintf(tmp, sizeof(tmp), "/p%d.tmp", i);
    snprintf(bak, sizeof(bak), "/p%d.bak", i);
    // 主档缺失时按 tmp（新档写好还没改名）→ bak（旧档已挪走还没删）的顺序兜底
    const char *src = path;
    if (!SPIFFS.exists(path)) {
      if (SPIFFS.exists(tmp)) src = tmp;
      else if (SPIFFS.exists(bak)) src = bak;
      else continue;
    }
    File f = SPIFFS.open(src, FILE_READ);
    if (!f || f.size() < (long)(4 + sizeof(PlayerData))) { if (f) f.close(); continue; }
    uint8_t head[4];
    f.read(head, 4);
    if (head[0] != 'P' || head[1] != 'D') { f.close(); continue; }
    f.read((uint8_t *)&player_data[i], sizeof(PlayerData));
    f.close();
    if (strcmp(src, tmp) != 0) SPIFFS.remove(tmp);
    if (strcmp(src, bak) != 0) SPIFFS.remove(bak);
    player_data[i].client_fd = -1;
    player_data[i].flags = 0;
    loaded++;
  }
  player_data_count = loaded;
  Serial.printf("[WORLD] 载入玩家存档 %d 个\n", loaded);
}

void worldStoreFlush() {
  if (!g_mounted) return;
  flushPending();
  flushStates();
}

size_t worldStoreChangeCount() { return block_changes_count; }

// 串口诊断：一眼看出改动是否真的在盘上
void worldStorePrintInfo() {
  if (!g_mounted) {
    sePrintln("[WORLD] SPIFFS 未挂载，改动不会保存");
    return;
  }
  long log_bytes = -1;
  if (SPIFFS.exists(kLogPath)) {
    File f = SPIFFS.open(kLogPath, FILE_READ);
    if (f) { log_bytes = (long)f.size(); f.close(); }
  }
  sePrintf("[WORLD] seed=%08X rng=%08X changes=%u log=%ld bytes pending=%u spiffs_used=%u/%u\n",
           (unsigned)world_seed, (unsigned)rng_seed, (unsigned)block_changes_count,
           log_bytes, (unsigned)g_pending_count,
           (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());
  // 读测试：确认这个 SPIFFS 上 size()/read() 到底能不能用
  File rf = SPIFFS.open(kLogPath, FILE_READ);
  if (rf) {
    uint8_t head[7] = {0};
    long pos0 = (long)rf.position();
    size_t got = rf.read(head, 7);
    sePrintf("[WORLD] readtest pos=%ld got=%u bytes=%02X %02X %02X %02X %02X %02X %02X\n",
             pos0, (unsigned)got, head[0], head[1], head[2], head[3], head[4], head[5], head[6]);
    rf.close();
  } else {
    sePrintln("[WORLD] readtest 打开失败");
  }
}

bool worldStoreReady() { return g_mounted; }

bool worldStoreReset() {
  if (!g_mounted) return false;
  flushPending();
  flushStates();
  SPIFFS.remove(kLogPath);
  SPIFFS.remove(kSeedPath);
  SPIFFS.remove(kStatePath);       // 楼梯/梯子 state 也要清，否则新世界里残留旧朝向
  // 玩家存档（坐标/血量/背包）和药水映射一起清，否则“重置世界”后还站在老地方、背着老背包
  for (int i = 0; i < MAX_PLAYERS; i++) {
    char p[16], t[16], b[16], q[16];
    snprintf(p, sizeof(p), "/p%d.dat", i);
    snprintf(t, sizeof(t), "/p%d.tmp", i);
    snprintf(b, sizeof(b), "/p%d.bak", i);
    snprintf(q, sizeof(q), "/q%d.dat", i);
    SPIFFS.remove(p);
    SPIFFS.remove(t);
    SPIFFS.remove(b);
    SPIFFS.remove(q);
  }
  Serial.println("[WORLD] 存档已清除，下次开机会生成新世界");
  return true;
}
