#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <ctype.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_scheduler.h"
#include "nos_manifest.h"
#include "nos_buffer.h"
#include "nos_ids.h"

#define MAX_WORKERS 8
#define MAX_COMPONENTS_PER_NODE 64

/* 全局组件注册表 (动态加载后存放在此) */
static nos_component_t* g_loaded_components[MAX_COMPONENTS_PER_NODE];
static uint32_t g_loaded_count = 0;

/* 外部函数声明 (由 nos_ipc_p2p.c 提供) */
nos_status_t nos_ipc_init(nos_thread_t *thread, const char *uds_path);

/* 外部函数声明 (由 nos_scheduler.c 内部提供) */
nos_status_t nos_service_register_provider_bind(uint32_t service_id, nos_component_t *provider, nos_thread_t *thread);

/**
 * @brief 动态加载组件 .so
 */
static nos_component_t* node_load_component(uint32_t id, const char *name, const char *lib_name) {
    char lib_path[256];
    
    /* 构造库路径，直接使用清单中定义的 lib_name */
    sprintf(lib_path, "./%s", lib_name);

    void *handle = dlopen(lib_path, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "[Node] Failed to load %s: %s\n", lib_path, dlerror());
        return NULL;
    }

    nos_comp_export_func_t export_func = (nos_comp_export_func_t)dlsym(handle, "nos_export_component");
    if (!export_func) {
        fprintf(stderr, "[Node] Symbol 'nos_export_component' not found in %s\n", lib_path);
        dlclose(handle);
        return NULL;
    }

    nos_component_t *comp = calloc(1, sizeof(nos_component_t));
    comp->id = id;
    comp->name = strdup(name);

    if (export_func(comp) != NOS_OK) {
        fprintf(stderr, "[Node] Component %s export failed\n", name);
        free((void*)comp->name);
        free(comp);
        dlclose(handle);
        return NULL;
    }

    /* 触发初始化生命周期 */
    if (comp->init && comp->init(comp) != NOS_OK) {
        fprintf(stderr, "[Node] Component %s init failed\n", name);
        free((void*)comp->name);
        free(comp);
        dlclose(handle);
        return NULL;
    }

    printf("[Node] Successfully loaded dynamic component: %s (ID:%u) using Model Lib: %s\n", name, id, lib_name);
    return comp;
}

static nos_component_t* node_get_loaded_comp_by_id(uint32_t id) {
    for (uint32_t i = 0; i < g_loaded_count; i++) {
        if (g_loaded_components[i]->id == id) return g_loaded_components[i];
    }
    return NULL;
}

static void* scheduler_thread_entry(void *arg) {
    nos_scheduler_run_loop((nos_thread_t *)arg);
    return NULL;
}

/**
 * @brief 初始化管理线程 (控制面)
 */
static nos_thread_t* node_init_mgmt(const char *uds_path) {
    nos_thread_t *mgmt_thread = malloc(sizeof(nos_thread_t));
    nos_scheduler_init_thread(mgmt_thread, 999, "Mgmt-Master");
    
    /* 将 IPC 监听绑定到管理线程 */
    nos_ipc_init(mgmt_thread, uds_path);
    printf("[Node] ControlPlane: IPC Listening on %s\n", uds_path);
    return mgmt_thread;
}

/**
 * @brief 根据清单初始化并启动工作线程池 (数据面)
 */
static int node_init_workers(const nos_node_def_t *node_def, nos_thread_t *worker_threads) {
    int worker_count = 0;
    for (int i = 0; node_def->threads[i].name != NULL && i < MAX_WORKERS; i++) {
        const nos_thread_def_t *t_def = &node_def->threads[i];
        nos_scheduler_init_thread(&worker_threads[i], i, t_def->name);
        
        /* 将组件加载到具体的工作线程 */
        for (int j = 0; t_def->comp_ids[j] != 0; j++) {
            /* 动态加载组件 (基于模型库名) */
            nos_component_t *comp = node_load_component(t_def->comp_ids[j], t_def->comp_names[j], t_def->comp_models[j]);
            if (comp) {
                if (g_loaded_count < MAX_COMPONENTS_PER_NODE) {
                    g_loaded_components[g_loaded_count++] = comp;
                }
                nos_scheduler_register_component(&worker_threads[i], comp);
                printf("[Node] DataPlane: Loaded %s onto %s\n", comp->name, t_def->name);
            }
        }
        worker_count++;
    }
    return worker_count;
}

/**
 * @brief 自动化路由注册 (建立本地服务与 Worker 的绑定关系，或注册远端路由)
 */
static void node_setup_routing(const char *current_node_name, nos_thread_t *worker_threads, int worker_count) {
    uint32_t svc_count = 0;
    const nos_service_def_t *svc_list = nos_manifest_get_services(&svc_count);
    
    for (uint32_t i = 0; i < svc_count; i++) {
        const nos_service_def_t *svc = &svc_list[i];
        if (strcmp(svc->node_name, current_node_name) == 0) {
            /* 本地服务路由：定位 Provider 所在的线程 */
            nos_component_t *provider = node_get_loaded_comp_by_id(svc->provider_comp_id);
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
                printf("[Node] Routing: Local Service %u -> %s (on %s)\n", 
                       svc->service_id, provider->name, owner_worker->name);
            }
        } else {
            /* 远端服务路由注册 */
            const nos_node_def_t *remote_node = nos_manifest_get_node(svc->node_name);
            if (remote_node) {
                nos_service_register_remote(svc->service_id, remote_node->uds_path);
                printf("[Node] Routing: Remote Service %u -> %s\n", 
                       svc->service_id, remote_node->name);
            }
        }
    }
}

/**
 * @brief 隔离性测试触发逻辑
 */
static void node_run_test_trigger(const char *node_name) {
    if (strcmp(node_name, "ProcA") == 0) {
        sleep(2);
        printf("[Node] %s triggering integration test (Local: %u/%u, Remote: %u)...\n", 
               node_name, SVC_ROUTING_V4, SVC_ROUTING_V6, SVC_DATA_PROC);
        
        uint32_t targets[] = {SVC_ROUTING_V4, SVC_ROUTING_V6, SVC_DATA_PROC, SVC_ROUTING_V4}; 
        for (int i = 0; i < 4; i++) {
            nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + 32, 0);
            if (buf) {
                nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
                msg->magic = NOS_IPC_MAGIC;
                msg->version = NOS_IPC_VERSION;
                msg->dst_service = targets[i];
                msg->src_component = 999;
                msg->msg_code = 1001;
                msg->payload_len = 32;
                sprintf((char*)(buf->data + sizeof(nos_service_msg_t)), "Ping target %u", targets[i]);
                nos_service_msg_send(buf);
            }
        }
    }
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
    nos_buffer_init_pool(node_def->buffer_pools);
    nos_buffer_dump_stats();

    /* 1. 初始化管理线程 */
    nos_thread_t *mgmt_thread = node_init_mgmt(node_def->uds_path);

    /* 2. 初始化工作线程 */
    nos_thread_t *worker_threads = malloc(sizeof(nos_thread_t) * MAX_WORKERS);
    int worker_count = node_init_workers(node_def, worker_threads);

    /* 3. 配置路由 */
    node_setup_routing(node_name, worker_threads, worker_count);

    /* 4. 触发启动生命周期 */
    for (uint32_t i = 0; i < g_loaded_count; i++) {
        if (g_loaded_components[i]->start) {
            g_loaded_components[i]->start(g_loaded_components[i]);
            printf("[Node] Started component: %s\n", g_loaded_components[i]->name);
        }
    }

    /* 5. 物理线程启动 */
    pthread_t mgmt_tid;
    pthread_create(&mgmt_tid, NULL, scheduler_thread_entry, mgmt_thread);

    pthread_t worker_tids[MAX_WORKERS];
    for (int i = 0; i < worker_count; i++) {
        pthread_create(&worker_tids[i], NULL, scheduler_thread_entry, &worker_threads[i]);
    }

    /* 5. 运行测试逻辑 */
    node_run_test_trigger(node_name);

    /* 6. 等待退出 */
    pthread_join(mgmt_tid, NULL);
    for (int i = 0; i < worker_count; i++) pthread_join(worker_tids[i], NULL);

    return 0;
}
