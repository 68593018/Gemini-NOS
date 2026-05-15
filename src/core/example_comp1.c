#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nos_component.h"
#include "nos_service.h"

/* 模拟业务消息定义 */
#define SERVICE_HELLO_ID  2
#define MSG_HELLO_REQ     1
#define MSG_HELLO_RSP     2

/* 组件 1 的私有上下文 */
typedef struct {
    int response_count;
} comp1_ctx_t;

static nos_status_t comp1_init(nos_component_t *self) {
    printf("[%s] Initializing...\n", self->name);
    self->priv = malloc(sizeof(comp1_ctx_t));
    ((comp1_ctx_t *)self->priv)->response_count = 0;
    return NOS_OK;
}

static nos_status_t comp1_start(nos_component_t *self) {
    printf("[%s] Started. Sending request to Service %d...\n", self->name, SERVICE_HELLO_ID);
    
    /* 构造请求消息 */
    size_t msg_len = sizeof(nos_service_msg_t) + 32;
    nos_service_msg_t *msg = (nos_service_msg_t *)malloc(msg_len);
    msg->dst_service = SERVICE_HELLO_ID;
    msg->src_component = self->id;
    msg->msg_code = MSG_HELLO_REQ;
    msg->tx_id = 100;
    msg->payload_len = 32;
    strcpy((char *)msg->payload, "Request from Comp1");

    /* 发送异步消息 */
    nos_service_msg_send(msg);
    free(msg);
    
    return NOS_OK;
}

static void comp1_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    comp1_ctx_t *ctx = (comp1_ctx_t *)self->priv;
    if (msg->msg_code == MSG_HELLO_RSP) {
        printf("[%s] Received response: \"%s\" (TX_ID: %u)\n", 
               self->name, (char *)msg->payload, msg->tx_id);
        ctx->response_count++;
    }
}

/* 导出组件 1 定义 */
nos_component_t g_comp1 = {
    .id = 1,
    .name = "Component-1",
    .init = comp1_init,
    .start = comp1_start,
    .on_msg = comp1_on_msg
};
