#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>
#include "nos_node_priv.h"
#include "nos_node_mgr.h"
#include "nos_cli.h"
#include "nos_buffer.h"

/* 定义并初始化全局上下文 */
nos_node_ctx_t g_node_ctx = {
    .keep_running = 1,
    .loaded_count = 0,
    .worker_count = 0
};

static void signal_handler(int sig) {
    printf("\n[Node] Received signal %d, initiating graceful shutdown...\n", sig);
    g_node_ctx.keep_running = 0;
}

static void* scheduler_thread_entry(void *arg) {
    nos_scheduler_run_loop((nos_thread_t *)arg);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: %s <node_name>\n", argv[0]); return -1; }

    const char *node_name = argv[1];
    const nos_node_def_t *node_def = nos_manifest_get_node(node_name);
    if (!node_def) { printf("Error: Node '%s' not found.\n", node_name); return -1; }

    printf("--- [NOS Node: %s] Starting ---\n", node_name);
    nos_buffer_init_pool(node_def->buffer_pools);

    /* 1. 初始化管理面与数据面线程 */
    node_init_mgmt(node_def->uds_path);
    node_init_workers(node_def);
    node_setup_routing(node_name);

    /* 2. 触发启动生命周期 */
    for (uint32_t i = 0; i < g_node_ctx.loaded_count; i++) {
        if (g_node_ctx.loaded_info[i].comp->start) {
            g_node_ctx.loaded_info[i].comp->start(g_node_ctx.loaded_info[i].comp);
        }
    }

    /* 3. 注册信号量 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 4. 物理线程启动 */
    pthread_create(&g_node_ctx.mgmt_tid, NULL, scheduler_thread_entry, g_node_ctx.mgmt_thread);
    for (uint32_t i = 0; i < g_node_ctx.worker_count; i++) {
        pthread_create(&g_node_ctx.worker_tids[i], NULL, scheduler_thread_entry, &g_node_ctx.worker_threads[i]);
    }

    /* 5. 启动 CLI 交互界面 */
    node_cli_start(node_name);

    /* 6. 等待退出 */
    while (g_node_ctx.keep_running) sleep(1);

    /* 7. 优雅关闭 */
    printf("[Node] Shutting down...\n");
    nos_scheduler_stop(g_node_ctx.mgmt_thread);
    for (uint32_t i = 0; i < g_node_ctx.worker_count; i++) nos_scheduler_stop(&g_node_ctx.worker_threads[i]);

    /* 等待物理线程退出 */
    pthread_join(g_node_ctx.mgmt_tid, NULL);
    for (uint32_t i = 0; i < g_node_ctx.worker_count; i++) pthread_join(g_node_ctx.worker_tids[i], NULL);
    
    /* 等待 CLI 线程退出 */
    pthread_join(g_node_ctx.cli_tid, NULL);

    /* 8. 资源清理 */
    for (int i = (int)g_node_ctx.loaded_count - 1; i >= 0; i--) {
        if (g_node_ctx.loaded_info[i].comp->stop) g_node_ctx.loaded_info[i].comp->stop(g_node_ctx.loaded_info[i].comp);
        free((void*)g_node_ctx.loaded_info[i].comp->name);
        free(g_node_ctx.loaded_info[i].comp);
        dlclose(g_node_ctx.loaded_info[i].handle);
    }
    free(g_node_ctx.mgmt_thread);
    free(g_node_ctx.worker_threads);

    printf("[Node] Shutdown complete.\n");
    return 0;
}
