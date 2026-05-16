#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"

#define MAX_ITERATIONS 10

static void generic_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    static int counter = 0;
    
    /* 
     * 在零拷贝架构中，payload 紧跟在 msg 头部后面。
     * msg 本身指向的是 nos_buffer_t->data。
     */
    const char *payload = (const char *)((uint8_t *)msg + sizeof(nos_service_msg_t));

    printf("[%s] RECEIVED: From Component %u, MsgCode %u, Payload: %s\n", 
           self->name, msg->src_component, msg->msg_code, payload);

    /* 业务逻辑 1：如果是跨进程请求 (1001)，则回包 */
    if (msg->msg_code == 1001) {
        printf("[%s] Auto-replying to Remote Service 101...\n", self->name);
        
        nos_buffer_t *buf = nos_buffer_alloc();
        if (buf) {
            nos_service_msg_t *rsp = (nos_service_msg_t *)buf->data;
            rsp->magic = NOS_IPC_MAGIC;
            rsp->version = NOS_IPC_VERSION;
            rsp->dst_service = 101; 
            rsp->src_component = self->id;
            rsp->msg_code = 1002;
            rsp->payload_len = 32;
            sprintf((char*)(buf->data + sizeof(nos_service_msg_t)), "Reply #%d from %s", ++counter, self->name);
            nos_service_msg_send(buf);
        }
    }
    
    /* 业务逻辑 2：如果是跨进程响应 (1002)，在限制次数内发起下一次请求 */
    if (msg->msg_code == 1002) {
        if (counter >= MAX_ITERATIONS) {
            printf("[%s] Reached MAX_ITERATIONS (%d). Stopping loop.\n", self->name, MAX_ITERATIONS);
            return;
        }
        sleep(1); 
        printf("[%s] Initiating next loop request (%d/%d) to Service 204...\n", 
               self->name, counter + 1, MAX_ITERATIONS);
        
        nos_buffer_t *buf = nos_buffer_alloc();
        if (buf) {
            nos_service_msg_t *next_req = (nos_service_msg_t *)buf->data;
            next_req->magic = NOS_IPC_MAGIC;
            next_req->version = NOS_IPC_VERSION;
            next_req->dst_service = 204;
            next_req->src_component = self->id;
            next_req->msg_code = 1001;
            next_req->payload_len = 32;
            sprintf((char*)(buf->data + sizeof(nos_service_msg_t)), "Loop Req #%d", ++counter);
            nos_service_msg_send(buf);
        }
    }
}

static nos_component_t g_comp1 = { .id = 1, .name = "Comp-1", .on_msg = generic_on_msg };
static nos_component_t g_comp2 = { .id = 2, .name = "Comp-2", .on_msg = generic_on_msg };
static nos_component_t g_comp3 = { .id = 3, .name = "Comp-3", .on_msg = generic_on_msg };
static nos_component_t g_comp4 = { .id = 4, .name = "Comp-4", .on_msg = generic_on_msg };
static nos_component_t g_comp5 = { .id = 5, .name = "Comp-5", .on_msg = generic_on_msg };

static nos_component_t* g_registry[] = { &g_comp1, &g_comp2, &g_comp3, &g_comp4, &g_comp5 };

nos_component_t* nos_get_component_by_id(uint32_t id) {
    for (size_t i = 0; i < sizeof(g_registry)/sizeof(g_registry[0]); i++) {
        if (g_registry[i]->id == id) {
            return g_registry[i];
        }
    }
    return NULL;
}
