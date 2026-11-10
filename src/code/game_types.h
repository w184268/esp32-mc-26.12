// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.

#ifndef GAME_TYPES_H
#define GAME_TYPES_H

#include <stdint.h>

// ============ 编译开关 ============

#define MAX_PLAYERS 5
#define MAX_MOBS 20
#define MOB_DESPAWN_DISTANCE 256
// 默认游戏模式：0 = 生存；1 = 创造。
// 玩家可以用 /gamemode 临时切换，死亡重生后回到这个默认值。
#define GAMEMODE 0
#define VIEW_DISTANCE 4   // 区块半径（1 区块 = 16 格）：客户端雾墙就卡在这个半径上。
                          // 原来是 1（只剩 16 格可见，外面全是雾），现在 4（9×9 = 81 区块，
                          // 可见约 64 格）。再大会明显拖长进服时的区块下发时间。
#define TIME_BETWEEN_TICKS 1000000
#define TICKS_PER_SECOND ((float)1000000 / TIME_BETWEEN_TICKS)
#define INITIAL_WORLD_SEED 0x6A0AEF04
#define INITIAL_RNG_SEED   0x350B10FB
#define CHUNK_SIZE 8
#define TERRAIN_BASE_HEIGHT 60
#define CAVE_BASE_DEPTH 24
#define BIOME_SIZE (CHUNK_SIZE * 8)
#define BIOME_RADIUS (BIOME_SIZE / 2)
#define VISITED_HISTORY 4
#define MAX_BLOCK_CHANGES 4700   // 4700 × 6B = 28.2KB（让出 1.8KB 给箱子内容独立表，DRAM 才装得下）
#define NETWORK_TIMEOUT_US 15000000
#define MAX_RECV_BUF_LEN 256

#define SEND_BRAND
#define BROADCAST_ALL_MOVEMENT
#define SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT
#define DO_FLUID_FLOW
#define ALLOW_CHESTS
// #define ENABLE_PLAYER_FLIGHT
#define ENABLE_PICKUP_ANIMATION
#define ENABLE_CACTUS_DAMAGE
// #define DEV_LOG_UNKNOWN_PACKETS
#define DEV_LOG_NETWORK_DIAGNOSTICS
#define DEV_LOG_LENGTH_DISCREPANCY
#define DEV_MINIMAL_PLAY_BOOTSTRAP

// ============ 协议状态 ============

#define STATE_NONE 0
#define STATE_STATUS 1
#define STATE_LOGIN 2
#define STATE_TRANSFER 3
#define STATE_CONFIGURATION 4
#define STATE_PLAY 5

// ============ 伤害类型 ============

#define D_generic 18
#define D_fall 10
#define D_lava 24
#define D_on_fire 31
#define D_cactus 2

// ============ Biome ID ============

#define W_plains 40
#define W_mangrove_swamp 31
#define W_desert 14
#define W_snowy_plains 46
#define W_beach 3

// ============ 数据结构 ============

struct BlockChange {
  int16_t x;
  int16_t z;
  uint8_t y;
  uint8_t block;
};

#pragma pack(push, 1)

struct PlayerData {
  uint8_t uuid[16];
  char name[16];
  int client_fd;  // 在新架构中用作 slot index
  int16_t x;
  uint8_t y;
  int16_t z;
  int16_t visited_x[VISITED_HISTORY];
  int16_t visited_z[VISITED_HISTORY];
#ifdef SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT
  uint16_t packets_since_update;
#endif
  int8_t yaw;
  int8_t pitch;
  uint8_t grounded_y;
  uint8_t health;
  uint8_t hunger;
  uint16_t saturation;
  uint8_t hotbar;
  uint8_t game_mode;   // 当前游戏模式：0 生存 / 1 创造（重生时恢复为 GAMEMODE）
  uint16_t inventory_items[41];
  uint16_t craft_items[9];
  uint8_t inventory_count[41];
  uint8_t craft_count[9];
  uint16_t flagval_16;
  uint8_t flagval_8;
  // 0x01 - 攻击冷却
  // 0x02 - 新玩家待生成
  // 0x04 - 潜行
  // 0x08 - 疾跑
  // 0x10 - 吃东西
  // 0x20 - 客户端加载中
  // 0x40 - 移动更新冷却
  // 0x80 - craft_items 已锁
  uint8_t flags;
};

// 附魔数据不再塞进 PlayerData：PlayerData 是定长落盘结构，加字段会让旧 /pN.dat 读不出来。
// 每个玩家的附魔信息单独存 /eN.dat（见 mc_server.cpp 的 enchantGet_ / applyEnchant_）。

struct MobData {  uint8_t type;
  int16_t x;
  uint8_t y;
  int16_t z;
  // 低 5 位: 血量, 中间 1 位: 羊剃毛, 高 2 位: 惊慌计时器
  uint8_t data;
};

#pragma pack(pop)

union EntityDataValue {
  uint8_t byte_val;
  int pose;
};

struct EntityData {
  uint8_t index;
  int type;  // 0 - Byte, 21 - Pose
  EntityDataValue value;
};

// ============ 世界生成结构 ============

struct ChunkAnchor {
  int16_t x;
  int16_t z;
  uint32_t hash;
  uint8_t biome;
};

struct ChunkFeature {
  int16_t x;
  uint8_t y;
  int16_t z;
  uint8_t variant;
};

#endif
