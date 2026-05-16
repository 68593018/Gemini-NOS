#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nos_component.h"
#include "nos_service.h"

static void generic_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    printf("[%s] RECEIVED: From Component %u, MsgCode %u, Payload: %s\n", 
           self->name, msg->src_component, msg->msg_code, (char*)msg->payload);

    /* 业务逻辑：如果是跨进程请求 (1001)，则回包 */
    if (msg->msg_code == 1001) {
        printf("[%s] Auto-replying to Remote Service 101...\n", self->name);
        size_t len = sizeof(nos_service_msg_t) + 32;
        nos_service_msg_t *rsp = malloc(len);
        rsp->dst_service = 101; 
        rsp->src_component = self->id;
        rsp->msg_code = 1002;
        rsp->payload_len = 32;
        sprintf((char*)rsp->payload, "Reply from %s", self->name);
        nos_service_msg_send(rsp);
        free(rsp);
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
