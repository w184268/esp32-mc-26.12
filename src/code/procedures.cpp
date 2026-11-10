// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.

#include "procedures.h"
#include "game_state.h"
#include "registries.h"
#include "terrain.h"
#include "world_store.h"
#include <string.h>
#include <esp_heap_caps.h>

// 槽位同步回调 (由 mc_server.cpp 注册)
void (*g_sync_slot_cb)(int fd, int slot, uint8_t count, uint16_t item) = nullptr;
// slot_idx -> 真实 socket fd 的映射表 (由 mc_server.cpp 维护)
int g_slot_fd_map[MAX_PLAYERS] = {-1, -1, -1, -1, -1};

// ============ 箱子内容（独立小表） ============

ChestStore chest_stores[MAX_CHEST_STORES];

ChestStore *findChestStore(int16_t x, uint8_t y, int16_t z) {
  for (int i = 0; i < MAX_CHEST_STORES; i++) {
    if (!chest_stores[i].used) continue;
    if (chest_stores[i].x == x && chest_stores[i].y == y && chest_stores[i].z == z)
      return &chest_stores[i];
  }
  return nullptr;
}

ChestStore *ensureChestStore(int16_t x, uint8_t y, int16_t z) {
  ChestStore *cs = findChestStore(x, y, z);
  if (cs) return cs;
  for (int i = 0; i < MAX_CHEST_STORES; i++) {
    if (chest_stores[i].used) continue;
    chest_stores[i].used = true;
    chest_stores[i].x = x;
    chest_stores[i].y = y;
    chest_stores[i].z = z;
    memset(chest_stores[i].items, 0, CHEST_ITEM_BYTES);
    return &chest_stores[i];
  }
  return nullptr;   // 箱子太多（8 个）
}

void removeChestStore(int16_t x, uint8_t y, int16_t z) {
  ChestStore *cs = findChestStore(x, y, z);
  if (!cs) return;
  memset(cs, 0, sizeof(ChestStore));
}

// ============ 方块改动 ============

uint8_t getBlockChange(int16_t x, uint8_t y, int16_t z) {
  for (int i = 0; i < block_changes_count; i++) {
    if (block_changes[i].block == 0xFF) continue;
    if (block_changes[i].x == x && block_changes[i].y == y && block_changes[i].z == z)
      return block_changes[i].block;
  }
  return 0xFF;
}

uint8_t makeBlockChange(int16_t x, uint8_t y, int16_t z, uint8_t block) {
  // 先查找已有改动（同坐标直接改，不新开记录：早期箱子用"保留 14 格"的方案时，
  // 箱子头被跳过，于是同坐标又追加了一条记录，客户端显示成新方块、箱子再也打不开）
  for (int i = 0; i < block_changes_count; i++) {
    if (block_changes[i].block == 0xFF) continue;
    if (block_changes[i].x == x && block_changes[i].y == y && block_changes[i].z == z) {
      if (block_changes[i].block == B_chest && block != B_chest) {
        // 箱子被替换 / 挖掉：内容表一起释放
        removeChestStore(x, y, z);
      }
      block_changes[i].block = block;
      if (block == B_chest) ensureChestStore(x, y, z);
      if (!isStairsBlock(block) && block != B_ladder) blockStateExtraClear(x, y, z);
      worldStoreNoteChange(x, y, z, block);
      return 0;
    }
  }
  // 找空槽
  for (int i = 0; i < MAX_BLOCK_CHANGES; i++) {
    if (block_changes[i].block != 0xFF) continue;
    block_changes[i].x = x;
    block_changes[i].y = y;
    block_changes[i].z = z;
    block_changes[i].block = block;
    if (i >= block_changes_count) block_changes_count = i + 1;
    if (block == B_chest) ensureChestStore(x, y, z);
    if (!isStairsBlock(block) && block != B_ladder) blockStateExtraClear(x, y, z);
    worldStoreNoteChange(x, y, z, block);
    return 0;
  }
  return 1; // 满了
}

// ============ 方块状态旁路（楼梯/梯子） ============
// 记录放 PSRAM：DRAM 现在只剩几 KB 余量（block_changes 4700×6 已占满），
// 4KB 的静态数组会直接把 dram0_0_seg 撑爆。
struct StateOverride {
  int16_t x;
  int16_t z;
  uint8_t y;
  uint8_t used;
  uint16_t state;
};

static StateOverride *s_state_tbl = nullptr;
static int s_state_count = 0;
static int s_state_next = 0;

static void ensureStateTable_() {
  if (s_state_tbl) return;
  s_state_tbl = (StateOverride *)heap_caps_malloc(sizeof(StateOverride) * MAX_STATE_OVERRIDES,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_state_tbl) s_state_tbl = (StateOverride *)malloc(sizeof(StateOverride) * MAX_STATE_OVERRIDES);
  if (s_state_tbl) memset(s_state_tbl, 0, sizeof(StateOverride) * MAX_STATE_OVERRIDES);
}

uint16_t blockStateExtraGet(int16_t x, uint8_t y, int16_t z) {
  if (!s_state_tbl) return 0;
  for (int i = 0; i < s_state_count; i++) {
    const StateOverride *e = &s_state_tbl[i];
    if (e->used && e->x == x && e->y == y && e->z == z) return e->state;
  }
  return 0;
}

void blockStateExtraSet(int16_t x, uint8_t y, int16_t z, uint16_t state) {
  ensureStateTable_();
  if (!s_state_tbl) return;
  for (int i = 0; i < s_state_count; i++) {
    StateOverride *e = &s_state_tbl[i];
    if (e->used && e->x == x && e->y == y && e->z == z) { e->state = state; return; }
  }
  StateOverride *e;
  if (s_state_count < MAX_STATE_OVERRIDES) {
    e = &s_state_tbl[s_state_count++];
  } else {
    e = &s_state_tbl[s_state_next];
    s_state_next = (s_state_next + 1) % MAX_STATE_OVERRIDES;
  }
  e->x = x; e->y = y; e->z = z; e->state = state; e->used = 1;
}

void blockStateExtraClear(int16_t x, uint8_t y, int16_t z) {
  if (!s_state_tbl) return;
  for (int i = 0; i < s_state_count; i++) {
    StateOverride *e = &s_state_tbl[i];
    if (e->used && e->x == x && e->y == y && e->z == z) { e->used = 0; e->state = 0; return; }
  }
}

uint16_t netStateAt(int16_t x, uint8_t y, int16_t z, uint8_t block) {
  uint16_t extra = blockStateExtraGet(x, y, z);
  if (extra) return extra;
  return block_palette[block];
}

// 26.1.2 原版楼梯的 state 排布（8 种楼梯逐一核对，排布完全一致）：
//   id = base + facing*20 + (half==top?0:10) + (waterlogged?0:1)
//   facing 顺序 north/south/west/east，每个朝向 20 个；前 10 个是 top 半格，后 10 个是 bottom
//   shape 固定 straight（每 10 个的第 1 个就是 straight）
static uint16_t stairsBaseState_(uint8_t block) {
  switch (block) {
    case B_oak_stairs:            return 3907;
    case B_cobblestone_stairs:    return 5747;
    case B_stone_brick_stairs:    return 8758;
    case B_brick_stairs:          return 8678;
    case B_nether_brick_stairs:   return 9367;
    case B_sandstone_stairs:      return 9493;
    case B_mud_brick_stairs:      return 8838;
    case B_resin_brick_stairs:    return 8923;
    default:                      return 0;
  }
}

uint16_t stairsStateId(uint8_t block, uint8_t facing, uint8_t top_half, uint8_t waterlogged) {
  uint16_t base = stairsBaseState_(block);
  if (!base) return block_palette[block];
  if (facing > 3) facing = 0;
  return (uint16_t)(base + facing * 20 + (top_half ? 0 : 10) + (waterlogged ? 0 : 1));
}

uint16_t ladderStateId(uint8_t facing, uint8_t waterlogged) {
  // 梯子 5719~5726：id = 5719 + facing*2 + (waterlogged?0:1)
  if (facing > 3) facing = 0;
  return (uint16_t)(5719 + facing * 2 + (waterlogged ? 0 : 1));
}

uint16_t furnaceStateId(uint8_t facing, uint8_t lit) {
  // 熔炉 5327~5334（facing 顺序与梯子一致）：id = 5327 + facing*2 + (lit?0:1)
  // 客户端 jar 导出的 blocks.json 实证：
  //   5327 north/true  5328 north/false(默认)  5329 south/true  5330 south/false
  //   5331 west/true   5332 west/false         5333 east/true   5334 east/false
  if (facing > 3) facing = 0;
  return (uint16_t)(5327 + facing * 2 + (lit ? 0 : 1));
}

// ============ 方块属性 ============

uint8_t isPassableBlock(uint8_t block) {
  switch (block) {
    case B_air: case B_short_grass: case B_dead_bush: case B_torch:
    case B_oak_sapling: case B_dandelion: case B_poppy: case B_snow:
    case B_moss_carpet: case B_lily_pad: case B_ladder:   // 梯子可以穿过去/攀爬，不能当实心方块
    case B_red_mushroom: case B_brown_mushroom: case B_fern:  // 十字形植物无碰撞
      return 1;
    default:
      if (block >= B_water && block <= B_water_7) return 1;
      if (block >= B_lava && block <= B_lava_6) return 1;
      return 0;
  }
}

uint8_t isPassableSpawnBlock(uint8_t block) {
  if (isPassableBlock(block)) return 1;
  if (block == B_oak_leaves) return 1;
  return 0;
}

uint8_t isReplaceableBlock(uint8_t block) {
  switch (block) {
    case B_air: case B_short_grass: case B_dead_bush: case B_snow:
    case B_moss_carpet:
    case B_red_mushroom: case B_brown_mushroom: case B_fern:
      return 1;
    default:
      if (block >= B_water && block <= B_water_7) return 1;
      if (block >= B_lava && block <= B_lava_6) return 1;
      return 0;
  }
}

uint8_t isColumnBlock(uint8_t block) {
  return (block == B_cactus || block == B_sugar_cane);
}

// 右键"覆盖"目标：草方块和地表植物。对着它们右键，方块直接盖在被点中的那一格，
// 被盖住的方块直接消失、不产生掉落物（按玩家要求的行为）。
uint8_t isCoverableBlock(uint8_t block) {
  switch (block) {
    case B_grass_block: case B_snowy_grass_block:
    case B_short_grass: case B_fern: case B_bush:
    case B_dead_bush: case B_short_dry_grass: case B_tall_dry_grass:
    case B_dandelion: case B_golden_dandelion: case B_torchflower:
    case B_poppy: case B_blue_orchid: case B_allium: case B_azure_bluet:
    case B_red_tulip: case B_orange_tulip: case B_white_tulip: case B_pink_tulip:
    case B_oxeye_daisy: case B_cornflower: case B_wither_rose: case B_lily_of_the_valley:
    case B_red_mushroom: case B_brown_mushroom: case B_oak_sapling:
    case B_moss_carpet: case B_snow:
      return 1;
    default: return 0;
  }
}

// 「不占满一格」的方块：半砖(slab)、楼梯、栅栏、墙、栏杆/锁链、台阶? 、火把、
// 梯子、告示牌、门/活板门、按钮/压力板、花/植物/蘑菇、地毯、雪层、床、玻璃板…
// 这些东西不能按"覆盖"的方式放进被点中的那一格：那样会把地面方块顶掉
// （玩家看到的“半砖把地上的块顶替掉”），而且它们本身就只占半格高度。
// 返回 1 = 不是满格方块 → 走正常的按面偏移放置。
uint8_t isPartialBlock(uint8_t block) {
  if (block >= B_white_bed && block <= B_black_bed) return 1;                  // 81..96 床
  if (block >= B_iron_bars && block <= B_waxed_oxidized_copper_chain) return 1; // 191..208 栏杆/锁链
  if (block >= B_dandelion && block <= B_lily_of_the_valley) return 1;          // 123..137 花
  switch (block) {
    case B_moss_carpet: case B_oak_slab: case B_stone_slab: case B_cobblestone_slab:
    case B_composter: case B_snow:
    case B_dead_bush: case B_short_grass: case B_torch: case B_lily_pad: case B_oak_sapling:
    case B_powered_rail: case B_detector_rail: case B_rail: case B_cobweb:
    case B_fern: case B_bush: case B_short_dry_grass: case B_tall_dry_grass: case B_seagrass:
    case B_brown_mushroom: case B_red_mushroom:
    case B_oak_stairs: case B_cobblestone_stairs: case B_brick_stairs:
    case B_stone_brick_stairs: case B_mud_brick_stairs: case B_resin_brick_stairs:
    case B_nether_brick_stairs: case B_sandstone_stairs:
    case B_wheat: case B_sugar_cane: case B_nether_wart:
    case B_oak_sign: case B_oak_hanging_sign: case B_oak_door: case B_iron_door:
    case B_oak_trapdoor: case B_ladder: case B_lever:
    case B_stone_pressure_plate: case B_oak_pressure_plate:
    case B_light_weighted_pressure_plate: case B_heavy_weighted_pressure_plate:
    case B_stone_button: case B_oak_button:
    case B_redstone_torch: case B_soul_torch: case B_copper_torch:
    case B_oak_fence: case B_nether_brick_fence: case B_oak_fence_gate:
    case B_cobblestone_wall: case B_mossy_cobblestone_wall: case B_resin_brick_wall:
    case B_resin_brick_slab:
    case B_glass_pane: case B_vine: case B_glow_lichen: case B_resin_clump:
    case B_cake: case B_repeater:
    case B_brewing_stand: case B_cauldron: case B_end_portal_frame:
    case B_tripwire_hook: case B_flower_pot:
    case B_skeleton_skull: case B_wither_skeleton_skull:
    case B_dragon_egg:
      return 1;
    default: return 0;
  }
}

uint8_t isStairsBlock(uint8_t block) {
  switch (block) {
    case B_oak_stairs: case B_cobblestone_stairs: case B_stone_brick_stairs:
    case B_brick_stairs: case B_nether_brick_stairs: case B_sandstone_stairs:
    case B_mud_brick_stairs: case B_resin_brick_stairs:
      return 1;
    default: return 0;
  }
}

uint8_t isUnsupportedBlock(uint8_t block) {
  if (isStairsBlock(block)) return 0;                      // 楼梯：已按原版 state（朝向+半格）放置
  if (block >= B_white_bed && block <= B_black_bed) return 1;              // 床：头/脚+朝向写死，只会出现半张床
  if (block >= B_iron_bars && block <= B_waxed_oxidized_copper_chain) return 1;  // 铁栏杆/锁链：连接形状写死
  switch (block) {
    case B_oak_door: case B_iron_door:                              // 门：朝向+开关写死
    case B_oak_trapdoor:                                            // 活板门：朝向+开关写死
    case B_oak_sign: case B_oak_hanging_sign:                        // 告示牌：挂接面写死
    case B_oak_fence_gate:                                          // 栅栏门：朝向写死
    case B_oak_fence: case B_nether_brick_fence:                     // 栅栏：连接形状写死（永远是一根柱子）
    case B_cobblestone_wall: case B_mossy_cobblestone_wall: case B_resin_brick_wall:  // 墙：同上
    case B_glass_pane:                                               // 玻璃板：连接形状写死
    case B_lever:                                                    // 拉杆：朝向写死
    case B_oak_button: case B_stone_button:                          // 按钮：朝向写死
    case B_stone_pressure_plate: case B_oak_pressure_plate:           // 压力板：朝向/按下写死
    case B_light_weighted_pressure_plate: case B_heavy_weighted_pressure_plate:
    case B_rail: case B_powered_rail: case B_detector_rail:           // 铁轨：拐弯/坡度写死
    case B_repeater:                                                 // 中继器：朝向/延迟写死
    case B_wheat: case B_sugar_cane: case B_nether_wart:              // 作物：生长阶段写死
    case B_cake:                                                     // 蛋糕：咬数写死
    case B_cauldron:                                                 // 炼药锅：水位写死
    case B_brewing_stand:                                            // 酿造台：药水位写死
    case B_end_portal_frame:                                         // 末地传送门框架：朝向/有无眼睛写死
      return 1;
    default: return 0;
  }
}

uint8_t isInstantlyMined(PlayerData *player, uint8_t block) {
  (void)player;
  switch (block) {
    case B_short_grass: case B_dead_bush: case B_torch: case B_dandelion:
    case B_poppy: case B_oak_sapling: case B_snow: case B_moss_carpet:
    case B_lily_pad: case B_oak_leaves:
    case B_red_mushroom: case B_brown_mushroom: case B_fern:
      return 1;
    default: return 0;
  }
}

uint32_t isCompostItem(uint16_t item) {
  // 返回堆肥概率 (0 = 不可堆肥, 其他值越大概率越高)
  switch (item) {
    case I_oak_leaves: case I_oak_sapling: case I_short_grass:
      return 0x40000000;
    default: return 0;
  }
}

uint8_t getItemStackSize(uint16_t item) {
  // 武器/工具不可堆叠：912~946 一整段（木/铜/石/金/铁/钻石/下界合金 x sword..hoe）
  if (item >= I_wooden_sword && item <= I_netherite_hoe) return 1;
  // 四套护甲 955~974（皮革/铁/金/钻石，下界合金紧跟其后到 994 段另算），鞘翅单独一只
  if (item >= I_leather_helmet && item <= I_netherite_boots) return 1;
  if (item == I_elytra) return 1;
  if (item == I_enchanted_book) return 1;
  return 64;
}

// ============ 挖掘结果 ============

// 工具识别。以前这里用的是 "held_item >= I_wooden_pickaxe" 这种 id 区间比较，
// 会把剑/斧/锹/锄也算成镐（它们的 id 就夹在镐附近），判定完全是错的。
uint8_t getToolKind(uint16_t item) {
  if (item < I_wooden_sword || item > I_netherite_hoe) return 0;
  return (uint8_t)((item - I_wooden_sword) % 5) + 1;   // 1 剑 / 2 锹 / 3 镐 / 4 斧 / 5 锄
}

uint8_t getToolTier(uint16_t item) {
  if (item < I_wooden_sword || item > I_netherite_hoe) return 0;
  uint8_t mat = (uint8_t)((item - I_wooden_sword) / 5);   // 0 木 1 铜 2 石 3 金 4 铁 5 钻石 6 下界合金
  switch (mat) {
    case 0: case 1: case 3: return 1;   // 木/铜/金同一档（原版金镐也只能挖到石级）
    case 2: return 2;
    case 4: return 3;
    case 5: return 5;
    default: return 6;
  }
}

// 手里这把镐够不够挖这个方块
static bool pickEnough_(uint16_t held, uint8_t need_tier) {
  return getToolKind(held) == 3 && getToolTier(held) >= need_tier;
}

uint16_t getMiningResult(uint16_t held_item, uint8_t block) {
  switch (block) {
    // 石类：原版必须用镐（木镐起步），空手挖不下来
    case B_stone: return pickEnough_(held_item, 1) ? I_cobblestone : 0;
    case B_cobblestone: return pickEnough_(held_item, 1) ? I_cobblestone : 0;
    case B_stone_bricks: return pickEnough_(held_item, 1) ? B_to_I[B_stone_bricks] : 0;
    case B_dirt: return I_dirt;
    case B_grass_block: return I_dirt;
    case B_snowy_grass_block: return I_dirt;
    case B_sand: return I_sand;
    case B_sandstone: return I_sandstone;
    case B_oak_log: return I_oak_log;
    case B_oak_planks: return I_oak_planks;
    case B_oak_leaves: return (fast_rand() & 15) == 0 ? I_oak_sapling : 0;
    // 矿石按原版工具等级门槛：煤/铜=木，铁=石，金/红石/钻石=铁
    case B_coal_ore: return pickEnough_(held_item, 1) ? I_coal : 0;
    case B_copper_ore: return pickEnough_(held_item, 1) ? I_copper_ingot : 0;
    case B_iron_ore: return pickEnough_(held_item, 2) ? I_raw_iron : 0;
    case B_gold_ore: return pickEnough_(held_item, 3) ? I_raw_gold : 0;
    case B_redstone_ore: return pickEnough_(held_item, 3) ? I_redstone : 0;
    case B_diamond_ore: return pickEnough_(held_item, 3) ? I_diamond : 0;
    case B_obsidian: return pickEnough_(held_item, 5) ? I_obsidian : 0;
    case B_crafting_table: return I_crafting_table;
    case B_furnace: return I_furnace;
    case B_cactus: return I_cactus;
    case B_mud: return I_mud;
    case B_ice: return 0;
    case B_snow: return I_snowball;
    case B_snow_block: return I_snowball;
    case B_oak_sapling: return I_oak_sapling;
    case B_torch: return I_torch;
    case B_oak_wood: return I_oak_wood;
    default: break;
  }
  // 方块到物品映射
  if (block < 256 && B_to_I[block] != 0) return B_to_I[block];
  return 0;
}

void bumpToolDurability(PlayerData *player) {
  // 耐久这一版仍然不扣：玩家存档 PlayerData 是定长落盘结构，加字段会让旧 /pN.dat 读不出来，
  // 耐久值目前没有地方存。工具行为（能不能挖、挖什么、伤害多少）已经按材质分档了。
  (void)player;
}

// ============ 玩家管理 ============

void resetPlayerData(PlayerData *player) {
  player->game_mode = GAMEMODE;   // 重生回到默认模式（生存）
  player->health = 20;
  player->hunger = 20;
  player->saturation = 2500;
  player->x = 8;
  player->z = 8;
  player->y = 80;
  player->flags |= 0x02;
  player->grounded_y = 0;
  for (int i = 0; i < 41; i++) { player->inventory_items[i] = 0; player->inventory_count[i] = 0; }
  for (int i = 0; i < 9; i++) { player->craft_items[i] = 0; player->craft_count[i] = 0; }
  player->flags &= ~0x80;

  // 不再发任何出生装备：背包完全清空，物品靠自己挖/捡
}

int reservePlayerData(int client_fd, uint8_t *uuid, char *name) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (memcmp(player_data[i].uuid, uuid, 16) == 0) {
      player_data[i].client_fd = client_fd;
      memcpy(player_data[i].name, name, 16);
      player_data[i].flags |= 0x20;
      player_data[i].flagval_16 = 0;
      for (int j = 0; j < VISITED_HISTORY; j++) {
        player_data[i].visited_x[j] = 32767;
        player_data[i].visited_z[j] = 32767;
      }
      return 0;
    }
    uint8_t empty = 1;
    for (uint8_t j = 0; j < 16; j++) { if (player_data[i].uuid[j] != 0) { empty = 0; break; } }
    if (empty) {
      if (player_data_count >= MAX_PLAYERS) return 1;
      player_data[i].client_fd = client_fd;
      player_data[i].flags |= 0x20;
      player_data[i].flagval_16 = 0;
      memcpy(player_data[i].uuid, uuid, 16);
      memcpy(player_data[i].name, name, 16);
      resetPlayerData(&player_data[i]);
      for (int j = 0; j < VISITED_HISTORY; j++) {
        player_data[i].visited_x[j] = 32767;
        player_data[i].visited_z[j] = 32767;
      }
      player_data_count++;
      return 0;
    }
  }
  // 槽位被离线玩家占满时挤掉一个（断线时已落盘），避免存档后永久占坑
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd >= 0) continue;
    memset(&player_data[i], 0, sizeof(PlayerData));
    player_data[i].client_fd = client_fd;
    player_data[i].flags = 0x20;
    player_data[i].flagval_16 = 0;
    memcpy(player_data[i].uuid, uuid, 16);
    memcpy(player_data[i].name, name, 16);
    resetPlayerData(&player_data[i]);
    for (int j = 0; j < VISITED_HISTORY; j++) {
      player_data[i].visited_x[j] = 32767;
      player_data[i].visited_z[j] = 32767;
    }
    return 0;
  }
  return 1;
}

int getPlayerData(int client_fd, PlayerData **output) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == client_fd) {
      *output = &player_data[i];
      return 0;
    }
  }
  return 1;
}

PlayerData *getPlayerByName(int start_offset, int end_offset, uint8_t *buffer) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd == -1) continue;
    int j;
    for (j = start_offset; j < end_offset && j < 256 && buffer[j] != ' '; j++) {
      if (player_data[i].name[j - start_offset] != (char)buffer[j]) break;
    }
    if ((j == end_offset || buffer[j] == ' ') && j < 256) return &player_data[i];
  }
  return nullptr;
}

int givePlayerItem(PlayerData *player, uint16_t item, uint8_t count) {
  return givePlayerItemAt(player, item, count, nullptr, false);
}

int givePlayerItemAt(PlayerData *player, uint16_t item, uint8_t count, uint8_t *out_slot, bool no_merge) {
  if (out_slot) *out_slot = 255;
  if (item == 0 || count == 0) return 0;
  // 药水功能已下线：任何来源（掉落物、合成、指令）的药水都不进背包
  if (item == I_potion || item == I_splash_potion || item == I_lingering_potion) return 0;
  uint8_t slot = 255;
  uint8_t stack_size = getItemStackSize(item);

  if (!no_merge) {
    for (int i = 0; i < 41; i++) {
      if (player->inventory_items[i] == item && player->inventory_count[i] <= stack_size - count) {
        slot = i; break;
      }
    }
  }
  if (slot == 255) {
    for (int i = 0; i < 41; i++) {
      if (player->inventory_count[i] == 0) { slot = i; break; }
    }
  }
  if (slot >= 36) return 1;

  player->inventory_items[slot] = item;
  player->inventory_count[slot] += count;

  if (out_slot) *out_slot = slot;

  // 通过回调同步给客户端
  if (g_sync_slot_cb && player->client_fd >= 0)
    g_sync_slot_cb(player->client_fd, slot, player->inventory_count[slot], player->inventory_items[slot]);

  return 0;
}

// ============ 槽位映射 ============

uint8_t serverSlotToClientSlot(int window_id, uint8_t slot) {
  if (window_id == 0) {
    if (slot < 9) return slot + 36;
    if (slot >= 9 && slot <= 35) return slot;
    if (slot == 40) return 45;
    if (slot >= 36 && slot <= 39) return 44 - slot;
    if (slot >= 41 && slot <= 44) return slot - 40;
  } else if (window_id == 12) {
    if (slot >= 41 && slot <= 49) return slot - 40;
    return serverSlotToClientSlot(0, slot - 1);
  } else if (window_id == 14) {
    if (slot >= 41 && slot <= 43) return slot - 41;
    return serverSlotToClientSlot(0, slot + 6);
  }
  return 255;
}

uint8_t clientSlotToServerSlot(int window_id, uint8_t slot) {
  if (window_id == 0) {
    if (slot >= 36 && slot <= 44) return slot - 36;
    if (slot >= 9 && slot <= 35) return slot;
    if (slot == 45) return 40;
    if (slot >= 5 && slot <= 8) return 44 - slot;
    if (slot == 1) return 41;
    if (slot == 2) return 42;
    if (slot == 3) return 44;
    if (slot == 4) return 45;
  } else if (window_id == 12) {
    if (slot >= 1 && slot <= 9) return 40 + slot;
    if (slot >= 10 && slot <= 45) return clientSlotToServerSlot(0, slot - 1);
  } else if (window_id == 14) {
    if (slot <= 2) return 41 + slot;
    if (slot >= 3 && slot <= 38) return clientSlotToServerSlot(0, slot + 6);
  } else if (window_id == 13) {          // 附魔台：0 物品 1 青金石
    if (slot <= 1) return 41 + slot;
    if (slot >= 2 && slot <= 37) return clientSlotToServerSlot(0, slot + 7);
  } else if (window_id == 8) {           // 铁砧：0 输入 1 附加 2 结果
    if (slot <= 2) return 41 + slot;
    if (slot >= 3 && slot <= 38) return clientSlotToServerSlot(0, slot + 6);
  } else if (window_id == 21) {          // 锻造台：0 模板 1 底材 2 附加 3 结果
    if (slot <= 3) return 41 + slot;
    if (slot >= 4 && slot <= 39) return clientSlotToServerSlot(0, slot + 5);
  }
#ifdef ALLOW_CHESTS
  if (window_id == 2) {
    if (slot <= 26) return 41 + slot;
    if (slot >= 27 && slot <= 62) return clientSlotToServerSlot(0, slot - 18);
  }
#endif
  return 255;
}

// ============ 流体 ============

static uint8_t isReplaceableFluid(uint8_t adjacent, uint8_t level, uint8_t fluid) {
  if (isReplaceableBlock(adjacent) && adjacent != fluid) return 1;
  if (adjacent >= fluid && adjacent < fluid + 8 && (adjacent - fluid) > level + 1) return 1;
  return 0;
}

static void handleFluidMovement(int16_t x, uint8_t y, int16_t z, uint8_t fluid, uint8_t block) {
  uint8_t level = block - fluid;
  uint8_t adjacent[4] = {
    getBlockAt(x + 1, y, z), getBlockAt(x - 1, y, z),
    getBlockAt(x, y, z + 1), getBlockAt(x, y, z - 1)
  };

  if (level != 0) {
    uint8_t connected = 0;
    for (int i = 0; i < 4; i++) { if (adjacent[i] == block - 1) { connected = 1; break; } }
    if (!connected) {
      makeBlockChange(x, y, z, B_air);
      checkFluidUpdate(x + 1, y, z, adjacent[0]);
      checkFluidUpdate(x - 1, y, z, adjacent[1]);
      checkFluidUpdate(x, y, z + 1, adjacent[2]);
      checkFluidUpdate(x, y, z - 1, adjacent[3]);
      return;
    }
  }

  uint8_t block_below = getBlockAt(x, y - 1, z);
  if (isReplaceableBlock(block_below)) {
    makeBlockChange(x, y - 1, z, fluid);
    handleFluidMovement(x, y - 1, z, fluid, fluid);
    return;
  }

  if (level == 3 && fluid == B_lava) return;
  if (level == 7) return;

  if (isReplaceableFluid(adjacent[0], level, fluid)) { makeBlockChange(x + 1, y, z, block + 1); handleFluidMovement(x + 1, y, z, fluid, block + 1); }
  if (isReplaceableFluid(adjacent[1], level, fluid)) { makeBlockChange(x - 1, y, z, block + 1); handleFluidMovement(x - 1, y, z, fluid, block + 1); }
  if (isReplaceableFluid(adjacent[2], level, fluid)) { makeBlockChange(x, y, z + 1, block + 1); handleFluidMovement(x, y, z + 1, fluid, block + 1); }
  if (isReplaceableFluid(adjacent[3], level, fluid)) { makeBlockChange(x, y, z - 1, block + 1); handleFluidMovement(x, y, z - 1, fluid, block + 1); }
}

void checkFluidUpdate(int16_t x, uint8_t y, int16_t z, uint8_t block) {
  uint8_t fluid;
  if (block >= B_water && block < B_water + 8) fluid = B_water;
  else if (block >= B_lava && block < B_lava + 4) fluid = B_lava;
  else return;
  handleFluidMovement(x, y, z, fluid, block);
}

// ============ Mob ============

void spawnMob(uint8_t type, int16_t x, uint8_t y, int16_t z, uint8_t health) {
  for (int i = 0; i < MAX_MOBS; i++) {
    if (mob_data[i].type != 0) continue;
    mob_data[i].type = type;
    mob_data[i].x = x;
    mob_data[i].y = y;
    mob_data[i].z = z;
    mob_data[i].data = health & 31;
    break;
  }
}
