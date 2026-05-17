#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include "nos_timer_api.h"
#include "nos_api.h"
#include "nos_buffer.h"

#define MAX_TIMERS 1024
#define TIMER_INVALID_ID 0

typedef struct {
    uint32_t timer_id;
    uint64_t expire_at_ms;   // 绝对到期时间 (Monotonic)
    uint32_t interval_ms;
    int      is_periodic;
    uint32_t dst_service;
    uint32_t msg_code;
} nos_timer_node_t;

typedef struct {
    nos_timer_node_t *heap[MAX_TIMERS];
    uint32_t         size;
    uint32_t         next_id;
    
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    pthread_t        dispatch_tid;
    int              initialized;
} nos_timer_mgr_t;

static nos_timer_mgr_t g_timer_mgr = {
    .size = 0,
    .next_id = 1,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .initialized = 0
};

/* 获取当前单调时间 (毫秒) */
static uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* 最小堆操作：向上调整 */
static void heapify_up(uint32_t idx) {
    while (idx > 0) {
        uint32_t parent = (idx - 1) / 2;
        if (g_timer_mgr.heap[idx]->expire_at_ms >= g_timer_mgr.heap[parent]->expire_at_ms) break;
        nos_timer_node_t *tmp = g_timer_mgr.heap[idx];
        g_timer_mgr.heap[idx] = g_timer_mgr.heap[parent];
        g_timer_mgr.heap[parent] = tmp;
        idx = parent;
    }
}

/* 最小堆操作：向下调整 */
static void heapify_down(uint32_t idx) {
    while (idx * 2 + 1 < g_timer_mgr.size) {
        uint32_t smallest = idx * 2 + 1;
        uint32_t right = idx * 2 + 2;
        if (right < g_timer_mgr.size && g_timer_mgr.heap[right]->expire_at_ms < g_timer_mgr.heap[smallest]->expire_at_ms) {
            smallest = right;
        }
        if (g_timer_mgr.heap[idx]->expire_at_ms <= g_timer_mgr.heap[smallest]->expire_at_ms) break;
        nos_timer_node_t *tmp = g_timer_mgr.heap[idx];
        g_timer_mgr.heap[idx] = g_timer_mgr.heap[smallest];
        g_timer_mgr.heap[smallest] = tmp;
        idx = smallest;
    }
}

static void* nos_timer_dispatch_thread(void *arg) {
    nos_sys_log_info("Timer dispatch thread started.");
    
    pthread_mutex_lock(&g_timer_mgr.lock);
    while (1) {
        if (g_timer_mgr.size == 0) {
            pthread_cond_wait(&g_timer_mgr.cond, &g_timer_mgr.lock);
            continue;
        }

        uint64_t now = get_monotonic_ms();
        nos_timer_node_t *top = g_timer_mgr.heap[0];

        if (now >= top->expire_at_ms) {
            /* 1. 定时器到期，弹出堆顶 */
            g_timer_mgr.heap[0] = g_timer_mgr.heap[--g_timer_mgr.size];
            heapify_down(0);

            /* 2. 发送消息 (解锁期间进行，防止阻塞引擎) */
            pthread_mutex_unlock(&g_timer_mgr.lock);
            
            nos_sys_log_debug("Timer expired: sending msg_code %u to service %u", top->msg_code, top->dst_service);
            nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t), 0);
            if (buf) {
                nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
                msg->magic = NOS_IPC_MAGIC;
                msg->dst_service = top->dst_service;
                msg->src_component = 0; // 0 表示由系统定时器产生
                msg->msg_code = top->msg_code;
                msg->payload_len = 0;
                nos_status_t res = nos_service_msg_send(buf);
                nos_sys_log_debug("Timer msg sent result: %d", res);
                nos_buffer_release(buf);
            } else {
                nos_sys_log_error("Timer failed to alloc buffer");
            }

            pthread_mutex_lock(&g_timer_mgr.lock);

            /* 3. 如果是周期性的，重新计算时间并压入堆 */
            if (top->is_periodic) {
                top->expire_at_ms = get_monotonic_ms() + top->interval_ms;
                g_timer_mgr.heap[g_timer_mgr.size] = top;
                heapify_up(g_timer_mgr.size++);
            } else {
                free(top);
            }
        } else {
            /* 4. 等待至最近的一个定时器到期 */
            uint64_t wait_ms = top->expire_at_ms - now;
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += wait_ms / 1000;
            ts.tv_nsec += (wait_ms % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            
            pthread_cond_timedwait(&g_timer_mgr.cond, &g_timer_mgr.lock, &ts);
        }
    }
    return NULL;
}

static nos_status_t nos_timer_start_impl(uint32_t interval_ms, int is_periodic, uint32_t dst_service, uint32_t msg_code, uint32_t *out_timer_id) {
    pthread_mutex_lock(&g_timer_mgr.lock);
    if (g_timer_mgr.size >= MAX_TIMERS) {
        pthread_mutex_unlock(&g_timer_mgr.lock);
        return NOS_ERR_BUSY;
    }

    nos_timer_node_t *node = malloc(sizeof(nos_timer_node_t));
    node->timer_id = g_timer_mgr.next_id++;
    node->interval_ms = interval_ms;
    node->expire_at_ms = get_monotonic_ms() + interval_ms;
    node->is_periodic = is_periodic;
    node->dst_service = dst_service;
    node->msg_code = msg_code;

    g_timer_mgr.heap[g_timer_mgr.size] = node;
    heapify_up(g_timer_mgr.size++);
    
    if (out_timer_id) *out_timer_id = node->timer_id;

    /* 如果新插入的是堆顶，唤醒分发线程重新计算等待时间 */
    pthread_cond_signal(&g_timer_mgr.cond);
    pthread_mutex_unlock(&g_timer_mgr.lock);
    
    return NOS_OK;
}

static nos_status_t nos_timer_stop_impl(uint32_t timer_id) {
    pthread_mutex_lock(&g_timer_mgr.lock);
    for (uint32_t i = 0; i < g_timer_mgr.size; i++) {
        if (g_timer_mgr.heap[i]->timer_id == timer_id) {
            free(g_timer_mgr.heap[i]);
            g_timer_mgr.heap[i] = g_timer_mgr.heap[--g_timer_mgr.size];
            heapify_down(i);
            pthread_mutex_unlock(&g_timer_mgr.lock);
            return NOS_OK;
        }
    }
    pthread_mutex_unlock(&g_timer_mgr.lock);
    return NOS_ERR;
}

static nos_timer_ops_t g_timer_ops = {
    .start = nos_timer_start_impl,
    .stop = nos_timer_stop_impl
};

void nos_timer_init(void) {
    if (g_timer_mgr.initialized) return;

    /* 使用 CLOCK_MONOTONIC 属性初始化条件变量，确保 timedwait 精度 */
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_timer_mgr.cond, &attr);
    pthread_condattr_destroy(&attr);

    pthread_create(&g_timer_mgr.dispatch_tid, NULL, nos_timer_dispatch_thread, NULL);
    pthread_detach(g_timer_mgr.dispatch_tid);

    extern nos_status_t nos_embedded_service_register(const char *name, void *ops);
    nos_embedded_service_register("SVC_TIMER", &g_timer_ops);
    
    g_timer_mgr.initialized = 1;
}
