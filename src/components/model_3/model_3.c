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
}

static nos_status_t comp_init(nos_component_t *self) {
    self->priv = calloc(1, sizeof(comp_ctx_t));
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
