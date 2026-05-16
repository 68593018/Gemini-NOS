#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_scheduler.h"
#include "nos_manifest.h"

/* 外部函数声明 */
nos_status_t nos_ipc_init(nos_thread_t *thread, const char *uds_path);
nos_component_t* nos_get_component_by_id(uint32_t id);
nos_status_t nos_scheduler_register_component(uint32_t thread_id, nos_component_t *comp);
void nos_scheduler_run_loop(nos_thread_t *self);

static void* scheduler_thread_entry(void *arg) {
    nos_scheduler_run_loop((nos_thread_t *)arg);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <node_name>\n", argv[0]);
        return -1;
    }

    const char *node_name = argv[1];
    const nos_node_def_t *node_def = nos_manifest_get_node(node_name);
    if (!node_def) {
        printf("Error: Node '%s' not found in manifest.\n", node_name);
        return -1;
    }

    printf("--- [NOS Node: %s] Starting ---\n", node_name);

    /* 1. 初始化调度器 */
    nos_thread_t main_thread;
    nos_scheduler_init_thread(&main_thread, 1, "MainWorker");

    /* 2. 加载本地组件 */
    for (int i = 0; node_def->comp_ids[i] != 0; i++) {
        nos_component_t *comp = nos_get_component_by_id(node_def->comp_ids[i]);
        if (comp) {
            nos_scheduler_register_component(1, comp);
            printf("[Node] Loaded Component: %s (ID: %u)\n", comp->name, comp->id);
        }
    }

    /* 3. 自动化路由注册 */
    uint32_t svc_count = 0;
    const nos_service_def_t *svc_list = nos_manifest_get_services(&svc_count);
    for (uint32_t i = 0; i < svc_count; i++) {
        const nos_service_def_t *svc = &svc_list[i];
        if (strcmp(svc->node_name, node_name) == 0) {
            /* 本地服务：绑定到对应的本地组件 */
            nos_component_t *provider = nos_get_component_by_id(svc->provider_comp_id);
            if (provider) {
                nos_service_register_provider(svc->service_id, provider);
                printf("[Node] Registered Local Service: %u (Provider: %s)\n", 
                       svc->service_id, provider->name);
            }
        } else {
            /* 远端服务：注册路由 */
            const nos_node_def_t *remote_node = nos_manifest_get_node(svc->node_name);
            if (remote_node) {
                nos_service_register_remote(svc->service_id, remote_node->uds_path);
                printf("[Node] Registered Remote Route: Service %u -> %s (%s)\n", 
                       svc->service_id, remote_node->name, remote_node->uds_path);
            }
        }
    }

    /* 4. 初始化 IPC 监听 */
    nos_ipc_init(&main_thread, node_def->uds_path);

    /* 5. 启动调度线程 */
    pthread_t tid;
    pthread_create(&tid, NULL, scheduler_thread_entry, &main_thread);

    /* 6. 特殊逻辑：如果是 ProcA，模拟发起一次测试请求 */
    if (strcmp(node_name, "ProcA") == 0) {
        sleep(2);
        printf("[Node] ProcA triggering initial test request to Service 204...\n");
        size_t len = sizeof(nos_service_msg_t) + 32;
        nos_service_msg_t *msg = malloc(len);
        msg->magic = NOS_IPC_MAGIC;
        msg->version = NOS_IPC_VERSION;
        msg->dst_service = 204;
        msg->src_component = 1;
        msg->msg_code = 1001;
        msg->payload_len = 32;
        strcpy((char*)msg->payload, "Meta-driven Req");
        nos_service_msg_send(msg);
        free(msg);
    }

    while(1) sleep(10);
    return 0;
}
