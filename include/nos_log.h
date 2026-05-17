#ifndef __NOS_LOG_H__
#define __NOS_LOG_H__

#include "nos_types.h"

/**
 * @brief 日志级别定义
 */
typedef enum {
    NOS_LOG_LEVEL_DEBUG,
    NOS_LOG_LEVEL_INFO,
    NOS_LOG_LEVEL_WARN,
    NOS_LOG_LEVEL_ERROR
} nos_log_level_t;

/**
 * @brief 嵌入式日志服务操作接口 (API Table)
 */
typedef struct {
    /**
     * @brief 打印带级别的日志
     * @param level 级别
     * @param comp_name 调用者组件名
     * @param fmt 格式化字符串
     */
    void (*log)(nos_log_level_t level, const char *comp_name, const char *fmt, ...);
    
    /**
     * @brief 设置全局过滤级别
     */
    void (*set_filter_level)(nos_log_level_t level);

    /**
     * @brief 设置特定组件的过滤级别
     */
    void (*set_comp_level)(const char *comp_name, nos_log_level_t level);
} nos_log_ops_t;

/**
 * @brief 内部接口：初始化日志引擎
 */
void nos_log_init(void);

#endif /* __NOS_LOG_H__ */
