#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_ids.h"
#include "nos_log.h"

typedef struct {
    nos_log_ops_t *log;
    int counter;
} comp_ctx_t;

static void comp_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    comp_ctx_t *ctx = (comp_ctx_t *)self->priv;
    
    if (ctx->log) ctx->log->log(NOS_LOG_LEVEL_INFO, self->name, "Received msg from Comp %u, Code %u", msg->src_component, msg->msg_code);

    if (msg->msg_code == 1001) {
        if (ctx->log) ctx->log->log(NOS_LOG_LEVEL_INFO, self->name, "Replying to MGMT Service");
        nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + 32, 0);
        if (buf) {
            nos_service_msg_t *rsp = (nos_service_msg_t *)buf->data;
            rsp->magic = NOS_IPC_MAGIC; rsp->dst_service = SVC_MGMT; 
            rsp->src_component = self->id; rsp->msg_code = 1002;
            nos_service_msg_send(buf);
        }
    }
}

static nos_status_t comp_init(nos_component_t *self) {
    comp_ctx_t *ctx = calloc(1, sizeof(comp_ctx_t));
    ctx->log = nos_embedded_service_get("SVC_LOG");
    self->priv = ctx;
    if (ctx->log) ctx->log->log(NOS_LOG_LEVEL_DEBUG, self->name, "Initialized with embedded log service");
    return NOS_OK;
}

static nos_status_t comp_start(nos_component_t *self) {
    return NOS_OK;
}

nos_status_t nos_export_component(nos_component_t *comp) {
    if (!comp) return NOS_ERR;
    comp->on_msg = comp_on_msg;
    comp->init = comp_init;
    comp->start = comp_start;
    return NOS_OK;
}
