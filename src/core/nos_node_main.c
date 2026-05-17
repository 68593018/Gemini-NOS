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

static loaded_comp_info_t g_loaded_info[MAX_COMPONENTS_PER_NODE];
static uint32_t g_loaded_count = 0;
static int g_keep_running = 1;

static void signal_handler(int sig) {
    printf("\n[Node] Received signal %d, initiating graceful shutdown...\n", sig);
    g_keep_running = 0;
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

    *out_handle = handle;
    printf("[Node] Successfully loaded dynamic component: %s (ID:%u) using Model Lib: %s\n", name, id, lib_name);
    return comp;
}

static nos_component_t* node_get_loaded_comp_by_id(uint32_t id) {
    for (uint32_t i = 0; i < g_loaded_count; i++) {
        if (g_loaded_info[i].comp->id == id) return g_loaded_info[i].comp;
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
            void *handle = NULL;
            nos_component_t *comp = node_load_component(t_def->comp_ids[j], t_def->comp_names[j], t_def->comp_models[j], &handle);
            if (comp) {
                if (g_loaded_count < MAX_COMPONENTS_PER_NODE) {
                    g_loaded_info[g_loaded_count].comp = comp;
                    g_loaded_info[g_loaded_count].handle = handle;
                    g_loaded_info[g_loaded_count].lib_name = t_def->comp_models[j];
                    g_loaded_info[g_loaded_count].owner_thread = &worker_threads[i];
                    g_loaded_count++;
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
 * @brief 运行时卸载组件
 */
static nos_status_t node_unload_component(const char *name) {
    printf("[Node] Unloading component: %s...\n", name);
    
    int idx = -1;
    for (uint32_t i = 0; i < g_loaded_count; i++) {
        if (g_loaded_info[i].comp && strcmp(g_loaded_info[i].comp->name, name) == 0) {
            idx = (int)i;
            break;
        }
    }
    if (idx == -1) {
        fprintf(stderr, "[Node] Error: Component %s not found for unloading.\n", name);
        return NOS_ERR;
    }

    loaded_comp_info_t *info = &g_loaded_info[idx];
    uint32_t id = info->comp->id;

    /* 1. 从调度器注销 */
    nos_scheduler_unregister_component(info->owner_thread, info->comp);

    /* 2. 注销关联的服务路由 (简化版) */
    nos_service_unregister_provider(101); 
    nos_service_unregister_provider(102);
    nos_service_unregister_provider(105);
    nos_service_unregister_provider(204);

    /* 3. 触发停止回调并清理 */
    if (info->comp->stop) info->comp->stop(info->comp);
    
    free((void*)info->comp->name);
    free(info->comp);
    dlclose(info->handle);

    /* 4. 从管理数组移除 (将后续元素前移) */
    for (uint32_t i = idx; i < g_loaded_count - 1; i++) {
        g_loaded_info[i] = g_loaded_info[i+1];
    }
    g_loaded_count--;

    printf("[Node] Component %s (ID:%u) has been completely unloaded.\n", name, id);
    return NOS_OK;
}

/**
 * @brief 运行时热替换组件
 */
static nos_status_t node_reload_component(const char *name) {
    printf("[Node] Hot-reloading component: %s...\n", name);
    
    /* 1. 先查找关键信息 (ID, Lib, Thread) */
    int idx = -1;
    for (uint32_t i = 0; i < g_loaded_count; i++) {
        if (strcmp(g_loaded_info[i].comp->name, name) == 0) {
            idx = (int)i;
            break;
        }
    }
    if (idx == -1) return NOS_ERR;

    uint32_t id = g_loaded_info[idx].comp->id;
    const char *lib_name = strdup(g_loaded_info[idx].lib_name); // 备份库名
    nos_thread_t *thread = g_loaded_info[idx].owner_thread;

    /* 2. 执行卸载 */
    node_unload_component(name);

    /* 3. 重新加载 */
    void *new_handle = NULL;
    nos_component_t *new_comp = node_load_component(id, name, lib_name, &new_handle);
    if (new_comp) {
        g_loaded_info[g_loaded_count].comp = new_comp;
        g_loaded_info[g_loaded_count].handle = new_handle;
        g_loaded_info[g_loaded_count].lib_name = lib_name; // lib_name 此时已经是指向 strdup 的内存了
        g_loaded_info[g_loaded_count].owner_thread = thread;
        g_loaded_count++;

        if (new_comp->start) new_comp->start(new_comp);
        nos_scheduler_register_component(thread, new_comp);
        nos_service_register_provider_bind(id == 1 ? 101 : (id == 2 ? 102 : (id == 5 ? 105 : 204)), new_comp, thread);
        printf("[Node] Hot-reload complete: %s is back online.\n", name);
    } else {
        free((void*)lib_name);
    }

    return (new_comp != NULL) ? NOS_OK : NOS_ERR;
}

/**
 * @brief 隔离性测试触发逻辑
 */
static void node_run_test_trigger(const char *node_name) {
    if (strcmp(node_name, "ProcA") == 0) {
        sleep(2);
        printf("[Node] %s triggering integration test...\n", node_name);
        
        /* 1. 发送测试 */
        nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + 32, 0);
        if (buf) {
            nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
            msg->magic = NOS_IPC_MAGIC; msg->dst_service = SVC_ROUTING_V4;
            msg->msg_code = 1001;
            sprintf((char*)(buf->data + sizeof(nos_service_msg_t)), "Ping Before Reload");
            nos_service_msg_send(buf);
        }

        sleep(1);
        /* 2. 在线热替换 Comp-2 */
        node_reload_component("Comp-2");

        sleep(1);
        /* 3. 替换后再发，确认新实例接管 */
        buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + 32, 0);
        if (buf) {
            nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
            msg->magic = NOS_IPC_MAGIC; msg->dst_service = SVC_ROUTING_V4;
            msg->msg_code = 1001;
            sprintf((char*)(buf->data + sizeof(nos_service_msg_t)), "Ping After Reload");
            nos_service_msg_send(buf);
        }

        sleep(1);
        /* 4. 演示：彻底卸载 Comp-3 */
        node_unload_component("Comp-3");
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
        if (g_loaded_info[i].comp->start) {
            g_loaded_info[i].comp->start(g_loaded_info[i].comp);
            printf("[Node] Started component: %s\n", g_loaded_info[i].comp->name);
        }
    }

    /* 增加信号捕获用于优雅退出 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 5. 物理线程启动 */
    pthread_t mgmt_tid;
    pthread_create(&mgmt_tid, NULL, scheduler_thread_entry, mgmt_thread);

    pthread_t worker_tids[MAX_WORKERS];
    for (int i = 0; i < worker_count; i++) {
        pthread_create(&worker_tids[i], NULL, scheduler_thread_entry, &worker_threads[i]);
    }

    /* 5. 运行测试逻辑 */
    node_run_test_trigger(node_name);

    /* 6. 等待退出或信号中止 */
    while (g_keep_running) {
        sleep(1);
    }

    printf("[Node] Shutting down...\n");

    /* 7. 通知所有调度器线程停止 */
    nos_scheduler_stop(mgmt_thread);
    for (int i = 0; i < worker_count; i++) {
        nos_scheduler_stop(&worker_threads[i]);
    }

    /* 8. 等待线程真正退出 (Graceful Join) */
    pthread_join(mgmt_tid, NULL);
    for (int i = 0; i < worker_count; i++) {
        pthread_join(worker_tids[i], NULL);
    }

    /* 9. 按加载相反顺序停止组件并清理资源 */
    for (int i = (int)g_loaded_count - 1; i >= 0; i--) {
        if (g_loaded_info[i].comp->stop) {
            g_loaded_info[i].comp->stop(g_loaded_info[i].comp);
        }
        printf("[Node] Stopped component: %s\n", g_loaded_info[i].comp->name);
        
        free((void*)g_loaded_info[i].comp->name);
        free(g_loaded_info[i].comp);
        dlclose(g_loaded_info[i].handle);
    }

    /* 10. 清理物理线程管理结构 */
    free(mgmt_thread);
    free(worker_threads);

    printf("[Node] Shutdown complete.\n");
    return 0;
}
