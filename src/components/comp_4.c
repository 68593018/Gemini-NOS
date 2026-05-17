#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"

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
}

nos_status_t nos_export_component(nos_component_t *comp) {
    if (!comp) return NOS_ERR;
    comp->on_msg = comp_on_msg;
    printf("[LibComp] Component '%s' (ID:%u) exported.\n", comp->name, comp->id);
    return NOS_OK;
}
