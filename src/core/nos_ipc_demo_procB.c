#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_scheduler.h"

/* 模拟组件 4, 5 */
static void generic_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    printf("[%s] RECEIVED: From Component %u, MsgCode %u, Payload: %s\n", 
           self->name, msg->src_component, msg->msg_code, (char*)msg->payload);

    /* 收到 1001 请求后，自动回包给进程 A 的服务 101 */
    if (msg->msg_code == 1001) {
        printf("[%s] Replying to Remote Service 101...\n", self->name);
        size_t len = sizeof(nos_service_msg_t) + 32;
        nos_service_msg_t *rsp = malloc(len);
        rsp->dst_service = 101; // 回给 ProcA 的服务 101
        rsp->src_component = self->id;
        rsp->msg_code = 1002; // 响应码
        rsp->payload_len = 32;
        sprintf((char*)rsp->payload, "Reply from ProcB %s", self->name);
        nos_service_msg_send(rsp);
        free(rsp);
    }
}

static nos_component_t g_comp4 = { .id = 4, .name = "Comp-4", .on_msg = generic_on_msg };
static nos_component_t g_comp5 = { .id = 5, .name = "Comp-5", .on_msg = generic_on_msg };

nos_status_t nos_ipc_init(nos_thread_t *thread, const char *uds_path);

static void* scheduler_thread_entry(void *arg) {
    nos_scheduler_run_loop((nos_thread_t *)arg);
    return NULL;
}

int main() {
    printf("--- [Process B] Starting (Comp 4, 5) ---\n");

    nos_thread_t main_thread;
    nos_scheduler_init_thread(&main_thread, 2, "ProcB-Worker");

    nos_scheduler_register_component(2, &g_comp4);
    nos_scheduler_register_component(2, &g_comp5);
    nos_service_register_provider(204, &g_comp4); // 本地提供服务 204

    /* 注册远端服务 (由进程 A 提供) */
    nos_service_register_remote(101, "/tmp/nos_proc_A.sock");

    nos_ipc_init(&main_thread, "/tmp/nos_proc_B.sock");

    pthread_t tid;
    pthread_create(&tid, NULL, scheduler_thread_entry, &main_thread);

    while(1) sleep(10);
    return 0;
}
