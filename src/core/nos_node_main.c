#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_scheduler.h"
#include "nos_manifest.h"
#include "nos_buffer.h"

/* 外部函数声明 (由 nos_ipc_p2p.c 提供) */
nos_status_t nos_ipc_init(nos_thread_t *thread, const char *uds_path);

/* 外部函数声明 (由 nos_scheduler.c 内部提供) */
nos_status_t nos_service_register_provider_bind(uint32_t service_id, nos_component_t *provider, nos_thread_t *thread);
nos_component_t* nos_get_component_by_id(uint32_t id);

static void* scheduler_thread_entry(void *arg) {
    nos_scheduler_run_loop((nos_thread_t *)arg);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <node_name>\n", argv[0]);
        return -1;
    }

    nos_buffer_init_pool();

    const char *node_name = argv[1];
    const nos_node_def_t *node_def = nos_manifest_get_node(node_name);
    if (!node_def) {
        printf("Error: Node '%s' not found in manifest.\n", node_name);
        return -1;
    }

    printf("--- [NOS Node: %s] Multi-threaded Starting ---\n", node_name);

    /* 1. 初始化并启动清单定义的线程 */
    nos_thread_t *threads = malloc(sizeof(nos_thread_t) * 8);
    int thread_count = 0;

    for (int i = 0; node_def->threads[i].name != NULL; i++) {
        const nos_thread_def_t *t_def = &node_def->threads[i];
        nos_scheduler_init_thread(&threads[i], i, t_def->name);
        
        /* 加载挂载到该线程的组件 */
        for (int j = 0; t_def->comp_ids[j] != 0; j++) {
            nos_component_t *comp = nos_get_component_by_id(t_def->comp_ids[j]);
            if (comp) {
                nos_scheduler_register_component(&threads[i], comp);
                printf("[Node] Loaded %s onto %s\n", comp->name, t_def->name);
            }
        }
        thread_count++;
    }

    /* 2. 自动化路由注册 (带线程绑定) */
    uint32_t svc_count = 0;
    const nos_service_def_t *svc_list = nos_manifest_get_services(&svc_count);
    for (uint32_t i = 0; i < svc_count; i++) {
        const nos_service_def_t *svc = &svc_list[i];
        if (strcmp(svc->node_name, node_name) == 0) {
            /* 本地服务：需要找到对应的线程 */
            nos_component_t *provider = nos_get_component_by_id(svc->provider_comp_id);
            nos_thread_t *target_t = NULL;
            // 查表确认组件在哪个线程
            for (int t = 0; t < thread_count; t++) {
                for (uint32_t c = 0; c < threads[t].component_count; c++) {
                    if (threads[t].components[c]->id == svc->provider_comp_id) {
                        target_t = &threads[t];
                        break;
                    }
                }
                if (target_t) break;
            }
            if (provider && target_t) {
                nos_service_register_provider_bind(svc->service_id, provider, target_t);
                printf("[Node] Registered Local Service: %u -> %s (on %s)\n", 
                       svc->service_id, provider->name, target_t->name);
            }
        } else {
            /* 远端服务注册路由 */
            const nos_node_def_t *remote_node = nos_manifest_get_node(svc->node_name);
            if (remote_node) {
                nos_service_register_remote(svc->service_id, remote_node->uds_path);
                printf("[Node] Registered Remote Route: Service %u -> %s\n", 
                       svc->service_id, remote_node->name);
            }
        }
    }

    /* 3. 初始化 IPC 监听 (默认挂在 0 号线程) */
    nos_ipc_init(&threads[0], node_def->uds_path);

    /* 4. 真正启动所有线程 */
    pthread_t tids[8];
    for (int i = 0; i < thread_count; i++) {
        pthread_create(&tids[i], NULL, scheduler_thread_entry, &threads[i]);
    }

    /* 5. 特殊逻辑：触发交互 */
    if (strcmp(node_name, "ProcA") == 0) {
        sleep(2);
        printf("[Node] ProcA triggering cross-thread interaction...\n");
        nos_buffer_t *buf = nos_buffer_alloc();
        if (buf) {
            nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
            msg->magic = NOS_IPC_MAGIC;
            msg->version = NOS_IPC_VERSION;
            msg->dst_service = 204; /* 远端请求 */
            msg->src_component = 1;
            msg->msg_code = 1001;
            msg->payload_len = 32;
            strcpy((char*)(buf->data + sizeof(nos_service_msg_t)), "Multi-thread Req");
            nos_service_msg_send(buf);
        }
    }

    for (int i = 0; i < thread_count; i++) {
        pthread_join(tids[i], NULL);
    }
    return 0;
}
