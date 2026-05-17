#ifndef __NOS_TIMER_API_H__
#define __NOS_TIMER_API_H__

#include "nos_types.h"

/**
 * @brief 定时器操作接口 (API Table)
 */
typedef struct {
    /**
     * @brief 启动一个定时器
     * @param interval_ms 间隔 (毫秒)
     * @param is_periodic 是否周期性
     * @param dst_service 到期后消息发送的目标服务 ID
     * @param msg_code    到期后消息的 Code
     * @param out_timer_id 输出生成的定时器 ID
     * @return nos_status_t 成功返回 NOS_OK
     */
    nos_status_t (*start)(uint32_t interval_ms, int is_periodic, uint32_t dst_service, uint32_t msg_code, uint32_t *out_timer_id);
    
    /**
     * @brief 停止并销毁一个定时器
     */
    nos_status_t (*stop)(uint32_t timer_id);
} nos_timer_ops_t;

/**
 * @brief 内部接口：初始化定时器引擎并注册服务
 */
void nos_timer_init(void);

#endif /* __NOS_TIMER_API_H__ */
