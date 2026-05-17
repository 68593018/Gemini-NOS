#ifndef __NOS_TIMER_API_H__
#define __NOS_TIMER_API_H__

#include "nos_types.h"

/**
 * @brief 定时器回调函数定义
 */
typedef void (*nos_timer_cb_t)(void *arg);
typedef void (*nos_timer_free_arg_t)(void *arg);

/**
 * @brief 定时器操作接口 (API Table)
 */
typedef struct {
    /**
     * @brief 启动一个定时器
     * @param interval_ms 间隔 (毫秒)
     * @param is_periodic 是否周期性
     * @param cb          到期回调函数
     * @param arg         回调参数
     * @param free_arg    参数释放函数 (可选)
     * @param out_timer_id 输出生成的定时器 ID
     * @return nos_status_t 成功返回 NOS_OK
     */
    nos_status_t (*start)(uint32_t interval_ms, int is_periodic, nos_timer_cb_t cb, void *arg, nos_timer_free_arg_t free_arg, uint32_t *out_timer_id);
    
    /**
     * @brief 停止并销毁一个定时器
     */
    nos_status_t (*stop)(uint32_t timer_id);
} nos_timer_ops_t;

/**
 * @brief 公开接口：启动当前线程的定时器 (由调度器提供实现)
 */
nos_status_t nos_timer_start(uint32_t interval_ms, int is_periodic, nos_timer_cb_t cb, void *arg, nos_timer_free_arg_t free_arg, uint32_t *out_id);
nos_status_t nos_timer_stop(uint32_t timer_id);

/**
 * @brief 内部接口：初始化定时器引擎并注册服务
 */
void nos_timer_init(void);

#endif /* __NOS_TIMER_API_H__ */
