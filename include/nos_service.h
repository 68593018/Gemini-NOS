#ifndef __NOS_SERVICE_H__
#define __NOS_SERVICE_H__

#include "nos_types.h"

/**
 * @brief 远程服务消息信封 (Message Envelope)
 */
typedef struct nos_service_msg_s {
    uint32_t dst_service;      /**< 目标服务 ID */
    uint32_t src_component;    /**< 发送者组件 ID */
    uint32_t msg_code;         /**< 业务操作码 */
    uint32_t tx_id;            /**< 事务 ID，用于关联 Req/Rsp */
    uint32_t payload_len;      /**< 载荷长度 */
    uint8_t  payload[0];       /**< 柔性数组，指向具体的业务数据结构 */
} nos_service_msg_t;

/**
 * @brief 嵌入式服务接口列表项
 */
typedef struct {
    const char *service_name;  /**< 服务名称 */
    void *ops;                 /**< 服务函数表指针 (由具体服务定义强转) */
} nos_embedded_service_t;

/**
 * @brief 远程服务 API (由系统总线提供)
 */
nos_status_t nos_service_msg_send(nos_service_msg_t *msg);

/**
 * @brief 注册远端服务路由信息
 * @param service_id 服务 ID
 * @param uds_path 目标进程监听的 UDS 路径
 */
nos_status_t nos_service_register_remote(uint32_t service_id, const char *uds_path);

/**
 * @brief 嵌入式服务获取接口
 * @param name 服务名称
 * @return void* 返回服务操作表指针，未找到返回 NULL
 */
void* nos_embedded_service_get(const char *name);

#endif /* __NOS_SERVICE_H__ */
