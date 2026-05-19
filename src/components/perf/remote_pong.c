#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"
#include "nos_ids.h"
#include "nos_api.h"

static void comp_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    if (msg->msg_code == 2001) { // PING
        /* 保持与请求相同的 Payload 长度进行回复 */
        uint32_t payload_len = msg->payload_len;
        uint32_t total_size = sizeof(nos_service_msg_t) + payload_len;
        
        nos_buffer_t *buf = NULL;
        extern nos_buffer_t* nos_shm_buffer_alloc(uint32_t size);
        if (total_size <= 4096) {
            buf = nos_shm_buffer_alloc(total_size);
        }
        if (!buf) {
            buf = nos_buffer_alloc(total_size, 0);
        }

        if (buf) {
            nos_service_msg_t *pong = (nos_service_msg_t *)buf->data;
            pong->magic = NOS_IPC_MAGIC;
            pong->dst_service = 113; // SVC_REMOTE_PING
            pong->src_component = self->id;
            pong->msg_code = 2002; // PONG
            pong->payload_len = payload_len;
            
            if (payload_len > 0) {
                memset(pong + 1, 0xBB, payload_len);
            }
            
            nos_service_msg_send(buf);
            nos_buffer_release(buf);
        }
    }
}

nos_status_t nos_export_component(nos_component_t *comp) {
    comp->on_msg = comp_on_msg;
    return NOS_OK;
}
