// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.

#ifndef GAME_STATE_H
#define GAME_STATE_H

#include "game_types.h"

// ============ 全局游戏状态 ============

extern uint32_t world_seed;
extern uint32_t rng_seed;
extern uint16_t world_time;
extern uint32_t server_ticks;
extern uint16_t client_count;

extern BlockChange block_changes[];
extern int block_changes_count;

extern PlayerData player_data[];
extern int player_data_count;

extern MobData mob_data[];

// ============ 工具函数 ============

static inline int mod_abs(int a, int b) {
  return ((a % b) + b) % b;
}

static inline int div_floor(int a, int b) {
  return a % b < 0 ? (a - b) / b : a / b;
}

uint32_t fast_rand();
uint64_t splitmix64(uint64_t state);

#endif
