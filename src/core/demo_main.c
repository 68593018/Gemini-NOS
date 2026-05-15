#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_scheduler.h"

/* 声明外部组件实例 */
extern nos_component_t g_comp1;
extern nos_component_t g_comp2;

/* 线程启动函数包装器 */
static void* scheduler_thread_entry(void *arg) {
    nos_thread_t *thread_obj = (nos_thread_t *)arg;
    nos_scheduler_run_loop(thread_obj);
    return NULL;
}

int main() {
    printf("--- NOS Component Persistent Thread Demo ---\n");

    /* 1. 初始化调度器线程对象 */
    nos_thread_t main_thread;
    nos_scheduler_init_thread(&main_thread, 1, "WorkerThread-1");

    /* 2. 注册组件与服务 */
    nos_scheduler_register_component(1, &g_comp1);
    nos_scheduler_register_component(1, &g_comp2);
    nos_service_register_provider(2, &g_comp2);

    /* 3. 调用组件初始化 */
    g_comp1.init(&g_comp1);
    g_comp2.init(&g_comp2);

    /* 4. 启动真实线程 */
    pthread_t tid;
    pthread_create(&tid, NULL, scheduler_thread_entry, &main_thread);
    printf("[Main] Scheduler thread created (TID: %lu)\n", tid);

    /* 5. 启动组件逻辑 */
    g_comp1.start(&g_comp1);

    /* 6. 主程序进入持久化状态 (模拟 Daemon) */
    printf("[Main] System is running. Press Ctrl+C to stop.\n");
    while (1) {
        sleep(10); // 主线程可以去干别的事，或者仅仅挂起
    }

    return 0;
}
