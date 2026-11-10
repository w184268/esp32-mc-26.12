// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.

#ifndef PROCEDURES_H
#define PROCEDURES_H

#include "game_types.h"
#include <stdint.h>

// 槽位同步回调 (由 mc_server.cpp 注册, 用于 givePlayerItem 实时更新客户端)
extern void (*g_sync_slot_cb)(int fd, int slot, uint8_t count, uint16_t item);
// slot_idx -> 真实 socket fd 的映射表
extern int g_slot_fd_map[MAX_PLAYERS];

// 方块改动查询/写入
uint8_t getBlockChange(int16_t x, uint8_t y, int16_t z);
uint8_t makeBlockChange(int16_t x, uint8_t y, int16_t z, uint8_t block);

// ---- 方块状态旁路（楼梯/梯子这类“同一方块多种朝向/半格”）----
// state id 取自 26.1.2 客户端 jar 的官方数据生成器导出（probe/gen1261/reports/blocks.json），
// 与本机 MC 26.1.2 完全一致；block_palette[] 里存的默认值仍是“朝北的下半格”那一个。
#define MAX_STATE_OVERRIDES 512
uint16_t blockStateExtraGet(int16_t x, uint8_t y, int16_t z);
void blockStateExtraSet(int16_t x, uint8_t y, int16_t z, uint16_t state);
void blockStateExtraClear(int16_t x, uint8_t y, int16_t z);
uint16_t netStateAt(int16_t x, uint8_t y, int16_t z, uint8_t block);
uint16_t stairsStateId(uint8_t block, uint8_t facing, uint8_t top_half, uint8_t waterlogged);
uint16_t ladderStateId(uint8_t facing, uint8_t waterlogged);
uint16_t furnaceStateId(uint8_t facing, uint8_t lit);

// 箱子内容（27 格 × 3 字节：item 低字节、item 高字节、count）
// 单独放在一张小表里，不再借用 block_changes[] 后面的保留槽——
// 借用槽位时，那些槽位要靠把 block 字节写成 0xFF 当"已占位"标记，
// 而 0xFF 正好落在 27 格数据的第 6 个字节上，使"第 2 格"变成 count=255、
// 客户端解码 container_set_slot 直接报错断线（玩家报的"箱子打不开"）。
#define MAX_CHEST_STORES 6
#define CHEST_ITEM_BYTES 81
struct ChestStore {
  bool used;
  int16_t x;
  uint8_t y;
  int16_t z;
  uint8_t items[CHEST_ITEM_BYTES];
};
extern ChestStore chest_stores[MAX_CHEST_STORES];
ChestStore *findChestStore(int16_t x, uint8_t y, int16_t z);
ChestStore *ensureChestStore(int16_t x, uint8_t y, int16_t z);
void removeChestStore(int16_t x, uint8_t y, int16_t z);

// 方块属性
uint8_t isInstantlyMined(PlayerData *player, uint8_t block);
uint8_t isColumnBlock(uint8_t block);
uint8_t isPassableBlock(uint8_t block);
uint8_t isPassableSpawnBlock(uint8_t block);
uint8_t isReplaceableBlock(uint8_t block);
uint8_t isCoverableBlock(uint8_t block);
// 不占满一格的方块（半砖/楼梯/栅栏/火把/梯子/花木…）：不能按覆盖方式放进被点中的那一格
uint8_t isPartialBlock(uint8_t block);
// 楼梯类方块：当前一律禁止放置（客户端只认一个 state，画不出正常台阶）
uint8_t isStairsBlock(uint8_t block);
// 全靠 block state 决定外形/朝向/连接/生长阶段的方块：服务端发不出 state，
// 放下去样子一定是错的，统一禁止放置并回聊天提示
uint8_t isUnsupportedBlock(uint8_t block);
uint32_t isCompostItem(uint16_t item);
uint8_t getItemStackSize(uint16_t item);

// 挖掘和工具
// 工具 id 段（912~946）每 5 个一组：sword/shovel/pickaxe/axe/hoe
//   912 木 / 917 铜 / 922 石 / 927 金 / 932 铁 / 937 钻石 / 942 下界合金
uint8_t getToolKind(uint16_t item);   // 0=非工具 1=剑 2=锹 3=镐 4=斧 5=锄
uint8_t getToolTier(uint16_t item);   // 0=非工具 1=木/铜/金 2=石 3=铁 5=钻石 6=下界合金
uint16_t getMiningResult(uint16_t held_item, uint8_t block);
void bumpToolDurability(PlayerData *player);

// 玩家管理
void resetPlayerData(PlayerData *player);
int reservePlayerData(int client_fd, uint8_t *uuid, char *name);
int getPlayerData(int client_fd, PlayerData **output);
PlayerData *getPlayerByName(int start_offset, int end_offset, uint8_t *buffer);
int givePlayerItem(PlayerData *player, uint16_t item, uint8_t count);
// 同上，但把“落到哪个背包格”通过 out_slot 带出来（255 = 没进背包）。
// no_merge=true 时不去合并已有堆叠——带附魔/带名字的物品不能和普通同类物品堆在一起。
int givePlayerItemAt(PlayerData *player, uint16_t item, uint8_t count, uint8_t *out_slot, bool no_merge = false);

// 流体
void checkFluidUpdate(int16_t x, uint8_t y, int16_t z, uint8_t block);

// Mob
void spawnMob(uint8_t type, int16_t x, uint8_t y, int16_t z, uint8_t health);

// 槽位映射
uint8_t serverSlotToClientSlot(int window_id, uint8_t slot);
uint8_t clientSlotToServerSlot(int window_id, uint8_t slot);

#endif
