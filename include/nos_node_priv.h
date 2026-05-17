#ifndef __NOS_NODE_PRIV_H__
#define __NOS_NODE_PRIV_H__

#include <pthread.h>
#include "nos_types.h"
#include "nos_component.h"
#include "nos_scheduler.h"

#define MAX_WORKERS 8
#define MAX_COMPONENTS_PER_NODE 64

/**
 * @brief 管理已加载组件及其动态库句柄
 */
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
    pthread_t cli_tid;
} nos_node_ctx_t;

/* 全局上下文对象声明 */
extern nos_node_ctx_t g_node_ctx;

#endif /* __NOS_NODE_PRIV_H__ */
