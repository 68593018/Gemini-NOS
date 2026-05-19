#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include "nos_shm.h"
#include "nos_log.h"
#include "nos_node_priv.h"

static volatile int g_keep_running = 1;

/* 为满足核心库依赖，提供一个空的上下文和配置 */
nos_node_ctx_t g_node_ctx = {
    .keep_running = 1,
    .loaded_count = 0,
    .worker_count = 0
};
static const nos_node_def_t dummy_def = { .name = "Daemon" };

static void sig_handler(int sig) {
    printf("[Daemon] Received signal %d, shutting down...\n", sig);
    g_keep_running = 0;
}

int main(void) {
    g_node_ctx.node_def = &dummy_def;
    
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("[Daemon] Starting NOS Shared Memory Daemon...\n");

    /* 强制以 Creator 身份初始化 SHM */
    extern nos_status_t _nos_shm_create_and_init(void);
    if (_nos_shm_create_and_init() != NOS_OK) {
        fprintf(stderr, "[Daemon] Failed to initialize SHM. Exiting.\n");
        return -1;
    }

    printf("[Daemon] SHM Initialization complete. Waiting for nodes...\n");

    /* 暂仅做驻留保活，后续可加入 GC 和心跳检测 */
    while (g_keep_running) {
        sleep(1);
    }

    /* 退出时清理资源 (可选) */
    shm_unlink(NOS_SHM_NAME);
    printf("[Daemon] Shutdown complete. SHM unlinked.\n");
    return 0;
}
