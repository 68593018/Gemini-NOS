#include <stdio.h>
#include <string.h>
#include "nos_component.h"
#include "nos_service.h"

/* 声明外部组件实例 */
extern nos_component_t g_comp1;
extern nos_component_t g_comp2;

/* 简单的组件注册表 (Mock) */
static nos_component_t *g_registry[] = { &g_comp1, &g_comp2 };

/**
 * @brief 实现异步消息发送 API (Mock 版)
 * 在单线程内，它直接找到目标组件并触发回调。
 */
nos_status_t nos_service_msg_send(nos_service_msg_t *msg) {
    nos_component_t *target = NULL;

    /* 路由策略：
     * 1. 如果指定了 dst_service，查找提供该服务的组件 (本例中 ID=2 提供服务)
     * 2. 如果 dst_service 为 0，则根据 src_component 发回 (简化的点对点)
     */
    if (msg->dst_service == 2) {
        target = &g_comp2;
    } else {
        // 简单模拟发回给 Comp1
        target = &g_comp1;
    }

    if (target && target->on_msg) {
        target->on_msg(target, msg);
    }

    return NOS_OK;
}

int main() {
    printf("--- NOS Component Communication Demo ---\n");

    /* 1. 初始化各组件 */
    for (int i = 0; i < 2; i++) {
        if (g_registry[i]->init) {
            g_registry[i]->init(g_registry[i]);
        }
    }

    /* 2. 启动组件 (触发业务逻辑) */
    printf("\n--- Starting Components ---\n");
    for (int i = 0; i < 2; i++) {
        if (g_registry[i]->start) {
            g_registry[i]->start(g_registry[i]);
        }
    }

    printf("\n--- Demo Finished ---\n");
    return 0;
}
