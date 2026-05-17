#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_ids.h"
#include "nos_api.h"

typedef struct {
    nos_kv_table_t *state_table;
    int local_counter;
} comp_ctx_t;

static void comp_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    comp_ctx_t *ctx = (comp_ctx_t *)self->priv;
    if (!ctx) return;
    
    ctx->local_counter++;
    if (ctx->state_table) {
        nos_kv_put_auto(ctx->state_table, self->name, &ctx->local_counter, sizeof(int));
    }
    nos_log_info(self, "Msg received, counter updated to: %d (Stateless Sync)", ctx->local_counter);
}

static nos_status_t comp_init(nos_component_t *self) {
    comp_ctx_t *ctx = calloc(1, sizeof(comp_ctx_t));
    if (!ctx) return NOS_ERR;
    self->priv = ctx;

    ctx->state_table = nos_kv_table_create_auto("CompStates", 32, sizeof(int), 128);
    if (ctx->state_table) {
        uint32_t len = sizeof(int);
        if (nos_kv_get_auto(ctx->state_table, self->name, &ctx->local_counter, &len) == NOS_OK) {
            nos_log_info(self, "State recovered from KV DB: counter = %d", ctx->local_counter);
        }
    }
    nos_log_debug(self, "Component instance isolated and initialized");
    return NOS_OK;
}

static nos_status_t comp_start(nos_component_t *self) { return NOS_OK; }

static void comp_stop(nos_component_t *self) {
    if (self->priv) { free(self->priv); self->priv = NULL; }
}

nos_status_t nos_export_component(nos_component_t *comp) {
    if (!comp) return NOS_ERR;
    comp->on_msg = comp_on_msg;
    comp->init = comp_init;
    comp->start = comp_start;
    comp->stop = comp_stop;
    return NOS_OK;
}
