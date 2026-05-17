#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_ids.h"
#include "nos_api.h"

typedef struct {
    nos_timer_t *heartbeat_timer;
} comp_ctx_t;

static void comp_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    /* 1. 处理来自定时器的到期事件 (Code 999) */
    if (msg->msg_code == 999) {
        nos_log_debug(self, "Timer triggered: sending heartbeat to SVC_ROUTING_V4");
        nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t), 0);
        if (buf) {
            nos_service_msg_t *hb = (nos_service_msg_t *)buf->data;
            hb->magic = NOS_IPC_MAGIC;
            hb->dst_service = SVC_ROUTING_V4; // 发送给 Comp-2
            hb->src_component = self->id;
            hb->msg_code = 1001;
            hb->payload_len = 0;
            nos_service_msg_send(buf);
            nos_buffer_release(buf);
        }
        return;
    }

    /* 2. 处理原有业务消息 */
    nos_log_info(self, "Received msg from Comp %u, Code %u", msg->src_component, msg->msg_code);

    if (msg->msg_code == 1001) {
        nos_log_info(self, "Replying to MGMT Service");
        nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + 32, 0);
        if (buf) {
            nos_service_msg_t *rsp = (nos_service_msg_t *)buf->data;
            rsp->magic = NOS_IPC_MAGIC; rsp->dst_service = SVC_MGMT; 
            rsp->src_component = self->id; rsp->msg_code = 1002;
            nos_service_msg_send(buf);
            nos_buffer_release(buf);
        }
    }
}

static nos_status_t comp_init(nos_component_t *self) {
    self->priv = calloc(1, sizeof(comp_ctx_t));
    nos_log_debug(self, "Initialized via Auto-Injection API");
    return NOS_OK;
}

static nos_status_t comp_start(nos_component_t *self) {
    comp_ctx_t *ctx = (comp_ctx_t *)self->priv;
    /* 1. 创建定时器对象，绑定到 Code 999 消息 */
    ctx->heartbeat_timer = nos_timer_create_auto(self, 999);
    
    /* 2. 启动定时器 */
    if (ctx->heartbeat_timer) {
        nos_timer_start_auto(self, ctx->heartbeat_timer, 3000, 1);
        nos_log_info(self, "Heartbeat timer object created and started (3s)");
    }
    return NOS_OK;
}

static void comp_stop(nos_component_t *self) {
    comp_ctx_t *ctx = (comp_ctx_t *)self->priv;
    if (ctx && ctx->heartbeat_timer) {
        nos_timer_delete_auto(ctx->heartbeat_timer);
        ctx->heartbeat_timer = NULL;
    }
    if (self->priv) free(self->priv);
}

nos_status_t nos_export_component(nos_component_t *comp) {
    if (!comp) return NOS_ERR;
    comp->on_msg = comp_on_msg;
    comp->init = comp_init;
    comp->start = comp_start;
    comp->stop = comp_stop;
    return NOS_OK;
}
