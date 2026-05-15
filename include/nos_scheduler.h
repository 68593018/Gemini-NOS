#ifndef __NOS_SCHEDULER_H__
#define __NOS_SCHEDULER_H__

#include "nos_component.h"

/**
 * @brief 调度器线程对象
 */
typedef struct nos_thread_s {
    uint32_t thread_id;         /**< 逻辑线程 ID */
    const char *name;           /**< 线程名称 */
    
    // 内部管理的组件列表 (简化表示)
    nos_component_t **components;
    uint32_t component_count;

    /**
     * @brief 启动调度循环 (阻塞直到线程停止)
     */
    nos_status_t (*run_loop)(struct nos_thread_s *self);

} nos_thread_t;

/**
 * @brief 向指定线程注册一个组件
 */
nos_status_t nos_scheduler_register_component(uint32_t thread_id, nos_component_t *comp);

#endif /* __NOS_SCHEDULER_H__ */
