#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_ids.h"
#include "nos_api.h"

typedef struct {
    uint64_t start_time_ns;
    uint32_t target_count;
    uint32_t current_count;
    uint32_t payload_size;
} rping_ctx_t;

static uint64_t get_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void send_ping(nos_component_t *self) {
    rping_ctx_t *ctx = (rping_ctx_t *)self->priv;
    uint32_t total_size = sizeof(nos_service_msg_t) + ctx->payload_size;
    
    nos_buffer_t *buf = NULL;
    /* 尝试从 SHM 申请内存以启用零拷贝路径 */
    extern nos_buffer_t* nos_shm_buffer_alloc(uint32_t size);
    if (total_size <= 4096) {
        buf = nos_shm_buffer_alloc(total_size);
    }
    /* 如果 SHM 失败或不支持，降级到本地池 */
    if (!buf) {
        buf = nos_buffer_alloc(total_size, 0);
    }

    if (buf) {
        nos_service_msg_t *msg = (nos_service_msg_t *)buf->data;
        msg->magic = NOS_IPC_MAGIC;
        msg->dst_service = 114; // SVC_REMOTE_PONG
        msg->src_component = self->id;
        msg->msg_code = 2001; // PING
        msg->payload_len = ctx->payload_size;
        
        /* 填充垃圾数据模拟真实负载 */
        if (ctx->payload_size > 0) {
            memset(msg + 1, 0xAA, ctx->payload_size);
        }
        
        nos_service_msg_send(buf);
        nos_buffer_release(buf);
    }
}

static void comp_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    rping_ctx_t *ctx = (rping_ctx_t *)self->priv;

    if (msg->msg_code == 3001) { // START_TEST from CLI
        /* Payload 结构: [target_count (4B)] [payload_size (4B)] */
        uint32_t *args = (uint32_t*)(msg + 1);
        ctx->target_count = (msg->payload_len >= 4) ? args[0] : 10000;
        ctx->payload_size = (msg->payload_len >= 8) ? args[1] : 0;
        
        ctx->current_count = 0;
        nos_log_info(self, "Cross-Process Perf Test Started: %u iterations, Payload: %u bytes", 
                     ctx->target_count, ctx->payload_size);
        ctx->start_time_ns = get_now_ns();
        send_ping(self);
        return;
    }

    if (msg->msg_code == 2002) { // PONG
        ctx->current_count++;
        if (ctx->current_count < ctx->target_count) {
            send_ping(self);
        } else {
            uint64_t end_time = get_now_ns();
            double total_sec = (double)(end_time - ctx->start_time_ns) / 1000000000.0;
            double pps = (double)ctx->target_count / total_sec;
            double bps = (pps * (sizeof(nos_service_msg_t) + ctx->payload_size) * 8);
            
            nos_log_info(self, "Test Complete for Size %u:", ctx->payload_size);
            nos_log_info(self, "  Total Time:  %.3f s", total_sec);
            nos_log_info(self, "  Throughput:  %.0f PPS (Packets/Sec)", pps);
            nos_log_info(self, "  Bandwidth:   %.2f Mbps", bps / 1000000.0);
            nos_log_info(self, "  Avg Latency: %.2f us", (total_sec * 1000000.0) / ctx->target_count);
        }
    }
}

static nos_status_t comp_init(nos_component_t *self) {
    self->priv = calloc(1, sizeof(rping_ctx_t));
    return NOS_OK;
}

static void comp_stop(nos_component_t *self) {
    if (self->priv) free(self->priv);
}

nos_status_t nos_export_component(nos_component_t *comp) {
    comp->on_msg = comp_on_msg;
    comp->init = comp_init;
    comp->stop = comp_stop;
    return NOS_OK;
}
