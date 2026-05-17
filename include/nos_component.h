#ifndef __NOS_COMPONENT_H__
#define __NOS_COMPONENT_H__

#include "nos_types.h"

/* 前向声明消息结构 */
struct nos_service_msg_s;

/**
 * @brief 组件对象结构体
 */
typedef struct nos_component_s {
    uint32_t id;                /**< 组件全局唯一 ID */
    const char *name;           /**< 组件名称 */
    void *priv;                 /**< 组件私有数据上下文 */

    /**
     * @brief 生命周期回调：初始化
     * 此时应完成私有内存分配、嵌入式服务获取。
     */
    nos_status_t (*init)(struct nos_component_s *self);

    /**
     * @brief 生命周期回调：启动
     * 此时组件进入工作状态，可订阅远程服务状态或启动定时器。
     */
    nos_status_t (*start)(struct nos_component_s *self);

    /**
     * @brief 生命周期回调：停止
     * 此时应释放资源，停止所有挂起的任务。
     */
    void (*stop)(struct nos_component_s *self);

    /**
     * @brief 异步消息处理回调 (远程服务)
     * 当收到发往该组件的消息或该组件关注的服务响应时调用。
     */
    void (*on_msg)(struct nos_component_s *self, const struct nos_service_msg_s *msg);

    /**
     * @brief 事件处理回调 (定时器、Socket 等)
     */
    void (*on_event)(struct nos_component_s *self, uint32_t event_id, void *event_data);

} nos_component_t;

/**
 * @brief 组件导出函数类型
 * 组件 .so 必须实现一个名为 "nos_export_component" 的函数。
 * 平台进程在 dlopen 后通过 dlsym 调用此函数。
 */
typedef nos_status_t (*nos_comp_export_func_t)(nos_component_t *comp);

#endif /* __NOS_COMPONENT_H__ */
