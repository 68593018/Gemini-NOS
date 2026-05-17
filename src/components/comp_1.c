#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"

#define MAX_ITERATIONS 10
static int counter = 0;

static void comp_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    const char *payload = (const char *)((uint8_t *)msg + sizeof(nos_service_msg_t));
    printf("[%s] RECEIVED: From Component %u, MsgCode %u, Payload: %s\n", 
           self->name, msg->src_component, msg->msg_code, payload);

    if (msg->msg_code == 1001) {
        printf("[%s] Auto-replying to Remote Service 101...\n", self->name);
        nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + 32, 0);
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
    
    if (msg->msg_code == 1002) {
        if (counter >= MAX_ITERATIONS) {
            printf("[%s] Reached MAX_ITERATIONS (%d). Stopping loop.\n", self->name, MAX_ITERATIONS);
            return;
        }
        sleep(1); 
        printf("[%s] Initiating next loop request (%d/%d) to Service 204...\n", 
               self->name, counter + 1, MAX_ITERATIONS);
        nos_buffer_t *buf = nos_buffer_alloc(sizeof(nos_service_msg_t) + 32, 0);
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

nos_status_t nos_export_component(nos_component_t *comp) {
    if (!comp) return NOS_ERR;
    comp->on_msg = comp_on_msg;
    printf("[LibComp] Component '%s' (ID:%u) exported.\n", comp->name, comp->id);
    return NOS_OK;
}
