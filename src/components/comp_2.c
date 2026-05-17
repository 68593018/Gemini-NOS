#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_ids.h"

/* 组件私有上下文定义 */
typedef struct {
    int local_counter;
} comp_ctx_t;

static void comp_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    comp_ctx_t *ctx = (comp_ctx_t *)self->priv;
    const char *payload = (const char *)((uint8_t *)msg + sizeof(nos_service_msg_t));
    
    ctx->local_counter++;
    printf("[%s] RECEIVED: From Component %u, MsgCode %u, Payload: %s (Local Counter: %d)\n", 
           self->name, msg->src_component, msg->msg_code, payload, ctx->local_counter);
}

static nos_status_t comp_init(nos_component_t *self) {
    /* 每一个实例分配自己的私有内存 */
    comp_ctx_t *ctx = calloc(1, sizeof(comp_ctx_t));
    self->priv = ctx;
    printf("[LibComp] Component '%s' initialized private context.\n", self->name);
    return NOS_OK;
}

static void comp_stop(nos_component_t *self) {
    if (self->priv) {
        free(self->priv);
        self->priv = NULL;
    }
}

static nos_status_t comp_start(nos_component_t *self) {
    return NOS_OK;
}

nos_status_t nos_export_component(nos_component_t *comp) {
    if (!comp) return NOS_ERR;
    comp->on_msg = comp_on_msg;
    comp->init = comp_init;
    comp->start = comp_start;
    comp->stop = comp_stop;
    printf("[LibComp] Component '%s' (ID:%u) exported with Isolation Support.\n", comp->name, comp->id);
    return NOS_OK;
}
