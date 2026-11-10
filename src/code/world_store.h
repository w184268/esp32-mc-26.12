// SPDX-License-Identifier: GPL-3.0-only
// 世界存档：种子 + 方块改动（SPIFFS 追加日志）

#ifndef WORLD_STORE_H
#define WORLD_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 挂载 SPIFFS 并尝试读取已保存的种子。必须在 mc_server.begin() 最前面调用。
void worldStoreBegin();

// 是否成功读到了上次保存的种子（false 表示需要现场生成一个新世界并保存）。
bool worldStoreSeedLoaded();

// 把当前的 world_seed / rng_seed 写入 Flash。
void worldStoreSaveSeed();

// 回放方块改动日志到 block_changes[]。必须在数组被清空(0xFF)之后调用。
void worldStoreReplayChanges();

// 记录一次方块改动（由 makeBlockChange 调用，内部缓冲，落盘由 tick 负责）。
void worldStoreNoteChange(int16_t x, uint8_t y, int16_t z, uint8_t block);

// 记录一次“方块状态”（楼梯朝向/半格、梯子朝向），单独写 /w.state，不动 /w.log 格式。
void worldStoreNoteState(int16_t x, uint8_t y, int16_t z, uint16_t state);

// 玩家存档（背包 / 坐标 / 血量 / 饥饿）
void worldStoreLoadPlayers();      // 开机载入到 player_data[]
void worldStoreSavePlayers();      // 保存在线玩家
void worldStoreSavePlayer(int idx);// 保存指定玩家（断线时用）
bool worldStoreHasPlayer(int idx); // 该槽是否有存档

// 药水效果映射（每个快捷栏格这瓶药水是什么效果，255=没有）
// 单独存 /qN.dat，不动 /pN.dat 的格式（旧档照样能读）。
bool worldStoreLoadPotionMap(int idx, uint8_t *map9);
void worldStoreSavePotionMap(int idx, const uint8_t *map9);

// 箱子内容存档（写进 /w.log，type=1）
void worldStoreSaveChest(int16_t x, uint8_t y, int16_t z);

// 主循环调用：定期把缓冲写入 Flash，并在日志过大时压缩。
void worldStoreTick();

// 立即刷新（断线/关机前调用）。
void worldStoreFlush();

// 当前内存里的方块改动条数。
size_t worldStoreChangeCount();

// 串口诊断：种子 / 改动条数 / 日志字节数 / SPIFFS 占用。
void worldStorePrintInfo();

// 是否可用（SPIFFS 挂载成功）。
bool worldStoreReady();

// 删除存档（换新世界用）。
bool worldStoreReset();

#endif
