// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.

#ifndef CRAFTING_H
#define CRAFTING_H

#include "game_types.h"

// 合成：max_w / max_h 是本次可用工作台尺寸（玩家背包 2×2 传 2,2；工作台传 3,3）
void getCraftingOutput(PlayerData *player, uint8_t max_w, uint8_t max_h,
                       uint8_t *count, uint16_t *item);

// 熔炉：槽位布局 craft_items[0]=原料 1=燃料 2=产物
// 返回“现在能烧出几个”，用于把结果先显示在产物槽里
void furnacePreview(PlayerData *player, uint8_t *count, uint16_t *item);

// 熔炉：点产物槽时真正结算一次，返回本次烧出的物品与数量
uint8_t furnaceTake(PlayerData *player, uint8_t want, uint16_t *out_item);

// 合成表对外只读视图：给客户端“配方书/已解锁合成”用（内部表结构不暴露）
int craftRecipeCount(void);
bool craftRecipeInfo(int i, uint8_t *w, uint8_t *h, uint16_t need9[9], uint16_t *out, uint8_t *count);

#endif
