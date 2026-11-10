// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.

#include "game_state.h"

uint32_t world_seed = INITIAL_WORLD_SEED;
uint32_t rng_seed = INITIAL_RNG_SEED;
uint16_t world_time = 0;
uint32_t server_ticks = 0;
uint16_t client_count = 0;

BlockChange block_changes[MAX_BLOCK_CHANGES];
int block_changes_count = 0;

PlayerData player_data[MAX_PLAYERS];
int player_data_count = 0;

MobData mob_data[MAX_MOBS];

uint32_t fast_rand() {
  rng_seed ^= rng_seed << 13;
  rng_seed ^= rng_seed >> 17;
  rng_seed ^= rng_seed << 5;
  return rng_seed;
}

uint64_t splitmix64(uint64_t state) {
  uint64_t z = state + 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}
