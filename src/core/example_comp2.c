#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nos_component.h"
#include "nos_service.h"

#define SERVICE_HELLO_ID  2
#define MSG_HELLO_REQ     1
#define MSG_HELLO_RSP     2

static nos_status_t comp2_init(nos_component_t *self) {
    printf("[%s] Initializing (Service Provider for ID %d)...\n", self->name, SERVICE_HELLO_ID);
    return NOS_OK;
}

static void comp2_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    if (msg->msg_code == MSG_HELLO_REQ) {
        printf("[%s] Received request from Component %u. Processing...\n", self->name, msg->src_component);
        
        /* 构造回包 */
        size_t rsp_len = sizeof(nos_service_msg_t) + 32;
        nos_service_msg_t *rsp = (nos_service_msg_t *)malloc(rsp_len);
        
        /* 寻址：发回给原发送者 */
        rsp->dst_service = 0; // 点对点不需要服务 ID
        rsp->src_component = self->id;
        rsp->msg_code = MSG_HELLO_RSP;
        rsp->tx_id = msg->tx_id; // 保持 TX_ID 一致
        rsp->payload_len = 32;
        strcpy((char *)rsp->payload, "Hello from Component 2");

        /* 发送响应 */
        nos_service_msg_send(rsp);
        free(rsp);
    }
}

/* 导出组件 2 定义 */
nos_component_t g_comp2 = {
    .id = 2,
    .name = "Component-2",
    .init = comp2_init,
    .on_msg = comp2_on_msg
};
