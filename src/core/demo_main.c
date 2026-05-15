#include <stdio.h>
#include <string.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_scheduler.h"

/* 声明外部组件实例 */
extern nos_component_t g_comp1;
extern nos_component_t g_comp2;

/* 声明调度器内部函数 (在 nos_scheduler.c 实现) */
extern nos_status_t nos_scheduler_run_loop(nos_thread_t *self);

int main() {
    printf("--- NOS Component Epoll-Driven Demo ---\n");

    /* 1. 初始化调度器线程对象 */
    nos_thread_t main_thread;
    nos_scheduler_init_thread(&main_thread, 1, "MainWorker");

    /* 2. 注册组件到线程 */
    nos_scheduler_register_component(1, &g_comp1);
    nos_scheduler_register_component(1, &g_comp2);

    /* 3. 动态注册服务提供者 (C2 提供 SERVICE_HELLO_ID) */
    nos_service_register_provider(2, &g_comp2);

    /* 4. 调用组件初始化回调 */
    g_comp1.init(&g_comp1);
    g_comp2.init(&g_comp2);

    /* 5. 启动组件 (触发异步请求) */
    printf("\n--- Starting Components ---\n");
    g_comp1.start(&g_comp1);

    /* 6. 进入主调度循环 */
    printf("\n--- Entering Epoll Loop ---\n");
    nos_scheduler_run_loop(&main_thread);

    printf("\n--- Demo Finished ---\n");
    return 0;
}
