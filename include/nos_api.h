#ifndef __NOS_API_H__
#define __NOS_API_H__

#include "nos_log.h"
#include "nos_service.h"

/**
 * @brief 内部 Gate 机制：自动获取并缓存日志服务句柄
 */
static inline nos_log_ops_t* _nos_get_log_ops(void) {
    static nos_log_ops_t *cache = NULL;
    if (__builtin_expect(cache == NULL, 0)) {
        cache = (nos_log_ops_t *)nos_embedded_service_get("SVC_LOG");
    }
    return cache;
}

/**
 * @brief 自动化日志 API
 * 组件无需声明句柄，直接调用。
 */
#define nos_log_debug(comp_name, fmt, ...) \
    do { nos_log_ops_t *ops = _nos_get_log_ops(); if(ops) ops->log(NOS_LOG_LEVEL_DEBUG, comp_name, fmt, ##__VA_ARGS__); } while(0)

#define nos_log_info(comp_name, fmt, ...) \
    do { nos_log_ops_t *ops = _nos_get_log_ops(); if(ops) ops->log(NOS_LOG_LEVEL_INFO, comp_name, fmt, ##__VA_ARGS__); } while(0)

#define nos_log_warn(comp_name, fmt, ...) \
    do { nos_log_ops_t *ops = _nos_get_log_ops(); if(ops) ops->log(NOS_LOG_LEVEL_WARN, comp_name, fmt, ##__VA_ARGS__); } while(0)

#define nos_log_error(comp_name, fmt, ...) \
    do { nos_log_ops_t *ops = _nos_get_log_ops(); if(ops) ops->log(NOS_LOG_LEVEL_ERROR, comp_name, fmt, ##__VA_ARGS__); } while(0)

/* 可以在此处继续扩展其他嵌入式服务的 Gate，如定时器、统计等 */

#endif /* __NOS_API_H__ */
