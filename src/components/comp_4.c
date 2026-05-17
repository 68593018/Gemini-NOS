#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_ids.h"

typedef struct {
    int counter;
} comp_ctx_t;

static void comp_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    comp_ctx_t *ctx = (comp_ctx_t *)self->priv;
    const char *payload = (const char *)((uint8_t *)msg + sizeof(nos_service_msg_t));
    ctx->counter++;
    printf("[%s] RECEIVED (Total: %d): From Component %u, MsgCode %u, Payload: %s\n", 
           self->name, ctx->counter, msg->src_component, msg->msg_code, payload);

    if (msg->msg_code == 1001) {
        printf("[%s] Auto-replying to MGMT Service...\n", self->name);
        nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + 32, 0);
        if (buf) {
            nos_service_msg_t *rsp = (nos_service_msg_t *)buf->data;
            rsp->magic = NOS_IPC_MAGIC;
            rsp->version = NOS_IPC_VERSION;
            rsp->dst_service = SVC_MGMT; 
            rsp->src_component = self->id;
            rsp->msg_code = 1002;
            rsp->payload_len = 32;
            sprintf((char*)(buf->data + sizeof(nos_service_msg_t)), "Reply #%d from %s", ctx->counter, self->name);
            nos_service_msg_send(buf);
        }
    }
}

static nos_status_t comp_init(nos_component_t *self) {
    self->priv = calloc(1, sizeof(comp_ctx_t));
    return NOS_OK;
}

nos_status_t nos_export_component(nos_component_t *comp) {
    if (!comp) return NOS_ERR;
    comp->on_msg = comp_on_msg;
    comp->init = comp_init;
    return NOS_OK;
}
