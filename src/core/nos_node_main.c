#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <ctype.h>
#include <signal.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_scheduler.h"
#include "nos_manifest.h"
#include "nos_buffer.h"
#include "nos_ids.h"

#define MAX_WORKERS 8
#define MAX_COMPONENTS_PER_NODE 64

/* 管理已加载组件及其动态库句柄 */
typedef struct {
    nos_component_t *comp;
    void *handle;
    const char *lib_name;
    nos_thread_t *owner_thread;
} loaded_comp_info_t;

/**
 * @brief 节点全局上下文收敛结构体
 */
typedef struct {
    loaded_comp_info_t loaded_info[MAX_COMPONENTS_PER_NODE];
    uint32_t loaded_count;
    volatile int keep_running;
    
    /* 物理线程与管理对象 */
    nos_thread_t *mgmt_thread;
    nos_thread_t *worker_threads;
    uint32_t worker_count;
    
    pthread_t mgmt_tid;
    pthread_t worker_tids[MAX_WORKERS];
} nos_node_ctx_t;

static nos_node_ctx_t g_node_ctx = {
    .keep_running = 1,
    .loaded_count = 0,
    .worker_count = 0
};

static void signal_handler(int sig) {
    printf("\n[Node] Received signal %d, initiating graceful shutdown...\n", sig);
    g_node_ctx.keep_running = 0;
}

/* 外部函数声明 (由 nos_ipc_p2p.c 提供) */
nos_status_t nos_ipc_init(nos_thread_t *thread, const char *uds_path);

/* 外部函数声明 (由 nos_scheduler.c 内部提供) */
nos_status_t nos_service_register_provider_bind(uint32_t service_id, nos_component_t *provider, nos_thread_t *thread);

/**
 * @brief 动态加载组件 .so
 */
static nos_component_t* node_load_component(uint32_t id, const char *name, const char *lib_name, void **out_handle) {
    char lib_path[256];
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

    if (comp->init && comp->init(comp) != NOS_OK) {
        fprintf(stderr, "[Node] Component %s init failed\n", name);
        free((void*)comp->name);
        free(comp);
        dlclose(handle);
        return NULL;
    }

    *out_handle = handle;
    printf("[Node] Successfully loaded dynamic component: %s (ID:%u) using Model Lib: %s\n", name, id, lib_name);
    return comp;
}

static nos_component_t* node_get_loaded_comp_by_id(uint32_t id) {
    for (uint32_t i = 0; i < g_node_ctx.loaded_count; i++) {
        if (g_node_ctx.loaded_info[i].comp->id == id) return g_node_ctx.loaded_info[i].comp;
    }
    return NULL;
}

static void* scheduler_thread_entry(void *arg) {
    nos_scheduler_run_loop((nos_thread_t *)arg);
    return NULL;
}

static void node_init_mgmt(const char *uds_path) {
    g_node_ctx.mgmt_thread = malloc(sizeof(nos_thread_t));
    nos_scheduler_init_thread(g_node_ctx.mgmt_thread, 999, "Mgmt-Master");
    nos_ipc_init(g_node_ctx.mgmt_thread, uds_path);
    printf("[Node] ControlPlane: IPC Listening on %s\n", uds_path);
}

static void node_init_workers(const nos_node_def_t *node_def) {
    g_node_ctx.worker_threads = malloc(sizeof(nos_thread_t) * MAX_WORKERS);
    g_node_ctx.worker_count = 0;
    
    for (int i = 0; node_def->threads[i].name != NULL && i < MAX_WORKERS; i++) {
        const nos_thread_def_t *t_def = &node_def->threads[i];
        nos_scheduler_init_thread(&g_node_ctx.worker_threads[i], i, t_def->name);
        
        for (int j = 0; t_def->comp_ids[j] != 0; j++) {
            void *handle = NULL;
            nos_component_t *comp = node_load_component(t_def->comp_ids[j], t_def->comp_names[j], t_def->comp_models[j], &handle);
            if (comp) {
                if (g_node_ctx.loaded_count < MAX_COMPONENTS_PER_NODE) {
                    loaded_comp_info_t *info = &g_node_ctx.loaded_info[g_node_ctx.loaded_count++];
                    info->comp = comp;
                    info->handle = handle;
                    info->lib_name = t_def->comp_models[j];
                    info->owner_thread = &g_node_ctx.worker_threads[i];
                }
                nos_scheduler_register_component(&g_node_ctx.worker_threads[i], comp);
                printf("[Node] DataPlane: Loaded %s onto %s\n", comp->name, t_def->name);
            }
        }
        g_node_ctx.worker_count++;
    }
}

static void node_setup_routing(const char *current_node_name) {
    uint32_t svc_count = 0;
    const nos_service_def_t *svc_list = nos_manifest_get_services(&svc_count);
    
    for (uint32_t i = 0; i < svc_count; i++) {
        const nos_service_def_t *svc = &svc_list[i];
        if (strcmp(svc->node_name, current_node_name) == 0) {
            nos_component_t *provider = node_get_loaded_comp_by_id(svc->provider_comp_id);
            nos_thread_t *owner_worker = NULL;
            for (uint32_t t = 0; t < g_node_ctx.worker_count; t++) {
                for (uint32_t c = 0; c < g_node_ctx.worker_threads[t].component_count; c++) {
                    if (g_node_ctx.worker_threads[t].components[c]->id == svc->provider_comp_id) {
                        owner_worker = &g_node_ctx.worker_threads[t];
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
            const nos_node_def_t *remote_node = nos_manifest_get_node(svc->node_name);
            if (remote_node) {
                nos_service_register_remote(svc->service_id, remote_node->uds_path);
                printf("[Node] Routing: Remote Service %u -> %s\n", 
                       svc->service_id, remote_node->name);
            }
        }
    }
}

static nos_status_t node_unload_component(const char *name) {
    printf("[Node] Unloading component: %s...\n", name);
    int idx = -1;
    for (uint32_t i = 0; i < g_node_ctx.loaded_count; i++) {
        if (g_node_ctx.loaded_info[i].comp && strcmp(g_node_ctx.loaded_info[i].comp->name, name) == 0) {
            idx = (int)i; break;
        }
    }
    if (idx == -1) return NOS_ERR;

    loaded_comp_info_t *info = &g_node_ctx.loaded_info[idx];
    nos_scheduler_unregister_component(info->owner_thread, info->comp);
    
    /* 简化：此处应遍历该组件提供的所有服务并注销 */
    nos_service_unregister_provider(101); nos_service_unregister_provider(102);
    nos_service_unregister_provider(105); nos_service_unregister_provider(204);

    if (info->comp->stop) info->comp->stop(info->comp);
    free((void*)info->comp->name); free(info->comp); dlclose(info->handle);

    for (uint32_t i = idx; i < g_node_ctx.loaded_count - 1; i++) {
        g_node_ctx.loaded_info[i] = g_node_ctx.loaded_info[i+1];
    }
    g_node_ctx.loaded_count--;
    return NOS_OK;
}

static nos_status_t node_reload_component(const char *name) {
    int idx = -1;
    for (uint32_t i = 0; i < g_node_ctx.loaded_count; i++) {
        if (strcmp(g_node_ctx.loaded_info[i].comp->name, name) == 0) {
            idx = (int)i; break;
        }
    }
    if (idx == -1) return NOS_ERR;

    uint32_t id = g_node_ctx.loaded_info[idx].comp->id;
    const char *lib_name = strdup(g_node_ctx.loaded_info[idx].lib_name);
    nos_thread_t *thread = g_node_ctx.loaded_info[idx].owner_thread;

    node_unload_component(name);

    void *new_handle = NULL;
    nos_component_t *new_comp = node_load_component(id, name, lib_name, &new_handle);
    if (new_comp) {
        loaded_comp_info_t *info = &g_node_ctx.loaded_info[g_node_ctx.loaded_count++];
        info->comp = new_comp; info->handle = new_handle; info->lib_name = lib_name; info->owner_thread = thread;
        if (new_comp->start) new_comp->start(new_comp);
        nos_scheduler_register_component(thread, new_comp);
        nos_service_register_provider_bind(id == 1 ? 101 : (id == 2 ? 102 : (id == 5 ? 105 : 204)), new_comp, thread);
        printf("[Node] Hot-reload complete: %s back online.\n", name);
    } else {
        free((void*)lib_name);
    }
    return (new_comp != NULL) ? NOS_OK : NOS_ERR;
}

static void node_run_test_trigger(const char *node_name) {
    if (strcmp(node_name, "ProcA") == 0) {
        sleep(2);
        printf("[Node] %s triggering integration test...\n", node_name);
        nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + 32, 0);
        if (buf) {
            nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
            msg->magic = NOS_IPC_MAGIC; msg->dst_service = SVC_ROUTING_V4;
            msg->msg_code = 1001;
            sprintf((char*)(buf->data + sizeof(nos_service_msg_t)), "Ping Before Reload");
            nos_service_msg_send(buf);
        }
        sleep(1);
        node_reload_component("Comp-2");
        sleep(1);
        buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + 32, 0);
        if (buf) {
            nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
            msg->magic = NOS_IPC_MAGIC; msg->dst_service = SVC_ROUTING_V4;
            msg->msg_code = 1001;
            sprintf((char*)(buf->data + sizeof(nos_service_msg_t)), "Ping After Reload");
            nos_service_msg_send(buf);
        }
        sleep(1);
        node_unload_component("Comp-3");
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: %s <node_name>\n", argv[0]); return -1; }

    const char *node_name = argv[1];
    const nos_node_def_t *node_def = nos_manifest_get_node(node_name);
    if (!node_def) { printf("Error: Node '%s' not found.\n", node_name); return -1; }

    printf("--- [NOS Node: %s] Starting ---\n", node_name);
    nos_buffer_init_pool(node_def->buffer_pools);
    nos_buffer_dump_stats();

    node_init_mgmt(node_def->uds_path);
    node_init_workers(node_def);
    node_setup_routing(node_name);

    for (uint32_t i = 0; i < g_node_ctx.loaded_count; i++) {
        if (g_node_ctx.loaded_info[i].comp->start) g_node_ctx.loaded_info[i].comp->start(g_node_ctx.loaded_info[i].comp);
        printf("[Node] Started component: %s\n", g_node_ctx.loaded_info[i].comp->name);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    pthread_create(&g_node_ctx.mgmt_tid, NULL, scheduler_thread_entry, g_node_ctx.mgmt_thread);
    for (uint32_t i = 0; i < g_node_ctx.worker_count; i++) {
        pthread_create(&g_node_ctx.worker_tids[i], NULL, scheduler_thread_entry, &g_node_ctx.worker_threads[i]);
    }

    node_run_test_trigger(node_name);

    while (g_node_ctx.keep_running) sleep(1);

    printf("[Node] Shutting down...\n");
    nos_scheduler_stop(g_node_ctx.mgmt_thread);
    for (uint32_t i = 0; i < g_node_ctx.worker_count; i++) nos_scheduler_stop(&g_node_ctx.worker_threads[i]);

    pthread_join(g_node_ctx.mgmt_tid, NULL);
    for (uint32_t i = 0; i < g_node_ctx.worker_count; i++) pthread_join(g_node_ctx.worker_tids[i], NULL);

    for (int i = (int)g_node_ctx.loaded_count - 1; i >= 0; i--) {
        if (g_node_ctx.loaded_info[i].comp->stop) g_node_ctx.loaded_info[i].comp->stop(g_node_ctx.loaded_info[i].comp);
        printf("[Node] Stopped component: %s\n", g_node_ctx.loaded_info[i].comp->name);
        free((void*)g_node_ctx.loaded_info[i].comp->name); free(g_node_ctx.loaded_info[i].comp); dlclose(g_node_ctx.loaded_info[i].handle);
    }
    free(g_node_ctx.mgmt_thread); free(g_node_ctx.worker_threads);
    printf("[Node] Shutdown complete.\n");
    return 0;
}
