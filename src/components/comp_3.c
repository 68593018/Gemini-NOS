#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "nos_component.h"
#include "nos_service.h"
#include "nos_buffer.h"

static void comp_on_msg(nos_component_t *self, const nos_service_msg_t *msg) {
    const char *payload = (const char *)((uint8_t *)msg + sizeof(nos_service_msg_t));
    printf("[%s] RECEIVED: From Component %u, MsgCode %u, Payload: %s\n", 
           self->name, msg->src_component, msg->msg_code, payload);
}

nos_status_t nos_export_component(nos_component_t *comp) {
    if (!comp) return NOS_ERR;
    comp->on_msg = comp_on_msg;
    printf("[LibComp] Component '%s' (ID:%u) exported.\n", comp->name, comp->id);
    return NOS_OK;
}
