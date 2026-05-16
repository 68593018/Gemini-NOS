#ifndef __NOS_MANIFEST_H__
#define __NOS_MANIFEST_H__

#include "nos_types.h"

/**
 * @brief 节点（进程）定义
 */
typedef struct {
    const char *name;       /**< 进程名，用于启动匹配 */
    const char *uds_path;   /**< 该进程监听的 UDS 路径 */
    uint32_t comp_ids[16];  /**< 分配给该进程的组件 ID 列表 (0 结束) */
} nos_node_def_t;

/**
 * @brief 全局服务拓扑定义
 */
typedef struct {
    uint32_t service_id;    /**< 服务 ID */
    const char *node_name;  /**< 该服务所在的进程名 */
    uint32_t provider_comp_id; /**< 提供该服务的组件 ID */
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
