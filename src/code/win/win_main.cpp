// SPDX-License-Identifier: GPL-3.0-only
// Attribution and modification details are documented in ../../NOTICE.

#ifdef _WIN32

#include "../mc_server.h"
#include "../game_state.h"
#include "win_platform.h"
#include <stdio.h>
#include <signal.h>
#include <time.h>

// Serial 全局实例
SerialClass Serial;

static const uint16_t MC_PORT = 25565;
static MinecraftServer server(MC_PORT);
static volatile bool running = true;

void signal_handler(int sig) {
  (void)sig;
  printf("\nShutting down...\n");
  running = false;
}

int main() {
  signal(SIGINT, signal_handler);

  // 固定种子
  world_seed = 0x6A0AEF04;
  rng_seed   = 0x6A0AEF04 ^ 0xDEADBEEF;
  printf("ESP32MC (Windows) starting...\n");
  printf("Port: %d  Seed: %08X\n", MC_PORT, world_seed);

  if (!server.begin()) {
    printf("Server begin failed!\n");
    return 1;
  }

  printf("Server running. Press Ctrl+C to stop.\n");
  printf("Connect with Minecraft client to localhost:%d\n\n", MC_PORT);

  while (running) {
    server.poll();
    Sleep(1);
  }

  printf("Server stopped.\n");
  return 0;
}

#endif // _WIN32
