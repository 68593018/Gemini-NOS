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
    int local_counter;
} comp_ctx_t;

static void comp_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    comp_ctx_t *ctx = (comp_ctx_t *)self->priv;
    ctx->local_counter++;
    if (ctx->log) ctx->log->log(NOS_LOG_LEVEL_INFO, self->name, "Msg received, counter: %d", ctx->local_counter);
}

static nos_status_t comp_init(nos_component_t *self) {
    comp_ctx_t *ctx = calloc(1, sizeof(comp_ctx_t));
    ctx->log = nos_embedded_service_get("SVC_LOG");
    self->priv = ctx;
    if (ctx->log) ctx->log->log(NOS_LOG_LEVEL_DEBUG, self->name, "Context initialized");
    return NOS_OK;
}

static void comp_stop(nos_component_t *self) {
    if (self->priv) free(self->priv);
}

static nos_status_t comp_start(nos_component_t *self) { return NOS_OK; }

nos_status_t nos_export_component(nos_component_t *comp) {
    if (!comp) return NOS_ERR;
    comp->on_msg = comp_on_msg;
    comp->init = comp_init;
    comp->start = comp_start;
    comp->stop = comp_stop;
    return NOS_OK;
}
