#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_scheduler.h"

/* 模拟组件 1, 2, 3 */
static void generic_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    printf("[%s] RECEIVED: From Component %u, MsgCode %u, Payload: %s\n", 
           self->name, msg->src_component, msg->msg_code, (char*)msg->payload);
}

static nos_component_t g_comp1 = { .id = 1, .name = "Comp-1", .on_msg = generic_on_msg };
static nos_component_t g_comp2 = { .id = 2, .name = "Comp-2", .on_msg = generic_on_msg };
static nos_component_t g_comp3 = { .id = 3, .name = "Comp-3", .on_msg = generic_on_msg };

/* 外部函数声明 (由 nos_ipc_p2p.c 提供) */
nos_status_t nos_ipc_init(nos_thread_t *thread, const char *uds_path);

static void* scheduler_thread_entry(void *arg) {
    nos_scheduler_run_loop((nos_thread_t *)arg);
    return NULL;
}

int main() {
    printf("--- [Process A] Starting (Comp 1, 2, 3) ---\n");

    nos_thread_t main_thread;
    nos_scheduler_init_thread(&main_thread, 1, "ProcA-Worker");

    /* 注册本地组件和服务 */
    nos_scheduler_register_component(1, &g_comp1);
    nos_scheduler_register_component(1, &g_comp2);
    nos_scheduler_register_component(1, &g_comp3);
    nos_service_register_provider(101, &g_comp1); // 本地服务 101

    /* 注册远端服务 (由进程 B 提供) */
    nos_service_register_remote(204, "/tmp/nos_proc_B.sock"); // 远端服务 204

    /* 初始化 IPC 监听 */
    nos_ipc_init(&main_thread, "/tmp/nos_proc_A.sock");

    pthread_t tid;
    pthread_create(&tid, NULL, scheduler_thread_entry, &main_thread);

    sleep(2); // 等待进程 B 启动

    /* 模拟跨进程通信：Comp-1 发送给 Comp-4 (远端服务 204) */
    printf("[Process A] Comp-1 sending Cross-Process Req to Service 204...\n");
    size_t len = sizeof(nos_service_msg_t) + 32;
    nos_service_msg_t *msg = malloc(len);
    msg->dst_service = 204;
    msg->src_component = 1;
    msg->msg_code = 1001;
    msg->payload_len = 32;
    strcpy((char*)msg->payload, "Hello from ProcA Comp-1");
    nos_service_msg_send(msg);
    free(msg);

    while(1) sleep(10);
    return 0;
}
