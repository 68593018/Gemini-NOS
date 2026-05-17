#ifndef __NOS_API_H__
#define __NOS_API_H__

#include <stdatomic.h>
#include "nos_types.h"
#include "nos_service.h"
#include "nos_log_api.h"
#include "nos_kv_api.h"
#include "nos_timer_api.h"

/**
 * @brief 内部接口：获取 Timer 操作句柄
 */
static inline nos_timer_ops_t* _nos_get_timer_ops(void) {
    extern void* nos_embedded_service_get(const char *name);
    static _Atomic(nos_timer_ops_t*) cache = NULL;
    
    nos_timer_ops_t* ops = atomic_load_explicit(&cache, memory_order_relaxed);
    if (!ops) {
        ops = (nos_timer_ops_t *)nos_embedded_service_get("SVC_TIMER");
        if (ops) {
            atomic_store_explicit(&cache, ops, memory_order_relaxed);
        }
    }
    return ops;
}

/**
 * @brief 内部辅助：定时器到期后转发消息的结构
 */
typedef struct {
    uint32_t dst_svc;
    uint32_t msg_code;
    uint32_t comp_id;
} nos_timer_msg_wrapper_t;

static inline void _nos_timer_msg_callback(void *arg) {
    nos_timer_msg_wrapper_t *w = (nos_timer_msg_wrapper_t *)arg;
    nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t), 0);
    if (buf) {
        nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
        msg->magic = NOS_IPC_MAGIC;
        msg->dst_service = w->dst_svc;
        msg->src_component = w->comp_id;
        msg->msg_code = w->msg_code;
        msg->payload_len = 0;
        nos_service_msg_send(buf);
        nos_buffer_release(buf);
    }
}

static inline void _nos_timer_free_wrapper(void *arg) {
    if (arg) free(arg);
}

/**
 * @brief 自动化 Timer API (集成到消息流)
 */
#define nos_timer_start_auto(dst_svc_id, interval_ms, is_periodic, code, out_id) \
    ({ \
        nos_timer_msg_wrapper_t *w = malloc(sizeof(nos_timer_msg_wrapper_t)); \
        w->dst_svc = dst_svc_id; w->msg_code = code; w->comp_id = 0; \
        nos_timer_ops_t *ops = _nos_get_timer_ops(); \
        ops ? ops->start(interval_ms, is_periodic, _nos_timer_msg_callback, w, _nos_timer_free_wrapper, out_id) : NOS_ERR; \
    })

#define nos_timer_stop_auto(timer_id) \
    ({ nos_timer_ops_t *ops = _nos_get_timer_ops(); ops ? ops->stop(timer_id) : NOS_ERR; })

/**
 * @brief 内部接口：获取 KV 操作句柄
 */
static inline nos_kv_ops_t* _nos_get_kv_ops(void) {
    extern void* nos_embedded_service_get(const char *name);
    static _Atomic(nos_kv_ops_t*) cache = NULL;
    
    nos_kv_ops_t* ops = atomic_load_explicit(&cache, memory_order_relaxed);
    if (!ops) {
        ops = (nos_kv_ops_t *)nos_embedded_service_get("SVC_KV_DB");
        if (ops) {
            atomic_store_explicit(&cache, ops, memory_order_relaxed);
        }
    }
    return ops;
}

/**
 * @brief 自动化 KV API
 */
#define nos_kv_table_create_auto(name, key_size, max_val, cap) \
    ({ nos_kv_ops_t *ops = _nos_get_kv_ops(); ops ? ops->table_create(name, key_size, max_val, cap) : NULL; })

#define nos_kv_put_auto(table, key, val, len) \
    ({ nos_kv_ops_t *ops = _nos_get_kv_ops(); ops ? ops->put(table, key, val, len) : NOS_ERR; })

#define nos_kv_get_auto(table, key, val, len) \
    ({ nos_kv_ops_t *ops = _nos_get_kv_ops(); ops ? ops->get(table, key, val, len) : NOS_ERR; })

#define nos_kv_get_ptr_auto(table, key, ptr, len, handle) \
    ({ nos_kv_ops_t *ops = _nos_get_kv_ops(); ops ? ops->get_ptr(table, key, ptr, len, handle) : NOS_ERR; })

#define nos_kv_release_ptr_auto(handle) \
    do { nos_kv_ops_t *ops = _nos_get_kv_ops(); if(ops) ops->release_ptr(handle); } while(0)

#define nos_kv_del_auto(table, key) \
    ({ nos_kv_ops_t *ops = _nos_get_kv_ops(); ops ? ops->del(table, key) : NOS_ERR; })

/**
 * @brief 自动化 API 门面 (Facade)
 * 
 * nos_api.h 作为组件开发者的统一入口。
 */

/* 可以在此处继续扩展其他嵌入式服务的 Gate，如定时器、统计等 */

#endif /* __NOS_API_H__ */
