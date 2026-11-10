// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.

#include "crafting.h"
#include "game_state.h"
#include "registries.h"
#include <string.h>

// ============================================================ 合成表
// 形状配方：need[] 是 3×3 的行优先图案，0 表示该格必须为空。
// w/h 是配方实际占用的宽高，匹配时先算合成格已用区域的包围盒，
// 只有包围盒与配方尺寸一致才用得上（和原版"配方不能被空隙打断"一致）。
struct CraftRecipe {
  uint8_t w, h;
  uint16_t need[9];
  uint16_t out;
  uint8_t count;
};

#define S_ I_stick

#define TOOL_SET(mat, pick, axe, shovel, sword) \
  {3, 3, {mat, mat, mat, 0, S_, 0, 0, S_, 0}, pick, 1}, \
  {2, 3, {mat, mat, 0, mat, S_, 0, 0, S_, 0}, axe, 1}, \
  {1, 3, {mat, 0, 0, S_, S_, 0, 0, 0, 0}, shovel, 1}, \
  {1, 3, {mat, 0, 0, mat, 0, 0, S_, 0, 0}, sword, 1}

#define BLOCK_3x3(mat, blk) {3, 3, {mat, mat, mat, mat, mat, mat, mat, mat, mat}, blk, 1}

static const CraftRecipe kRecipes[] = {
  // 木头与基础件
  {1, 1, {I_oak_log, 0, 0, 0, 0, 0, 0, 0, 0}, I_oak_planks, 4},
  {2, 2, {I_oak_log, I_oak_log, 0, I_oak_log, I_oak_log, 0, 0, 0, 0}, I_oak_wood, 3},
  {1, 2, {I_oak_planks, 0, 0, I_oak_planks, 0, 0, 0, 0, 0}, I_stick, 4},
  {2, 2, {I_oak_planks, I_oak_planks, 0, I_oak_planks, I_oak_planks, 0, 0, 0, 0}, I_crafting_table, 1},
  {3, 3, {I_oak_planks, I_oak_planks, I_oak_planks,
          I_oak_planks, 0, I_oak_planks,
          I_oak_planks, I_oak_planks, I_oak_planks}, I_chest, 1},
  {3, 3, {I_cobblestone, I_cobblestone, I_cobblestone,
          I_cobblestone, 0, I_cobblestone,
          I_cobblestone, I_cobblestone, I_cobblestone}, I_furnace, 1},
  {1, 2, {I_coal, 0, 0, I_stick, 0, 0, 0, 0, 0}, I_torch, 4},
  {1, 2, {I_charcoal, 0, 0, I_stick, 0, 0, 0, 0, 0}, I_torch, 4},
  {3, 2, {I_oak_planks, 0, I_oak_planks, 0, I_oak_planks, 0, 0, 0, 0}, I_bowl, 4},
  {3, 1, {I_wheat, I_wheat, I_wheat, 0, 0, 0, 0, 0, 0}, I_bread, 1},

  // 工具（木/石/铁/金/钻）
  TOOL_SET(I_oak_planks, I_wooden_pickaxe, I_wooden_axe, I_wooden_shovel, I_wooden_sword),
  TOOL_SET(I_cobblestone, I_stone_pickaxe, I_stone_axe, I_stone_shovel, I_stone_sword),
  TOOL_SET(I_iron_ingot, I_iron_pickaxe, I_iron_axe, I_iron_shovel, I_iron_sword),
  TOOL_SET(I_gold_ingot, I_golden_pickaxe, I_golden_axe, I_golden_shovel, I_golden_sword),
  TOOL_SET(I_diamond, I_diamond_pickaxe, I_diamond_axe, I_diamond_shovel, I_diamond_sword),

  // 铁盔甲（只做铁，金/钻同形状不另列，避免配方表过大）
  {3, 2, {I_iron_ingot, I_iron_ingot, I_iron_ingot, I_iron_ingot, 0, I_iron_ingot, 0, 0, 0}, I_iron_helmet, 1},
  {3, 3, {I_iron_ingot, 0, I_iron_ingot,
          I_iron_ingot, I_iron_ingot, I_iron_ingot,
          I_iron_ingot, I_iron_ingot, I_iron_ingot}, I_iron_chestplate, 1},
  {3, 3, {I_iron_ingot, I_iron_ingot, I_iron_ingot,
          I_iron_ingot, 0, I_iron_ingot,
          I_iron_ingot, 0, I_iron_ingot}, I_iron_leggings, 1},
  {3, 2, {I_iron_ingot, 0, I_iron_ingot, I_iron_ingot, 0, I_iron_ingot, 0, 0, 0}, I_iron_boots, 1},
  {3, 2, {I_iron_ingot, 0, I_iron_ingot, 0, I_iron_ingot, 0, 0, 0, 0}, I_bucket, 1},

  // 台阶（3 个平铺）
  {3, 1, {I_oak_planks, I_oak_planks, I_oak_planks, 0, 0, 0, 0, 0, 0}, I_oak_slab, 6},
  {3, 1, {I_cobblestone, I_cobblestone, I_cobblestone, 0, 0, 0, 0, 0, 0}, I_cobblestone_slab, 6},
  {3, 1, {I_stone, I_stone, I_stone, 0, 0, 0, 0, 0, 0}, I_stone_slab, 6},
  {2, 2, {I_stone, I_stone, 0, I_stone, I_stone, 0, 0, 0, 0}, I_stone_bricks, 4},
  {2, 2, {I_snowball, I_snowball, 0, I_snowball, I_snowball, 0, 0, 0, 0}, I_snow_block, 1},

  // 压缩方块
  BLOCK_3x3(I_iron_ingot, I_iron_block),
  BLOCK_3x3(I_gold_ingot, I_gold_block),
  BLOCK_3x3(I_diamond, I_diamond_block),
  BLOCK_3x3(I_redstone, I_redstone_block),
  BLOCK_3x3(I_coal, I_coal_block),
  BLOCK_3x3(I_copper_ingot, I_copper_block),

  // 拆方块回材料
  {1, 1, {I_iron_block, 0, 0, 0, 0, 0, 0, 0, 0}, I_iron_ingot, 9},
  {1, 1, {I_gold_block, 0, 0, 0, 0, 0, 0, 0, 0}, I_gold_ingot, 9},
  {1, 1, {I_diamond_block, 0, 0, 0, 0, 0, 0, 0, 0}, I_diamond, 9},
  {1, 1, {I_redstone_block, 0, 0, 0, 0, 0, 0, 0, 0}, I_redstone, 9},
  {1, 1, {I_coal_block, 0, 0, 0, 0, 0, 0, 0, 0}, I_coal, 9},
  {1, 1, {I_copper_block, 0, 0, 0, 0, 0, 0, 0, 0}, I_copper_ingot, 9},
};

#define RECIPE_COUNT ((int)(sizeof(kRecipes) / sizeof(kRecipes[0])))

int craftRecipeCount(void) { return RECIPE_COUNT; }

bool craftRecipeInfo(int i, uint8_t *w, uint8_t *h, uint16_t need9[9], uint16_t *out, uint8_t *count) {
  if (i < 0 || i >= RECIPE_COUNT) return false;
  const CraftRecipe& r = kRecipes[i];
  if (w) *w = r.w;
  if (h) *h = r.h;
  if (need9) memcpy(need9, r.need, sizeof(r.need));
  if (out) *out = r.out;
  if (count) *count = r.count;
  return true;
}

// 单条配方在 (r0,c0) 处是否吻合；mirror = 1 时按左右镜像比对
static bool recipeMatches(const CraftRecipe& r, const uint16_t *grid, uint8_t r0, uint8_t c0, bool mirror) {
  for (uint8_t dr = 0; dr < r.h; dr++) {
    for (uint8_t dc = 0; dc < r.w; dc++) {
      uint8_t src = mirror ? (uint8_t)(r.w - 1 - dc) : dc;
      uint16_t want = r.need[dr * 3 + src];
      uint16_t have = grid[(r0 + dr) * 3 + (c0 + dc)];
      if (want == 0) { if (have != 0) return false; }
      else if (have != want) return false;
    }
  }
  return true;
}

void getCraftingOutput(PlayerData *player, uint8_t max_w, uint8_t max_h,
                       uint8_t *count, uint16_t *item) {
  *count = 0; *item = 0;
  if (player == nullptr) return;

  const uint16_t *grid = player->craft_items;

  // 找已填格子的包围盒（只看 max_w/max_h 允许的范围）
  int min_r = 3, max_r = -1, min_c = 3, max_c = -1, filled = 0;
  for (uint8_t r = 0; r < max_h && r < 3; r++) {
    for (uint8_t c = 0; c < max_w && c < 3; c++) {
      if (grid[r * 3 + c]) {
        filled++;
        if ((int)r < min_r) min_r = r;
        if ((int)r > max_r) max_r = r;
        if ((int)c < min_c) min_c = c;
        if ((int)c > max_c) max_c = c;
      }
    }
  }
  if (filled == 0) return;
  uint8_t bw = (uint8_t)(max_c - min_c + 1);
  uint8_t bh = (uint8_t)(max_r - min_r + 1);

  for (int i = 0; i < RECIPE_COUNT; i++) {
    const CraftRecipe& r = kRecipes[i];
    if (r.w != bw || r.h != bh) continue;
    if (r.w > max_w || r.h > max_h) continue;
    if (recipeMatches(r, grid, (uint8_t)min_r, (uint8_t)min_c, false) ||
        recipeMatches(r, grid, (uint8_t)min_r, (uint8_t)min_c, true)) {
      *item = r.out;
      *count = r.count;
      return;
    }
  }
}

// ============================================================ 熔炉
// 槽位：craft_items[0] = 原料，[1] = 燃料，[2] = 产物（点击结算后清空）
static uint8_t fuelUnits(uint16_t item) {
  switch (item) {
    case I_coal: case I_charcoal: return 8;
    case I_coal_block: return 80;
    case I_oak_log: case I_oak_wood: return 2;
    case I_oak_planks: case I_crafting_table: case I_chest: return 2;
    case I_stick: case I_oak_sapling: return 1;
    default: return 0;
  }
}

static uint16_t smeltOutput(uint16_t in) {
  switch (in) {
    case I_cobblestone: return I_stone;
    case I_sand: return I_glass;
    case I_raw_iron: return I_iron_ingot;
    case I_raw_gold: return I_gold_ingot;
    case I_raw_copper: return I_copper_ingot;
    case I_oak_log: case I_oak_wood: return I_charcoal;
    case I_beef: return I_cooked_beef;
    case I_porkchop: return I_cooked_porkchop;
    case I_chicken: return I_cooked_chicken;
    case I_mutton: return I_cooked_mutton;
    case I_clay_ball: return I_brick;
    default: return 0;
  }
}

// 算“这一次最多能烧出多少”：受原料数量与燃料总热量共同限制，产物不超过 64。
// 注意：这里**不看**产物槽里现有的东西。产物槽里放的是服务端自己算的“预览”，
// 以前把预览当成真实堆叠去夹（existing_count >= 64 就 return 0），
// 导致取产物时永远算出 0 件：原料一件没扣、产物槽又被重算回 64 → 可以无限取。
static uint8_t furnaceCapacity(const PlayerData *player, uint16_t *out_item) {
  uint16_t in = player->craft_items[0];
  uint16_t fu = player->craft_items[1];
  uint8_t in_count = player->craft_count[0];
  uint8_t fu_count = player->craft_count[1];
  if (in == 0 || fu == 0 || in_count == 0 || fu_count == 0) return 0;

  uint16_t out = smeltOutput(in);
  if (out == 0) return 0;

  uint8_t units_per_fuel = fuelUnits(fu);
  if (units_per_fuel == 0) return 0;

  uint32_t units = (uint32_t)fu_count * units_per_fuel;
  uint32_t n = units < in_count ? units : in_count;
  if (n == 0) return 0;
  if (n > 64) n = 64;
  *out_item = out;
  return (uint8_t)n;
}

void furnacePreview(PlayerData *player, uint8_t *count, uint16_t *item) {
  *count = 0; *item = 0;
  uint16_t out = 0;
  uint8_t n = furnaceCapacity(player, &out);
  if (n == 0) return;
  *item = out;
  *count = n;
}

// 取走产物：want = 客户端这一次真正拿走的件数（它看到产物槽里有多少就拿多少，
// 手上空间不够就只拿一部分）。按实际拿走的件数扣原料和燃料，不多扣也不凭空生成。
uint8_t furnaceTake(PlayerData *player, uint8_t want, uint16_t *out_item) {
  *out_item = 0;
  uint16_t out = player->craft_items[2];
  if (out == 0 || want == 0) return 0;

  uint16_t cap_out = 0;
  uint8_t cap = furnaceCapacity(player, &cap_out);
  if (cap == 0 || cap_out != out) return 0;      // 原料/燃料已经不足以支撑这份产物
  if (want > cap) want = cap;

  uint8_t units_per_fuel = fuelUnits(player->craft_items[1]);
  if (units_per_fuel == 0) return 0;
  uint8_t fuel_need = (uint8_t)((want + units_per_fuel - 1) / units_per_fuel);
  if (fuel_need > player->craft_count[1]) fuel_need = player->craft_count[1];

  player->craft_count[0] = (uint8_t)(player->craft_count[0] - want);
  if (player->craft_count[0] == 0) player->craft_items[0] = 0;
  player->craft_count[1] = (uint8_t)(player->craft_count[1] - fuel_need);
  if (player->craft_count[1] == 0) player->craft_items[1] = 0;

  *out_item = out;
  return want;
}
