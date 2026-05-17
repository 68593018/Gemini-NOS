#ifndef __NOS_MANIFEST_H__
#define __NOS_MANIFEST_H__

#include "nos_types.h"

/**
 * @brief 线程（调度器实例）定义
 */
typedef struct {
    const char *name;       /**< 线程名称 */
    uint32_t comp_ids[16];  /**< 分配给该线程的组件 ID 列表 (0 结束) */
    const char *comp_names[16]; /**< 组件名称列表 (用于动态加载) */
    const char *comp_models[16]; /**< 组件模型对应的库文件名 (用于 dlopen) */
} nos_thread_def_t;

/**
 * @brief 缓冲区池规格定义
 */
typedef struct {
    uint32_t chunk_size;    /**< 块大小 */
    uint32_t chunk_count;   /**< 块数量 */
} nos_buffer_pool_def_t;

/**
 * @brief 节点（进程）定义
 */
typedef struct {
    const char *name;       /**< 进程名 */
    const char *uds_path;   /**< 进程监听的 UDS 路径 */
    const nos_buffer_pool_def_t *buffer_pools; /**< 指向缓冲区池配置列表 (chunk_size 为 0 结束) */
    nos_thread_def_t threads[8]; /**< 进程内的线程列表 (空名字结束) */
} nos_node_def_t;

/**
 * @brief 服务运行状态 (预留)
 */
typedef enum {
    NOS_SVC_ST_UNKNOWN,
    NOS_SVC_ST_ACTIVE,
    NOS_SVC_ST_INACTIVE,
    NOS_SVC_ST_ERROR
} nos_service_status_t;

/**
 * @brief 服务定义
 */
typedef struct {
    uint32_t service_id;    /**< 服务逻辑 ID */
    const char *node_name;  /**< 提供该服务的节点名称 */
    uint32_t provider_comp_id; /**< 提供该服务的组件 ID */
    nos_service_status_t status; /**< 服务当前状态 (预留) */
} nos_service_def_t;


/**
 * @brief 获取节点定义
 */
const nos_node_def_t* nos_manifest_get_node(const char *node_name);

/**
 * @brief 获取所有服务定义
 */
const nos_service_def_t* nos_manifest_get_services(uint32_t *count);

#endif /* __NOS_MANIFEST_H__ */
