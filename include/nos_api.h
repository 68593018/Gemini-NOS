#ifndef __NOS_API_H__
#define __NOS_API_H__

#include <stdatomic.h>
#include "nos_log.h"
#include "nos_service.h"

/**
 * @brief 内部 Gate 机制：自动化服务发现桥接器
 * 
 * 设计原理：
 * 1. 采用“门面模式”隐藏底座复杂的查找逻辑。
 * 2. 使用 C11 _Atomic 确保在多线程（如多个 Worker）并发首次调用 API 时，
 *    句柄的查找与写入是内存安全的，避免 Data Race。
 * 3. 配合 static 局部变量实现一次查找、永久缓存，性能损耗极低。
 */
static inline nos_log_ops_t* _nos_get_log_ops(void) {
    /* 使用原子变量存放句柄缓存 */
    static _Atomic(nos_log_ops_t*) cache = NULL;
    
    /* 第一次读取：非阻塞获取当前值 */
    nos_log_ops_t* ops = atomic_load_explicit(&cache, memory_order_relaxed);
    
    if (__builtin_expect(ops == NULL, 0)) {
        /* 缓存未建立，执行底座服务查找 */
        ops = (nos_log_ops_t *)nos_embedded_service_get("SVC_LOG");
        
        /* 原子写入：确保后续线程能看到一致的指针值 */
        atomic_store_explicit(&cache, ops, memory_order_relaxed);
    }
    return ops;
}

/**
 * @brief 自动化日志 API
 * 
 * 优势：组件代码不需要持有任何 handle 结构，直接调用。
 * 实现：通过 _nos_get_log_ops 原子门面自动桥接到平台提供的日志引擎。
 */
#define nos_log_debug(self, fmt, ...) \
    do { nos_log_ops_t *ops = _nos_get_log_ops(); if(ops) ops->log(NOS_LOG_LEVEL_DEBUG, (self)->id, fmt, ##__VA_ARGS__); } while(0)

#define nos_log_info(self, fmt, ...) \
    do { nos_log_ops_t *ops = _nos_get_log_ops(); if(ops) ops->log(NOS_LOG_LEVEL_INFO, (self)->id, fmt, ##__VA_ARGS__); } while(0)

#define nos_log_warn(self, fmt, ...) \
    do { nos_log_ops_t *ops = _nos_get_log_ops(); if(ops) ops->log(NOS_LOG_LEVEL_WARN, (self)->id, fmt, ##__VA_ARGS__); } while(0)

#define nos_log_error(self, fmt, ...) \
    do { nos_log_ops_t *ops = _nos_get_log_ops(); if(ops) ops->log(NOS_LOG_LEVEL_ERROR, (self)->id, fmt, ##__VA_ARGS__); } while(0)

/* 可以在此处继续扩展其他嵌入式服务的 Gate，如定时器、统计等 */

#endif /* __NOS_API_H__ */
