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

#include <unistd.h>

static nos_status_t comp1_start(nos_component_t *self) {
    printf("[%s] Started. Starting continuous interaction...\n", self->name);
    
    /* 立即发送第一条请求 */
    size_t msg_len = sizeof(nos_service_msg_t) + 32;
    nos_service_msg_t *msg = (nos_service_msg_t *)malloc(msg_len);
    msg->dst_service = SERVICE_HELLO_ID;
    msg->src_component = self->id;
    msg->msg_code = MSG_HELLO_REQ;
    msg->tx_id = 1;
    msg->payload_len = 32;
    strcpy((char *)msg->payload, "Continuous Req");
    nos_service_msg_send(msg);
    free(msg);
    
    return NOS_OK;
}

static void comp1_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    comp1_ctx_t *ctx = (comp1_ctx_t *)self->priv;
    if (msg->msg_code == MSG_HELLO_RSP) {
        printf("[%s] Received response #%d: \"%s\"\n", 
               self->name, ++ctx->response_count, (char *)msg->payload);
        
        /* 模拟定时：睡眠 1 秒后发起下一次请求 */
        /* 注意：在真实非阻塞系统中应使用定时器事件，这里为了演示持续性暂用 sleep */
        sleep(1); 
        
        size_t next_len = sizeof(nos_service_msg_t) + 32;
        nos_service_msg_t *next_msg = (nos_service_msg_t *)malloc(next_len);
        next_msg->dst_service = SERVICE_HELLO_ID;
        next_msg->src_component = self->id;
        next_msg->msg_code = MSG_HELLO_REQ;
        next_msg->tx_id = ctx->response_count + 1;
        next_msg->payload_len = 32;
        sprintf((char *)next_msg->payload, "Continuous Req #%d", ctx->response_count + 1);
        nos_service_msg_send(next_msg);
        free(next_msg);
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
