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

    printf("--- [NOS Node: %s] Master-Worker Architecture Starting ---\n", node_name);

    /* 1. 【控制面】初始化专属管理线程 (Master) */
    nos_thread_t *mgmt_thread = malloc(sizeof(nos_thread_t));
    nos_scheduler_init_thread(mgmt_thread, 999, "Mgmt-Master");

    /* 2. 【数据面】根据清单初始化并启动工作线程池 (Workers) */
    nos_thread_t *worker_threads = malloc(sizeof(nos_thread_t) * 8);
    int worker_count = 0;

    for (int i = 0; node_def->threads[i].name != NULL; i++) {
        const nos_thread_def_t *t_def = &node_def->threads[i];
        nos_scheduler_init_thread(&worker_threads[i], i, t_def->name);
        
        /* 将组件加载到具体的工作线程 */
        for (int j = 0; t_def->comp_ids[j] != 0; j++) {
            nos_component_t *comp = nos_get_component_by_id(t_def->comp_ids[j]);
            if (comp) {
                nos_scheduler_register_component(&worker_threads[i], comp);
                printf("[Node] DataPlane: Loaded %s onto %s\n", comp->name, t_def->name);
            }
        }
        worker_count++;
    }

    /* 3. 自动化路由注册 (Master 与 Workers 联动) */
    uint32_t svc_count = 0;
    const nos_service_def_t *svc_list = nos_manifest_get_services(&svc_count);
    for (uint32_t i = 0; i < svc_count; i++) {
        const nos_service_def_t *svc = &svc_list[i];
        if (strcmp(svc->node_name, node_name) == 0) {
            /* 本地服务：确定该组件位于哪个 Worker 线程 */
            nos_component_t *provider = nos_get_component_by_id(svc->provider_comp_id);
            nos_thread_t *owner_worker = NULL;
            for (int t = 0; t < worker_count; t++) {
                for (uint32_t c = 0; c < worker_threads[t].component_count; c++) {
                    if (worker_threads[t].components[c]->id == svc->provider_comp_id) {
                        owner_worker = &worker_threads[t];
                        break;
                    }
                }
                if (owner_worker) break;
            }
            if (provider && owner_worker) {
                nos_service_register_provider_bind(svc->service_id, provider, owner_worker);
                printf("[Node] Routing: Service %u -> %s (on %s)\n", 
                       svc->service_id, provider->name, owner_worker->name);
            }
        } else {
            /* 远端服务注册路由 */
            const nos_node_def_t *remote_node = nos_manifest_get_node(svc->node_name);
            if (remote_node) {
                nos_service_register_remote(svc->service_id, remote_node->uds_path);
                printf("[Node] Routing: Remote Service %u -> %s\n", 
                       svc->service_id, remote_node->name);
            }
        }
    }

    /* 4. 将 IPC 监听强绑定到管理线程 (控制面接收 IO) */
    nos_ipc_init(mgmt_thread, node_def->uds_path);
    printf("[Node] ControlPlane: IPC Listening on %s\n", node_def->uds_path);

    /* 5. 启动所有物理线程 */
    pthread_t mgmt_tid;
    pthread_create(&mgmt_tid, NULL, scheduler_thread_entry, mgmt_thread);

    pthread_t worker_tids[8];
    for (int i = 0; i < worker_count; i++) {
        pthread_create(&worker_tids[i], NULL, scheduler_thread_entry, &worker_threads[i]);
    }

    /* 6. 模拟业务触发 (测试用) */
    if (strcmp(node_name, "ProcA") == 0) {
        sleep(2);
        printf("[Node] ProcA triggering Master-Worker initial test...\n");
        nos_buffer_t *buf = nos_buffer_alloc();
        if (buf) {
            nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
            msg->magic = NOS_IPC_MAGIC;
            msg->version = NOS_IPC_VERSION;
            msg->dst_service = 204;
            msg->src_component = 1;
            msg->msg_code = 1001;
            msg->payload_len = 32;
            strcpy((char*)(buf->data + sizeof(nos_service_msg_t)), "Master-Worker Message");
            nos_service_msg_send(buf);
        }
    }

    pthread_join(mgmt_tid, NULL);
    for (int i = 0; i < worker_count; i++) pthread_join(worker_tids[i], NULL);
    return 0;
}
