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
    nos_kv_table_t *state_table;
    int local_counter;
} comp_ctx_t;

static void comp_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    comp_ctx_t *ctx = (comp_ctx_t *)self->priv;
    ctx->local_counter++;
    
    /* 同步状态到 KV 数据库 */
    nos_kv_put_auto(ctx->state_table, self->name, &ctx->local_counter, sizeof(int));
    
    nos_log_info(self, "Msg received, counter updated to: %d (Stateless Sync)", ctx->local_counter);
}

static nos_status_t comp_init(nos_component_t *self) {
    comp_ctx_t *ctx = calloc(1, sizeof(comp_ctx_t));
    self->priv = ctx;

    /* 
     * 1. 尝试创建或打开全局状态表 
     * 注意：在单进程原型中，table_create 如果表名已存在应返回现有句柄，此处简化处理
     */
    static nos_kv_table_t *g_comp_states = NULL;
    if (!g_comp_states) {
        g_comp_states = nos_kv_table_create_auto("CompStates", 32, sizeof(int), 128);
    }
    ctx->state_table = g_comp_states;

    /* 2. 尝试从 KV 数据库恢复状态 (基于组件名称作为 Key) */
    uint32_t len = sizeof(int);
    if (nos_kv_get_auto(ctx->state_table, self->name, &ctx->local_counter, &len) == NOS_OK) {
        nos_log_info(self, "State recovered from KV DB: counter = %d", ctx->local_counter);
    } else {
        nos_log_debug(self, "No existing state found, starting from 0");
    }

    nos_log_debug(self, "Component initialized with stateless capability");
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
