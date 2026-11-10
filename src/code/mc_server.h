// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../NOTICE.

#ifndef MC_SERVER_H
#define MC_SERVER_H

#ifdef _WIN32
#include "win_platform.h"
#include "win_network_layer.h"
#else
#include <WiFi.h>
#include "network_layer.h"
#endif

#include "packet_codec.h"
#include "game_types.h"

class MinecraftServer {
 public:
  explicit MinecraftServer(uint16_t port);

  bool begin();
  void poll();

  // 调试出口（串口 AT 命令用）：不开客户端也能塞一个掉落物、打印地上掉落物。
  // 主要给自己验"掉落物不会被超时删掉"用。
  void debugAddDrop(uint16_t item, uint8_t count);
  void debugPrintDrops();
  // 调试出口：等价于"玩家对着 (x,y,z) 右键"，用 AT+USE= 触发，
  // 不需要真的把准星对准方块，方便无客户端/无鼠标时验证容器与附魔台。
  void debugUseBlock(int16_t x, int16_t y, int16_t z);

 private:
  // ============ 客户端槽位 ============
  struct ClientSlot {
    int fd;              // socket fd, -1 表示未使用
    uint8_t state;
    bool used;
    bool config_received_info;
    bool config_received_packs;
    uint8_t uuid[16];
    char name[16];
    int player_index;
    // 延迟发块队列
    int8_t chunk_queue_idx;
    int16_t chunk_center_x;
    int16_t chunk_center_z;
    uint32_t chunk_next_send_ms;
    // 移动边缘区块队列
    int8_t edge_queue_count;
    int8_t edge_queue_idx;
    int16_t edge_chunks_x[50];
    int16_t edge_chunks_z[50];
    // 自适应区块发送速率
    uint16_t chunk_interval_ms;   // 当前发块间隔 (ms), 10~500
    uint32_t chunk_send_start_ms; // 上次发块开始时间, 用于测量耗时
    uint8_t  chunk_slow_count;    // 连续超时次数, 用于退避
    // 当前打开的箱子（用于内容存档）
    uint8_t *chest_buf;           // 指向 block_changes 里的 27 格存储区
    int16_t  chest_x;
    int16_t  chest_z;
    uint8_t  chest_y;
    bool     chest_open;
  };

  static const uint8_t kMaxClients = MAX_PLAYERS;

  // ============ 连接管理 ============
  bool acceptClient_();
  void serviceClient_(uint8_t slot_index);
  void closeClient_(uint8_t slot_index, int cause);

  // ============ 协议状态处理 ============
  bool handleHandshake_(ClientSlot& slot, PacketCodec& codec, int32_t packet_id);
  bool handleStatus_(ClientSlot& slot, PacketCodec& codec, int32_t packet_id, int32_t packet_len);
  bool handleLogin_(ClientSlot& slot, PacketCodec& codec, int32_t packet_id, int32_t packet_len);
  bool handleConfiguration_(ClientSlot& slot, PacketCodec& codec, int32_t packet_id, int32_t packet_len);
  bool handlePlay_(ClientSlot& slot, PacketCodec& codec, int32_t packet_id, int32_t packet_len);

  // ============ 发包: 状态/登录/配置 ============
  bool sendStatusResponse_(PacketCodec& codec);
  bool sendLoginSuccess_(PacketCodec& codec, const uint8_t uuid[16], const char* name);
  bool sendBrand_(PacketCodec& codec);
  bool sendPluginMessage_(PacketCodec& codec, const char* channel, const uint8_t* data, uint32_t data_len);
  bool sendKnownPacks_(PacketCodec& codec);
  bool sendEnabledFeatures_(PacketCodec& codec);
  bool sendRegistries_(PacketCodec& codec);
  bool sendFinishConfiguration_(PacketCodec& codec);

  // ============ 发包: Play ============
  bool sendLoginPlay_(PacketCodec& codec, uint32_t entity_id);
  bool sendSynchronizePlayerPosition_(PacketCodec& codec, double x, double y, double z, float yaw, float pitch);
  bool sendSetEntityMotion_(PacketCodec& codec, int entity_id, double vx, double vy, double vz);
  bool sendSetDefaultSpawnPosition_(PacketCodec& codec, int64_t x, int64_t y, int64_t z, float yaw, float pitch);
  bool sendStartWaitingForChunks_(PacketCodec& codec);
  bool sendSetCenterChunk_(PacketCodec& codec, int x, int z);
  bool sendChunkDataAndUpdateLight_(PacketCodec& codec, int chunk_x, int chunk_z);
  bool sendKeepAlive_(PacketCodec& codec);
  bool sendUpdateTime_(PacketCodec& codec, uint64_t ticks);
  bool sendSetHealth_(PacketCodec& codec, uint8_t health, uint8_t food, uint16_t saturation);
  bool sendSetHeldItem_(PacketCodec& codec, uint8_t slot);
  bool sendSetContainerSlot_(PacketCodec& codec, int window_id, uint16_t slot, uint8_t count, uint16_t item);
  void sendHeldSlot_(PacketCodec& codec, PlayerData* player, uint8_t count, uint16_t item);
  bool sendSetContainerSlotEnch_(PacketCodec& codec, int window_id, uint16_t slot, uint8_t count, uint16_t item, uint8_t ench_level, const char* name = nullptr, uint8_t ench_id = 33);
  bool sendRecipeBookAdd_(PacketCodec& codec);
  bool sendBlockUpdate_(PacketCodec& codec, int64_t x, int64_t y, int64_t z, uint8_t block);
  bool sendBlockUpdateState_(PacketCodec& codec, int64_t x, int64_t y, int64_t z, uint16_t state);
  bool sendAcknowledgeBlockChange_(PacketCodec& codec, int sequence);
  bool sendPlayerInfoUpdateAddPlayer_(PacketCodec& codec, PlayerData& player);
  bool sendSpawnEntity_(PacketCodec& codec, int id, uint8_t* uuid, int type, double x, double y, double z, uint8_t yaw, uint8_t pitch);
  bool sendEntityAnimation_(PacketCodec& codec, int id, uint8_t animation);
  bool sendTeleportEntity_(PacketCodec& codec, int id, double x, double y, double z, float yaw, float pitch);
  bool sendSetHeadRotation_(PacketCodec& codec, int id, uint8_t yaw);
  bool sendUpdateEntityRotation_(PacketCodec& codec, int id, uint8_t yaw, uint8_t pitch);
  bool sendDamageEvent_(PacketCodec& codec, int entity_id, int type);
  bool sendRemoveEntity_(PacketCodec& codec, int entity_id);
  bool sendSystemChat_(PacketCodec& codec, const char* message, uint16_t len);
  bool sendEntityEvent_(PacketCodec& codec, int entity_id, uint8_t status);
  bool sendOpenScreen_(PacketCodec& codec, uint8_t window, const char* title, uint16_t length);
  bool sendOpenScreenTyped_(PacketCodec& codec, uint8_t container_id, uint8_t menu_type, const char* title, uint16_t length);
  bool sendSetExperience_(PacketCodec& codec, float progress, int32_t level, int32_t total);
  bool sendSetContainerData_(PacketCodec& codec, uint8_t window, int16_t data_id, int16_t value);
  bool sendUpdateRecipes_(PacketCodec& codec);
  bool sendRespawn_(PacketCodec& codec, uint8_t mode);
  // 天气 / 时间 / 掉落物
  void setWeather_(uint8_t w, bool announce);
  void broadcastTime_();
  void addDrop_(int16_t x, uint8_t y, int16_t z, uint16_t item, uint8_t count);
  void dropChestContents_(int16_t x, uint8_t y, int16_t z);
  void dropHeldItem_(PlayerData* player, uint8_t drop_n);
  bool sendSetEntityDataItem_(PacketCodec& codec, int id, uint16_t item, uint8_t count);
  bool sendSpawnItemEntity_(PacketCodec& codec, int id, uint8_t* uuid, double x, double y, double z);
  bool sendEntityDataProbe_(PacketCodec& codec, uint8_t entity_id);
  void tickDrops_();
  void dropSpawnEntity_(uint8_t idx);
  void dropRemoveEntity_(uint8_t idx);
  bool sendCommandTree_(PacketCodec& codec, uint8_t packet_id);
  bool sendGameEvent_(PacketCodec& codec, uint8_t reason, float value);
  void fixPlayerEmbedded_(uint8_t slot_idx, PlayerData* player);

  // ============ 附魔（服务端记录，物品包带附魔光效）============
  uint16_t enchantGet_(uint8_t slot_idx, uint8_t inv_slot);
  uint8_t enchantIdGet_(uint8_t slot_idx, uint8_t inv_slot);
  void applyEnchant_(uint8_t slot_idx, uint8_t inv_slot, uint8_t id, uint8_t level);

  // ============ 药水（喝下去给状态效果，附魔已整体移除）============
  // 每个快捷栏格记这瓶药水是什么效果；255 = 没有药水。
  // 物品堆本身不带组件（塞组件会让 container_set_slot 解码崩），
  // 所以药水就是「服务端记效果 + 客户端一瓶普通药水瓶」。

  unsigned long gm_last_change_ms_ = 0;
  uint8_t gm_last_slot_ = 255;
  bool runCommand_(uint8_t slot_idx, PlayerData* player, const char* cmdline);
  void teleportPlayer_(uint8_t slot_idx, int16_t tx, uint8_t ty, int16_t tz);
  void killPlayer_(uint8_t slot_idx);
  void dropAllInventory_(PlayerData* player);
  void setPlayerGameMode_(uint8_t slot_idx, uint8_t mode);
  bool sendPlayerAbilities_(PacketCodec& codec, uint8_t flags);

  // ============ 收包处理 ============
  bool consumeClientInformation_(PacketCodec& codec);
  bool consumePluginMessage_(PacketCodec& codec, int32_t payload_len);
  bool consumeKnownPacks_(PacketCodec& codec);
  bool skipRemainingPacket_(PacketCodec& codec, int32_t packet_len, int32_t packet_id);

  // ============ 游戏逻辑 ============
  void spawnPlayer_(uint8_t slot_index);
  void handlePlayerJoin_(uint8_t slot_index);
  void handlePlayerDisconnect_(uint8_t slot_index);
  void handleServerTick_();
  void handlePlayerAction_(PlayerData* player, int action, int16_t x, int16_t y, int16_t z);
  void handlePlayerUseItem_(PlayerData* player, int16_t x, int16_t y, int16_t z, uint8_t face);
  void hurtEntity_(int entity_id, int attacker_slot, uint8_t damage_type, uint8_t damage);
  void broadcastPlayerMetadata_(PlayerData* player);
  bool handleClickContainer_(uint8_t slot_idx, PacketCodec& codec, int32_t packet_len);
  bool canPlayerEat_(PlayerData* player);
  void doPlayerEat_(PlayerData* player);
  uint8_t getArmorItemSlot_(uint16_t item);
  void broadcastBlockChangesInArea_(int16_t x1, int16_t z1, int16_t x2, int16_t z2);
  void tickMobs_();
  void trySpawnMobNearPlayer_(PlayerData* player);
  void broadcastMobSpawn_(uint8_t type, int16_t x, uint8_t y, int16_t z);

  // ============ 辅助 ============
  PacketCodec codecForSlot_(uint8_t slot_index);
  uint16_t onlineCount_() const;
  int slotIndexForPlayer_(PlayerData* player);
  void processDeferredChunks_(uint8_t slot_index);

  // ============ 成员 ============
  NetworkLayer network_;
  ClientSlot clients_[kMaxClients];

  // 天气状态：0 晴 / 1 雨 / 2 雷雨；掉落物只存在内存，重启即清空
  uint8_t weather_ = 0;
  unsigned long weather_change_ms_ = 0;
  static const uint8_t kMaxDrops = 48;   // >= 背包 41 格：死亡爆背包时一格一件也能全放下
  struct DropItem { int16_t x; uint8_t y; int16_t z; uint16_t item; uint8_t count; unsigned long born_ms; int entity_id; uint8_t uuid[16]; };
  DropItem* drops_ = nullptr;   // 放堆里（DRAM .bss 只剩几百字节，塞不下数组）
  int64_t last_tick_time_us_;
};

#endif

// 串口调试接口：指令树包 id 自校准
void mcCommandTreeSetPacketId(uint8_t id);
void mcEntityDataSetId(uint8_t id);
uint8_t mcEntityDataGetId();
void mcEntityDataPrintState();
void mcJoinSetMask(uint8_t m);
void mcCommandTreePrintState();
// 串口调试：把服务端实例的掉落物入口挂给上面两个调试函数
void mcDebugHookDrops(void (*add_fn)(uint16_t, uint8_t), void (*print_fn)());
void mcDebugAddDrop(uint16_t item, uint8_t count);
void mcDebugPrintDrops();
// 串口调试：模拟"对某个方块右键"（AT+USE=x,y,z 用）
void mcDebugHookUseBlock(void (*use_fn)(int16_t, int16_t, int16_t));
void mcDebugUseBlock(int16_t x, int16_t y, int16_t z);
