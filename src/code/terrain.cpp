// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.

#include "terrain.h"
#include "game_state.h"
#include "registries.h"
#include "procedures.h"
#include <string.h>
#include <stdlib.h>

uint32_t getChunkHash(int16_t x, int16_t z) {
  uint8_t buf[8];
  memcpy(buf, &x, 2);
  memcpy(buf + 2, &z, 2);
  memcpy(buf + 4, &world_seed, 4);
  return (uint32_t)splitmix64(*((uint64_t *)buf));
}

uint8_t getChunkBiome(int16_t x, int16_t z) {
  x += BIOME_RADIUS;
  z += BIOME_RADIUS;

  int8_t dx = BIOME_RADIUS - mod_abs(x, BIOME_SIZE);
  int8_t dz = BIOME_RADIUS - mod_abs(z, BIOME_SIZE);
  if (dx * dx + dz * dz > BIOME_RADIUS * BIOME_RADIUS) return W_beach;

  int16_t biome_x = div_floor(x, BIOME_SIZE);
  int16_t biome_z = div_floor(z, BIOME_SIZE);

  uint8_t index = abs((biome_x & 3) + ((biome_z * 4) & 15));
  static const uint8_t biome_lookup[4] = {
    W_plains, W_mangrove_swamp, W_desert, W_snowy_plains
  };
  return biome_lookup[(world_seed >> (index * 2)) & 3];
}

static uint8_t getCornerHeight(uint32_t hash, uint8_t biome) {
  uint8_t height = TERRAIN_BASE_HEIGHT;

  switch (biome) {
    case W_mangrove_swamp:
      height += (hash % 3) + ((hash >> 4) % 3) + ((hash >> 8) % 3) + ((hash >> 12) % 3);
      if (height < 64) height -= (hash >> 24) & 3;
      break;
    case W_plains:
      height += (hash & 3) + ((hash >> 4) & 3) + ((hash >> 8) & 3) + ((hash >> 12) & 3);
      // 山地变体：约 1/3 的角点抬高，插值后形成连绵山脊
      if ((hash % 97) < 33) {
        height += 6 + ((hash >> 20) & 7);
      }
      break;
    case W_desert:
      height += 4 + (hash & 3) + ((hash >> 4) & 3);
      break;
    case W_beach:
      height = 62 - ((hash & 3) + ((hash >> 4) & 3) + ((hash >> 8) & 3));
      break;
    case W_snowy_plains:
      height += (hash & 7) + ((hash >> 4) & 7);
      break;
    default: break;
  }
  // 世界可视高度上限 80（getTerrainAt 中 y > 80 一律为空气），山脊封顶避免顶部被截平
  if (height > 78) height = 78;
  return height;
}

static uint8_t interpolate(uint8_t a, uint8_t b, uint8_t c, uint8_t d, int x, int z) {
  uint16_t top    = a * (CHUNK_SIZE - x) + b * x;
  uint16_t bottom = c * (CHUNK_SIZE - x) + d * x;
  return (top * (CHUNK_SIZE - z) + bottom * z) / (CHUNK_SIZE * CHUNK_SIZE);
}

static uint8_t getPlainsDecoration(ChunkAnchor anchor, int rx, int rz, uint8_t height) {
  if (height < 64) return 0;
  uint8_t deco = (uint8_t)(
    ((anchor.hash >> ((rx + rz) & 15)) & 0xFF) ^
    ((rx & 15) << 4) ^ (rz & 15) ^ height
  );
  if ((deco & 31) == 0) return B_dandelion;
  if ((deco & 31) == 1) return B_poppy;
  if ((deco & 3) == 0) return B_short_grass;
  return 0;
}

// ============================================================
// 扩建：结构物 / 新地形元素 / 出生点
// 全部由 chunk hash 推导，不引入全局状态，任意坐标可重复求值
// ============================================================

#define STRUCT_NONE        0
#define STRUCT_HOUSE       1
#define STRUCT_VILLAGE     2
#define STRUCT_RUIN        3
#define STRUCT_IGLOO       4
#define STRUCT_DESERT_HUT  5
#define STRUCT_SHACK       6

struct StructureSpec {
  uint8_t type;    // STRUCT_*
  uint8_t ox, oz;  // 区域内偏移
  uint8_t size;    // 足迹边长（5 或 6）
  uint8_t base_y;  // 地面层
};

// 结构足迹四角的最大高度 + 1：只按真正占用的地皮算，坡地上不容易被判定为"落差过大"
static uint8_t structBaseHeight(ChunkAnchor anchor, uint8_t ox, uint8_t oz, uint8_t size) {
  int x0 = anchor.x * CHUNK_SIZE + ox;
  int z0 = anchor.z * CHUNK_SIZE + oz;
  int x1 = x0 + size - 1;
  int z1 = z0 + size - 1;

  uint8_t m = getHeightAt(x0, z0);
  uint8_t v = getHeightAt(x1, z0); if (v > m) m = v;
  v = getHeightAt(x0, z1); if (v > m) m = v;
  v = getHeightAt(x1, z1); if (v > m) m = v;
  return m + 1;
}

static StructureSpec getStructureSpec(ChunkAnchor anchor) {
  StructureSpec s = { STRUCT_NONE, 0, 0, 0, 0 };

  // 约 1/40 的区块生成一座结构
  if ((anchor.hash % 20) != 0) return s;
  // 海边/水下的区域不放
  if (anchor.biome == W_beach) return s;

  switch (anchor.biome) {
    case W_plains:
      s.type = ((anchor.hash >> 6) % 3 == 0) ? STRUCT_VILLAGE : STRUCT_HOUSE;
      break;
    case W_desert:        s.type = STRUCT_DESERT_HUT; break;
    case W_snowy_plains:  s.type = STRUCT_IGLOO;      break;
    case W_mangrove_swamp:s.type = STRUCT_SHACK;      break;
    default: return s;
  }

  s.size = (s.type == STRUCT_VILLAGE) ? 6 : 5;
  uint8_t span = CHUNK_SIZE - s.size + 1;       // 3 或 4
  s.ox = (uint8_t)(((anchor.hash >> 8) & 0xFF) % span);
  s.oz = (uint8_t)(((anchor.hash >> 16) & 0xFF) % span);
  s.base_y = structBaseHeight(anchor, s.ox, s.oz, s.size);
  return s;
}

// 单块缓存：同一区块内反复求值时避免重复计算地基高度（结果与无缓存完全一致）
static StructureSpec getStructureSpecCached(ChunkAnchor anchor) {
  static uint32_t cached_hash = 0xFFFFFFFFu;
  static StructureSpec cached_spec;
  if (cached_hash == anchor.hash) return cached_spec;
  cached_spec = getStructureSpec(anchor);
  cached_hash = anchor.hash;
  return cached_spec;
}

// 返回 0xFF 表示"该列不由结构接管"
static uint8_t structureBlockAt(const StructureSpec &s, uint8_t rx, uint8_t rz, int y,
                                uint8_t terrain_height, uint32_t hash) {
  if (s.type == STRUCT_NONE) return 0xFF;
  if (terrain_height < 63) return 0xFF;

  int lx = (int)rx - (int)s.ox;
  int lz = (int)rz - (int)s.oz;
  if (lx < 0 || lz < 0 || lx >= (int)s.size || lz >= (int)s.size) return 0xFF;

  // 地形起伏过大时放弃，避免长出巨大石柱
  // 坡地允许最高 8 格落差，落差由下方地基自动填平，避免结构整座消失
  if ((int)s.base_y - (int)terrain_height > 8 || (int)terrain_height > (int)s.base_y) return 0xFF;

  int base = (int)s.base_y;

  // 地基：填平地面到地板之间
  if (y < base && y > (int)terrain_height) return B_cobblestone;
  if (y < base) return 0xFF;          // 地下交给自然地形

  int ly = y - base;                  // 0 = 地板层

  if (s.type == STRUCT_VILLAGE) {
    // 6x6 内两座 3x3 小屋：A 在 (0..2,0..2)，B 在 (3..5,3..5)
    int hut_a = (lx <= 2 && lz <= 2);
    int hut_b = (lx >= 3 && lz >= 3);
    int hx = hut_a ? lx : lx - 3;
    int hz = hut_a ? lz : lz - 3;
    if (!hut_a && !hut_b) {
      // 两屋之间的空地：地板铺路
      return (ly == 0) ? B_coarse_dirt : ((ly <= 1) ? B_air : 0xFF);
    }
    int corner = (hx == 0 || hx == 2) && (hz == 0 || hz == 2);
    int wall = (hx == 0 || hx == 2 || hz == 0 || hz == 2);
    if (ly == 0) return B_oak_planks;
    if (ly >= 1 && ly <= 2) {
      if (corner) return B_oak_log;
      if (wall) {
        if (hz == 2 && hx == 1 && ly <= 1) return B_air;            // 门
        if (ly == 2 && hx == 1 && hz == 0) return B_glass;          // 窗
        return B_oak_planks;
      }
      return B_air;
    }
    if (ly == 3) return B_oak_planks;                                // 屋顶
    return B_air;
  }

  // 其余为 5x5
  int corner = (lx == 0 || lx == 4) && (lz == 0 || lz == 4);
  int wall = (lx == 0 || lx == 4 || lz == 0 || lz == 4);

  switch (s.type) {
    case STRUCT_HOUSE: {
      if (ly == 0) return B_oak_planks;
      if (ly >= 1 && ly <= 3) {
        if (corner) return B_oak_log;
        if (wall) {
          if (lz == 4 && lx == 2 && ly <= 2) return B_air;        // 门
          if (ly == 2 && ((lx == 2 && (lz == 0 || lz == 4)) || (lz == 2 && (lx == 0 || lx == 4)))) {
            return B_glass;                                       // 窗
          }
          return B_oak_planks;
        }
        if (ly == 1 && lx == 2 && lz == 1) return B_crafting_table;
        if (ly == 1 && lx == 1 && lz == 3) return B_torch;
        return B_air;
      }
      if (ly == 4) return B_oak_planks;                           // 屋顶
      return B_air;
    }
    case STRUCT_RUIN: {
      if (ly == 0) return B_cobblestone;
      if (ly >= 1 && ly <= 2 && wall) {
        // 按位置散开缺口，形成残垣
        uint8_t r = (uint8_t)(hash >> (((lx * 5 + lz) + ly * 7) & 15));
        if ((r & 3) == 0) return B_air;
        return (r & 4) ? B_mossy_cobblestone : B_cobblestone;
      }
      if (ly >= 1 && ly <= 2) return B_air;
      return B_air;
    }
    case STRUCT_IGLOO: {
      if (ly == 0) return B_snow_block;
      if (ly >= 1 && ly <= 2 && wall) {
        if (lz == 4 && lx == 2 && ly <= 1) return B_air;
        return B_snow_block;
      }
      if (ly == 3) return B_snow_block;
      return B_air;
    }
    case STRUCT_DESERT_HUT: {
      if (ly == 0) return B_sandstone;
      if (ly >= 1 && ly <= 2 && wall) {
        if (lz == 4 && lx == 2 && ly <= 1) return B_air;
        if (ly == 2 && lx == 2 && lz == 0) return B_glass;
        return B_sandstone;
      }
      if (ly == 3) return wall ? B_chiseled_sandstone : B_sandstone;
      return B_air;
    }
    case STRUCT_SHACK: {
      if (ly == 0) return B_oak_planks;
      if (ly >= 1 && ly <= 2 && wall) return corner ? B_oak_log : B_oak_planks;
      if (ly == 3) return wall ? B_oak_planks : B_air;
      if (ly >= 1 && ly <= 2) return B_air;
      return B_air;
    }
    default: return 0xFF;
  }
}

// 沙漠岩浆坑（约 1/97 区块），3x3 浅坑底部铺岩浆
static uint8_t lavaPitBlockAt(ChunkAnchor anchor, uint8_t rx, uint8_t rz, int y, uint8_t height) {
  if (anchor.biome != W_desert) return 0xFF;
  if ((anchor.hash % 97) != 0) return 0xFF;
  if (height < 65) return 0xFF;

  int dx = (int)rx - 3, dz = (int)rz - 3;
  if (dx < -1 || dx > 1 || dz < -1 || dz > 1) return 0xFF;

  if (y == height) return B_lava;
  if (y == height - 1) return B_lava;
  if (y == height + 1) return B_air;
  return 0xFF;
}

// 地表水池（约 1/17 区块），圆形浅水洼
static uint8_t pondBlockAt(ChunkAnchor anchor, uint8_t rx, uint8_t rz, int y, uint8_t height) {
  if (anchor.biome != W_plains && anchor.biome != W_mangrove_swamp) return 0xFF;
  if (height < 64) return 0xFF;
  if ((anchor.hash % 17) != 0) return 0xFF;

  int cx = 3 + (int)((anchor.hash >> 4) % 2);
  int cz = 3 + (int)((anchor.hash >> 6) % 2);
  int dx = (int)rx - cx, dz = (int)rz - cz;
  int d2 = dx * dx + dz * dz;

  if (d2 <= 3) {
    if (y == height) return B_water;
    if (y == height + 1) return B_air;
    if (y == height - 1) return B_clay;
  } else if (d2 <= 5) {
    if (y == height) return B_sand;
  }
  return 0xFF;
}

// 蜿蜒水带（平原），用 x/z 的连续函数保证跨区块连贯
static bool riverHere(int x, int z) {
  int wobble = ((z * 3) >> 4) * 5;
  int band = mod_abs(x + wobble + (int)(world_seed & 31), 37);
  return band < 3;
}

// 浮空岛（约 1/61 平原区块，y 76~78）
static uint8_t skyIslandBlockAt(ChunkAnchor anchor, uint8_t rx, uint8_t rz, int y) {
  if (anchor.biome != W_plains) return 0xFF;
  if ((anchor.hash % 61) != 0) return 0xFF;

  int cx = 1 + (int)((anchor.hash >> 10) % 6);
  int cz = 1 + (int)((anchor.hash >> 13) % 6);
  int dx = (int)rx - cx, dz = (int)rz - cz;
  int d2 = dx * dx + dz * dz;
  if (d2 > 4) return 0xFF;

  if (y == 76) return B_stone;
  if (y == 77) return B_dirt;
  if (y == 78) return B_grass_block;
  if (y == 79 && dx == 0 && dz == 0) return B_oak_log;
  if (y == 80 && dx >= -1 && dx <= 1 && dz >= -1 && dz <= 1) return B_oak_leaves;
  if (y > 78) return B_air;
  return 0xFF;
}

uint8_t getSafeSpawnY(int16_t x, int16_t z) {
  for (int y = 79; y >= 64; y--) {
    uint8_t b = getBlockAt(x, (uint8_t)y, z);
    if (b != B_air && !isReplaceableBlock(b) && b != B_water && b != B_lava) {
      return (uint8_t)(y + 1);
    }
  }
  return (uint8_t)(getHeightAt(x, z) + 1);
}

static uint8_t getHeightAtFromAnchors(int rx, int rz, ChunkAnchor *anchor_ptr) {
  if (rx == 0 && rz == 0) {
    int height = getCornerHeight(anchor_ptr[0].hash, anchor_ptr[0].biome);
    if (height > 67) return height - 1;
  }
  return interpolate(
    getCornerHeight(anchor_ptr[0].hash, anchor_ptr[0].biome),
    getCornerHeight(anchor_ptr[1].hash, anchor_ptr[1].biome),
    getCornerHeight(anchor_ptr[16 / CHUNK_SIZE + 1].hash, anchor_ptr[16 / CHUNK_SIZE + 1].biome),
    getCornerHeight(anchor_ptr[16 / CHUNK_SIZE + 2].hash, anchor_ptr[16 / CHUNK_SIZE + 2].biome),
    rx, rz
  );
}

uint8_t getHeightAtFromHash(int rx, int rz, int _x, int _z, uint32_t chunk_hash, uint8_t biome) {
  if (rx == 0 && rz == 0) {
    int height = getCornerHeight(chunk_hash, biome);
    if (height > 67) return height - 1;
  }
  return interpolate(
    getCornerHeight(chunk_hash, biome),
    getCornerHeight(getChunkHash(_x + 1, _z), getChunkBiome(_x + 1, _z)),
    getCornerHeight(getChunkHash(_x, _z + 1), getChunkBiome(_x, _z + 1)),
    getCornerHeight(getChunkHash(_x + 1, _z + 1), getChunkBiome(_x + 1, _z + 1)),
    rx, rz
  );
}

uint8_t getHeightAt(int x, int z) {
  int _x = div_floor(x, CHUNK_SIZE);
  int _z = div_floor(z, CHUNK_SIZE);
  int rx = mod_abs(x, CHUNK_SIZE);
  int rz = mod_abs(z, CHUNK_SIZE);
  uint32_t chunk_hash = getChunkHash(_x, _z);
  uint8_t biome = getChunkBiome(_x, _z);
  return getHeightAtFromHash(rx, rz, _x, _z, chunk_hash, biome);
}

static ChunkFeature getFeatureFromAnchor(ChunkAnchor anchor) {
  ChunkFeature feature;
  uint8_t feature_position = anchor.hash % (CHUNK_SIZE * CHUNK_SIZE);
  feature.x = feature_position % CHUNK_SIZE;
  feature.z = feature_position / CHUNK_SIZE;
  uint8_t skip_feature = 0;

  if (anchor.biome != W_mangrove_swamp) {
    if (feature.x < 3 || feature.x > CHUNK_SIZE - 3) skip_feature = 1;
    else if (feature.z < 3 || feature.z > CHUNK_SIZE - 3) skip_feature = 1;
  }

  if (skip_feature) {
    feature.y = 0xFF;
  } else {
    feature.x += anchor.x * CHUNK_SIZE;
    feature.z += anchor.z * CHUNK_SIZE;
    feature.y = getHeightAtFromHash(
      mod_abs(feature.x, CHUNK_SIZE), mod_abs(feature.z, CHUNK_SIZE),
      anchor.x, anchor.z, anchor.hash, anchor.biome
    ) + 1;
    feature.variant = (anchor.hash >> (feature.x + feature.z)) & 1;
  }
  return feature;
}

// 地形扩建总开关：置 0 可一键退回原始地形，用于排查"网络协议错误"这类客户端问题
#ifndef TERRAIN_EXTRAS
#define TERRAIN_EXTRAS 1
#endif

// ---- 地下矿洞：沿 X / 沿 Z 两组隧道 + 少量洞厅，纯哈希定点生成 ----
static inline uint32_t caveMix(uint32_t a, uint32_t b, uint32_t c) {
  uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA6Bu ^ c * 0xC2B2AE35u;
  h ^= h >> 15; h *= 0x27D4EB2Fu; h ^= h >> 13;
  return h;
}

static bool caveAt(int x, int y, int z, uint8_t height, uint32_t chash) {
  if (y < 6) return false;                    // 基岩附近不掏空
  int top = (int)height - 5;                  // 地表往下留 4 层，别把草皮顶穿
  if (top <= 10 || y > top) return false;

  uint32_t seed = chash ^ 0x51ED2701u;
  uint32_t span = (uint32_t)(top - 9);

  // 隧道组 1：沿 X 方向延伸（每 8 格 z 一条中心线，中心高度哈希决定）
  {
    uint32_t h = caveMix((uint32_t)(x >> 6), (uint32_t)((z >> 3) & 0xFF), seed);
    int cy = 9 + (int)((h >> 3) % span);
    int wob = (int)((h >> 11) & 7);
    int dz = (z & 7) - wob; if (dz < 0) dz = -dz;
    int dy = y - cy; if (dy < 0) dy = -dy;
    if (dz == 0 && dy <= 1) return true;      // 3 格高、1 格宽的直洞
    if (dz == 1 && dy <= 0) return true;      // 偶尔加宽一格
  }

  // 隧道组 2：沿 Z 方向延伸，和组 1 交叉出十字洞
  {
    uint32_t h = caveMix((uint32_t)((x >> 3) & 0xFF), (uint32_t)(z >> 6), seed ^ 0x3C6EF372u);
    int cy = 9 + (int)((h >> 3) % span);
    int wob = (int)((h >> 11) & 7);
    int dx = (x & 7) - wob; if (dx < 0) dx = -dx;
    int dy = y - cy; if (dy < 0) dy = -dy;
    if (dx == 0 && dy <= 1) return true;
    if (dx == 1 && dy <= 0) return true;
  }

  // 洞厅：粗格 8×4×8 的低概率整块掏空，形成大小不一的空腔
  {
    uint32_t h = caveMix((uint32_t)((x + 4096) >> 3),
                         (uint32_t)((y + 64) >> 2) * 131u,
                         (uint32_t)((z + 4096) >> 3) ^ seed);
    if ((h & 0xFF) < 9) return true;
  }
  return false;
}

static uint8_t getTerrainAtFromCache(int x, int y, int z, int rx, int rz,
                                      ChunkAnchor anchor, ChunkFeature feature, uint8_t height) {
#if TERRAIN_EXTRAS
  // ---- 扩建内容优先接管（结构物 / 浮空岛 / 水池 / 河流）----
  // 先做廉价取模再进入开销较大的分支，保证 1/40 以外区块几乎零成本
  if ((anchor.hash % 20) == 0) {
    uint8_t sb = structureBlockAt(getStructureSpecCached(anchor), (uint8_t)rx, (uint8_t)rz, y, height, anchor.hash);
    if (sb != 0xFF) return sb;
  }

  uint8_t sky = skyIslandBlockAt(anchor, (uint8_t)rx, (uint8_t)rz, y);
  if (sky != 0xFF) return sky;

  uint8_t lava = lavaPitBlockAt(anchor, (uint8_t)rx, (uint8_t)rz, y, height);
  if (lava != 0xFF) return lava;

  uint8_t pond = pondBlockAt(anchor, (uint8_t)rx, (uint8_t)rz, y, height);
  if (pond != 0xFF) return pond;

  if (anchor.biome == W_plains && height >= 64 && height <= 66 && riverHere(x, z)) {
    if (y == height) return B_water;
    if (y == height + 1) return B_air;
  }
#endif

  if (y >= 64 && y >= height && feature.y != 255) switch (anchor.biome) {
    case W_plains: {
      if (feature.y < 64) break;
      uint8_t forest = ((anchor.hash >> 27) & 3) == 0;   // 1/4 平原为密林，树冠更大
      uint8_t birch_style = (((anchor.hash >> ((feature.x + feature.z) & 7)) & 3) == 0);
      uint8_t ore_tree_roll = (uint8_t)(
        ((anchor.hash >> ((feature.x ^ feature.z) & 15)) & 255) ^
        ((feature.x * 17 + feature.z * 31) & 255)
      );
      uint8_t ore_tree_type = 0;
      if (ore_tree_roll & 1) ore_tree_type = (ore_tree_roll & 2) ? 2 : 1;

      uint8_t tree_log = B_oak_log, tree_leaf = B_oak_leaves, tree_root = B_dirt;
      if (ore_tree_type == 1) { tree_log = B_iron_ore; tree_leaf = B_stone; tree_root = B_iron_ore; }
      else if (ore_tree_type == 2) { tree_log = B_diamond_ore; tree_leaf = B_stone; tree_root = B_diamond_ore; }
      int tree_top = feature.y - feature.variant + 6 + (birch_style ? 1 : 0);

      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return tree_root;
        if (y >= feature.y && y < tree_top) return tree_log;
      }
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
      int leaf_reach = forest ? 4 : 3;
      if (dx < leaf_reach && dz < leaf_reach && y > tree_top - 4 && y < tree_top - 1) {
        if (y == tree_top - 2 && dx == leaf_reach - 1 && dz == leaf_reach - 1) break;
        return tree_leaf;
      }
      if (dx < 2 && dz < 2 && y >= tree_top - 1 && y <= tree_top) {
        if (y == tree_top && dx == 1 && dz == 1) break;
        return tree_leaf;
      }
      if (y == height) return B_grass_block;
      if (y == height + 1) {
        uint8_t decoration = getPlainsDecoration(anchor, rx, rz, height);
        if (decoration) return decoration;
      }
      return B_air;
    }
    case W_desert: {
      if (x != feature.x || z != feature.z) break;
      if (feature.variant == 0) {
        if (y == height + 1) return B_dead_bush;
      } else if (y > height) {
        if (height & 1 && y <= height + 3) return B_cactus;
        if (y <= height + 2) return B_cactus;
      }
      break;
    }
    case W_mangrove_swamp: {
      if (x == feature.x && z == feature.z && y == 64 && height < 63) return B_lily_pad;
      if (y == height + 1) {
        uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
        uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
        if (dx + dz < 4) return B_moss_carpet;
      }
      break;
    }
    case W_snowy_plains: {
      if (x == feature.x && z == feature.z && y == height + 1 && height >= 64) return B_short_grass;
      break;
    }
    default: break;
  }

  // 地表层
  if (height >= 63) {
    if (y == height) {
      // 山地变体的裸岩山脊与雪线
      if (anchor.biome == W_plains && height >= 72) return B_stone;
      if (anchor.biome == W_snowy_plains && height >= 76) return B_snow_block;
      if (anchor.biome == W_mangrove_swamp) return B_mud;
      if (anchor.biome == W_snowy_plains) return B_snowy_grass_block;
      if (anchor.biome == W_desert) return B_sand;
      if (anchor.biome == W_beach) return B_sand;
      return B_grass_block;
    }
    if (anchor.biome == W_plains && y == height + 1) {
      uint8_t decoration = getPlainsDecoration(anchor, rx, rz, height);
      if (decoration) return decoration;
      // 蘑菇点缀（森林/阴影处）
      uint8_t mush = (uint8_t)((anchor.hash >> ((rx * 3 + rz) & 15)) & 63);
      if (mush == 0) return B_red_mushroom;
      if (mush == 1) return B_brown_mushroom;
      if (mush == 2) return B_fern;
    }
    if (anchor.biome == W_snowy_plains && y == height + 1) return B_snow;
  }

  // 矿石和洞穴
  if (y <= height - 4) {
    if (caveAt(x, y, z, height, anchor.hash)) return B_air;

    uint8_t ore_y = ((rx & 15) << 4) + (rz & 15);
    ore_y ^= ore_y << 4; ore_y ^= ore_y >> 5; ore_y ^= ore_y << 1;
    ore_y &= 63;

    if (y == ore_y) {
      uint8_t ore_probability = (anchor.hash >> (ore_y % 24)) & 255;
      if (y < 15) {
        if (ore_probability < 10) return B_diamond_ore;
        if (ore_probability < 12) return B_gold_ore;
        if (ore_probability < 15) return B_redstone_ore;
      }
      if (y < 30) {
        if (ore_probability < 3) return B_gold_ore;
        if (ore_probability < 8) return B_redstone_ore;
      }
      if (y < 54) {
        if (ore_probability < 30) return B_iron_ore;
        if (ore_probability < 40) return B_copper_ore;
      }
      if (ore_probability < 60) return B_coal_ore;
      if (y < 5) return B_lava;
      return B_cobblestone;
    }
    return B_stone;
  }

  // 中间土层
  if (y <= height) {
    if (anchor.biome == W_desert) return B_sandstone;
    if (anchor.biome == W_mangrove_swamp) return B_mud;
    if (anchor.biome == W_beach && height > 64) return B_sandstone;
    return B_dirt;
  }

  // 海平面以下补水/冰
  if (y == 63 && anchor.biome == W_snowy_plains) return B_ice;
  if (y < 64) return B_water;
  return B_air;
}

uint8_t getTerrainAt(int x, int y, int z, ChunkAnchor anchor) {
  if (y > 80) return B_air;
  int rx = x % CHUNK_SIZE; if (rx < 0) rx += CHUNK_SIZE;
  int rz = z % CHUNK_SIZE; if (rz < 0) rz += CHUNK_SIZE;
  ChunkFeature feature = getFeatureFromAnchor(anchor);
  uint8_t height = getHeightAtFromHash(rx, rz, anchor.x, anchor.z, anchor.hash, anchor.biome);
  return getTerrainAtFromCache(x, y, z, rx, rz, anchor, feature, height);
}

uint8_t getBlockAt(int x, int y, int z) {
  if (y < 0) return B_bedrock;
  uint8_t block_change = getBlockChange(x, y, z);
  if (block_change != 0xFF) return block_change;
  int16_t anchor_x = div_floor(x, CHUNK_SIZE);
  int16_t anchor_z = div_floor(z, CHUNK_SIZE);
  ChunkAnchor anchor = { anchor_x, anchor_z, getChunkHash(anchor_x, anchor_z), getChunkBiome(anchor_x, anchor_z) };
  return getTerrainAt(x, y, z, anchor);
}

uint8_t chunk_section[4096];

uint8_t buildChunkSectionInto(uint8_t *section_out, int cx, int cy, int cz) {
  ChunkAnchor chunk_anchors[(16 / CHUNK_SIZE + 1) * (16 / CHUNK_SIZE + 1)];
  ChunkFeature chunk_features[256 / (CHUNK_SIZE * CHUNK_SIZE)];
  uint8_t chunk_section_height[16][16];

  int anchor_index = 0, feature_index = 0;
  for (int i = cz; i < cz + 16 + CHUNK_SIZE; i += CHUNK_SIZE) {
    for (int j = cx; j < cx + 16 + CHUNK_SIZE; j += CHUNK_SIZE) {
      ChunkAnchor *anchor = chunk_anchors + anchor_index;
      anchor->x = j / CHUNK_SIZE;
      anchor->z = i / CHUNK_SIZE;
      anchor->hash = getChunkHash(anchor->x, anchor->z);
      anchor->biome = getChunkBiome(anchor->x, anchor->z);
      if (i != cz + 16 && j != cx + 16) {
        chunk_features[feature_index] = getFeatureFromAnchor(*anchor);
        feature_index++;
      }
      anchor_index++;
    }
  }

  for (int i = 0; i < 16; i++) {
    for (int j = 0; j < 16; j++) {
      anchor_index = (j / CHUNK_SIZE) + (i / CHUNK_SIZE) * (16 / CHUNK_SIZE + 1);
      ChunkAnchor *anchor_ptr = chunk_anchors + anchor_index;
      chunk_section_height[j][i] = getHeightAtFromAnchors(j % CHUNK_SIZE, i % CHUNK_SIZE, anchor_ptr);
    }
  }

  for (int j = 0; j < 4096; j += 8) {
    int y = j / 256 + cy;
    int rz = j / 16 % 16;
    int rz_mod = rz % CHUNK_SIZE;
    feature_index = (j % 16) / CHUNK_SIZE + (j / 16 % 16) / CHUNK_SIZE * (16 / CHUNK_SIZE);
    anchor_index = (j % 16) / CHUNK_SIZE + (j / 16 % 16) / CHUNK_SIZE * (16 / CHUNK_SIZE + 1);
    for (int offset = 0; offset < 8; offset++) {
      int k = j + offset;
      int rx = k % 16;
      section_out[k] = getTerrainAtFromCache(
        rx + cx, y, rz + cz,
        rx % CHUNK_SIZE, rz_mod,
        chunk_anchors[anchor_index],
        chunk_features[feature_index],
        chunk_section_height[rx][rz]
      );
    }
  }
  return chunk_anchors[0].biome;
}

uint8_t buildChunkSection(int cx, int cy, int cz) {
  return buildChunkSectionInto(chunk_section, cx, cy, cz);
}

// ============================================================
// Structures
// ============================================================

static void setBlockIfReplaceable(int16_t x, uint8_t y, int16_t z, uint8_t block) {
  uint8_t target = getBlockAt(x, y, z);
  if (!isReplaceableBlock(target) && target != B_oak_leaves) return;
  makeBlockChange(x, y, z, block);
}

void placeTreeStructure(int16_t x, uint8_t y, int16_t z) {
  uint32_t r = fast_rand();
  uint8_t birch_style = ((r >> 3) & 3) == 0;
  uint8_t log_block = B_oak_log;
  uint8_t height = (birch_style ? 5 : 4) + (r % 3);

  makeBlockChange(x, y - 1, z, B_dirt);
  makeBlockChange(x, y, z, log_block);
  for (int i = 1; i < height; i++) setBlockIfReplaceable(x, y + i, z, log_block);

  uint8_t t = 2;
  for (int i = -2; i <= 2; i++) {
    for (int j = -2; j <= 2; j++) {
      setBlockIfReplaceable(x + i, y + height - 3, z + j, B_oak_leaves);
      if ((i == 2 || i == -2) && (j == 2 || j == -2)) { t++; if ((r >> t) & 1) continue; }
      setBlockIfReplaceable(x + i, y + height - 2, z + j, B_oak_leaves);
    }
  }
  for (int i = -1; i <= 1; i++) {
    for (int j = -1; j <= 1; j++) {
      setBlockIfReplaceable(x + i, y + height - 1, z + j, B_oak_leaves);
      if ((i == 1 || i == -1) && (j == 1 || j == -1)) { t++; if ((r >> t) & 1) continue; }
      setBlockIfReplaceable(x + i, y + height, z + j, B_oak_leaves);
    }
  }
}
