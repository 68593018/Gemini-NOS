#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "nos_scheduler.h"
#include "nos_service.h"

#define MAX_QUEUE_SIZE 1024

/* 内部消息队列结构 */
static nos_service_msg_t *g_msg_queue[MAX_QUEUE_SIZE];
static int g_head = 0;
static int g_tail = 0;
static bool g_keep_running = true;

/* 内部组件注册表 (简化版) */
static nos_component_t *g_components[32];
static uint32_t g_comp_count = 0;

/* 实现：向队列投递消息 (异步) */
nos_status_t nos_service_msg_send(nos_service_msg_t *msg) {
    int next_tail = (g_tail + 1) % MAX_QUEUE_SIZE;
    if (next_tail == g_head) {
        printf("[Scheduler] Error: Message queue full!\n");
        return NOS_ERR_BUSY;
    }

    /* 拷贝消息到队列 (深度拷贝以模拟真实异步) */
    size_t full_len = sizeof(nos_service_msg_t) + msg->payload_len;
    nos_service_msg_t *new_msg = (nos_service_msg_t *)malloc(full_len);
    memcpy(new_msg, msg, full_len);

    g_msg_queue[g_tail] = new_msg;
    g_tail = next_tail;
    return NOS_OK;
}

/* 实现：注册组件 */
nos_status_t nos_scheduler_register_component(uint32_t thread_id, nos_component_t *comp) {
    if (g_comp_count >= 32) return NOS_ERR;
    g_components[g_comp_count++] = comp;
    return NOS_OK;
}

/* 实现：路由与分发消息 */
static void dispatch_message(nos_service_msg_t *msg) {
    nos_component_t *target = NULL;

    /* 简单的服务路由逻辑 */
    for (uint32_t i = 0; i < g_comp_count; i++) {
        // 匹配逻辑：如果指定了服务 ID 且组件 ID 匹配 (本例简化) 
        // 或者直接根据目标组件 ID 匹配 (用于 Rsp)
        if ((msg->dst_service != 0 && g_components[i]->id == msg->dst_service) || 
            (msg->dst_service == 0 && g_components[i]->id == 1)) { // 强制回包给 1
            target = g_components[i];
            break;
        }
    }

    if (target && target->on_msg) {
        target->on_msg(target, msg);
    }

    free(msg); // 处理完后释放内存
}

/* 实现：核心调度循环 */
nos_status_t nos_scheduler_run_loop(nos_thread_t *self) {
    printf("[Scheduler] Thread '%s' loop started.\n", self->name);
    
    int processed_count = 0;
    while (g_keep_running) {
        /* 1. 检查队列是否有消息 */
        if (g_head != g_tail) {
            nos_service_msg_t *msg = g_msg_queue[g_head];
            g_head = (g_head + 1) % MAX_QUEUE_SIZE;

            /* 2. 分发消息给组件 */
            dispatch_message(msg);
            processed_count++;
        } else {
            /* 3. 队列为空，简单退出模拟 (真实系统会在这里 epoll_wait) */
            if (processed_count > 0) {
                 printf("[Scheduler] No more messages, finishing demo loop.\n");
                 g_keep_running = false;
            }
        }
    }
    return NOS_OK;
}
